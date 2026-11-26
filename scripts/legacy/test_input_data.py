"""历史回归：依赖已停用的主程序 --report-input 接口，不纳入当前基线。
当前验收请使用 scripts/run_baseline.ps1；本脚本保留供查阅。
"""
import argparse
import csv
from functools import partial
from pathlib import Path
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "augmentation line plan" / "Data"


def run(executable, directory, success=True, message="", *, options=()):
    result = subprocess.run([str(executable), *options, str(directory)], capture_output=True,
                            encoding="utf-8", errors="strict")
    if (result.returncode == 0) != success:
        raise AssertionError(f"{directory.name}: exit={result.returncode}\n{result.stdout}\n{result.stderr}")
    if message and message not in result.stderr:
        raise AssertionError(f"{directory.name}: missing {message!r}: {result.stderr}")
    return result.stdout


def change(directory, filename, mutation):
    path = directory / filename
    with path.open(encoding="utf-8-sig", newline="") as source:
        reader = csv.DictReader(source)
        fields, rows = reader.fieldnames, list(reader)
    mutation(fields, rows)
    with path.open("w", encoding="utf-8-sig", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields, quoting=csv.QUOTE_ALL)
        writer.writeheader()
        writer.writerows(rows)


def change_time(row, position, value):
    times = row["Times"].split(";")
    times[position] = str(value)
    row["Times"] = ";".join(times)


def use_legacy_terminal_slots(fields, rows):
    for row in rows:
        times = row["Times"].split(";")
        row["Times"] = ";".join([str(int(times[0]) - 4), *times, str(int(times[-1]) + 4)])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--configuration", choices=["Debug", "Release"], default="Debug")
    parser.add_argument("--application", type=Path)
    parser.add_argument("--checks", type=Path)
    args = parser.parse_args()
    application = args.application or ROOT / "augmentation line plan" / "augmentation line plan" / "x64" / args.configuration / "augmentation line plan.exe"
    checks = args.checks or ROOT / "tmp" / "input_data_tests" / args.configuration / "input_data_tests.exe"
    # 主程序默认执行完整求解；加载器回归明确使用仅输入报告模式。
    run_application = partial(run, application, options=("--report-input",))
    workspace = ROOT / "tmp" / f"input_data_cases_{uuid.uuid4().hex}"
    workspace.mkdir(parents=True)
    baseline = run_application(SOURCE)
    run(checks, SOURCE)
    total = 1

    def fixture(name):
        directory = workspace / name
        directory.mkdir()
        for original in SOURCE.glob("*.csv"):
            (directory / original.name).write_bytes(original.read_bytes())
        return directory

    # 行顺序、BOM 和引号不应改变编号与报告。
    shuffled = fixture("shuffled_quoted_bom")
    for path in shuffled.glob("*.csv"):
        change(shuffled, path.name, lambda fields, rows: rows.reverse())
    assert run_application(shuffled) == baseline
    run(checks, shuffled)
    total += 1
    legacy = fixture("legacy_terminal_slots")
    change(legacy, "timetable.csv", use_legacy_terminal_slots)
    assert run_application(legacy) == baseline
    run(checks, legacy)
    total += 1
    # 不连续业务 ID 与内部编号分离，所有外键同步改写。
    remapped = fixture("sparse_ids")
    for filename, column in [
        ("stations.csv", "station_id"), ("platforms.csv", "platform_id"),
        ("sections.csv", "section_id"), ("lines.csv", "line_id"), ("ods.csv", "od_id"),
    ]:
        change(remapped, filename, lambda fields, rows, key=column: [
            row.update({key: str(100 + 7 * int(row[key]))}) for row in rows
        ])
    change(remapped, "platforms.csv", lambda fields, rows: [
        row.update(station_id=str(100 + 7 * int(row["station_id"]))) for row in rows
    ])
    change(remapped, "line_allowed_platforms.csv", lambda fields, rows: [
        row.update(line_id=str(100 + 7 * int(row["line_id"])),
                   platform_ids=";".join(str(100 + 7 * int(pid)) for pid in row["platform_ids"].split(";")))
        for row in rows
    ])
    run_application(remapped)
    run(checks, remapped)
    total += 1
    high_frequency = fixture("no_candidate_pruning")
    change(high_frequency, "parameters.csv", lambda fields, rows: [
        row.update(value="1;2;10") for row in rows if row["parameter"] == "candidate_frequencies"
    ])
    report = run_application(high_frequency)
    assert "候选方案=120" in report and "新增备选任务=520" in report
    total += 1
    quoted = fixture("quoted_special_characters")
    change(quoted, "timetable.csv", lambda fields, rows: rows[0].update(trainID='T,"100"\nservice'))
    run_application(quoted)
    total += 1

    cases = [
        ("duplicate_id", "stations.csv", lambda f, r: r.append(dict(r[0])), "station_id"),
        ("unknown_station", "platforms.csv", lambda f, r: r[0].update(station_id="9999"), "station_id"),
        ("bad_integer", "sections.csv", lambda f, r: r[0].update(pure_running_time="13x"), "pure_running_time"),
        ("nonfinite_demand", "ods.csv", lambda f, r: r[0].update(demand="nan"), "demand"),
        ("bad_unit", "parameters.csv", lambda f, r: r[0].update(unit="second"), "unit"),
        ("bad_time", "timetable.csv", lambda f, r: change_time(r[0], 1, 34), "Times"),
        ("missing_actual_time", "timetable.csv", lambda f, r: r[0].update(
            Times=r[0]["Times"].rsplit(";", 1)[0]), "Paths/Times"),
        ("early_terminal", "timetable.csv", lambda f, r: change_time(r[0], 0, 14), "超出规划时域"),
        ("late_terminal", "timetable.csv", lambda f, r: change_time(r[-1], -1, 226), "超出规划时域"),
        ("missing_terminal_parameter", "parameters.csv", lambda f, r: r.remove(next(
            row for row in r if row["parameter"] == "minimum_terminal_platform_occupation_time"
        )), "minimum_terminal_platform_occupation_time"),
        ("bad_pair", "timetable.csv", lambda f, r: r[0].update(Paths=r[0]["Paths"].replace("A-t1;A-t1", "A-t1;A-t3")), "Paths"),
        ("unknown_track", "line_allowed_platforms.csv", lambda f, r: r[0].update(platform_ids="9999;3"), "platform_ids"),
        ("track_name_mismatch", "line_allowed_platforms.csv", lambda f, r: r[0].update(platform_name="t3;t1"), "轨道"),
        ("duplicate_frequency", "parameters.csv", lambda f, r: [
            row.update(value="1;1") for row in r if row["parameter"] == "candidate_frequencies"
        ], "频率重复"),
        ("missing_header", "ods.csv", lambda f, r: (f.remove("demand"), [row.pop("demand") for row in r]), "demand"),
        ("orphan_allocation", "line_allowed_platforms.csv", lambda f, r: r[0].update(line_id="9999"), "line_allowed_platforms"),
        ("empty_list_element", "lines.csv", lambda f, r: r[0].update(stop_plan="1;;1;1;1"), "stop_plan"),
    ]
    for name, filename, mutation, expected in cases:
        directory = fixture(name)
        change(directory, filename, mutation)
        run_application(directory, False, expected)
        total += 1
    for name, payload, expected in [
        ("invalid_utf8", b"\xff", "UTF-8"),
        ("unclosed_quote", b'od_id,origin_station,destination_station,demand\n0,"A,E,1', "引号"),
    ]:
        directory = fixture(name)
        (directory / "ods.csv").write_bytes(payload)
        run_application(directory, False, expected)
        total += 1
    run_application(workspace / "missing_directory", False, "parameters.csv")
    total += 1
    print(f"PASS: {total} input pipeline scenarios ({args.configuration}).")
    print(f"Fixtures retained: {workspace}")


if __name__ == "__main__":
    main()
