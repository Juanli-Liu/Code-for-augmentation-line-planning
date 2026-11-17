"""Historical regression requiring the retired --solve-master application CLI.
Use scripts/run_baseline.ps1 for the current C++ acceptance baseline.
"""

import argparse
import csv
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from plot_existing_timetable import derive_visit_times


ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "augmentation line plan" / "Data"


def read_rows(directory, name):
    with (directory / name).open(encoding="utf-8-sig", newline="") as file:
        return list(csv.DictReader(file))


def write_rows(directory, name, rows):
    with (directory / name).open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def make_case(workspace, name, ods, line_ids=("1",), frequency="1", turnaround="15"):
    directory = workspace / name
    directory.mkdir()
    for source in DATA.glob("*.csv"):
        shutil.copyfile(source, directory / source.name)

    for filename in ("lines.csv", "line_allowed_platforms.csv"):
        rows = read_rows(directory, filename)
        write_rows(directory, filename, [row for row in rows if row["line_id"] in line_ids])
    write_rows(directory, "ods.csv", [
        dict(od_id=10 + index * 7, origin_station=origin, destination_station=destination, demand=demand)
        for index, (origin, destination, demand) in enumerate(ods)
    ])
    changes = {"long_available_units": "1", "short_available_units": "0",
               "candidate_frequencies": frequency, "long_minimum_turnaround_time": turnaround}
    rows = read_rows(directory, "parameters.csv")
    for row in rows:
        if row["parameter"] in changes:
            row["value"] = changes[row["parameter"]]
    write_rows(directory, "parameters.csv", rows)
    return directory


def check_solution(executable, directory, objective, unmet_by_od):
    result = subprocess.run([executable, "--solve-master", directory], capture_output=True,
                            encoding="utf-8", timeout=60, check=False)
    report = result.stdout
    assert result.returncode == 0 and "主问题状态=最优" in report, (directory, report, result.stderr)
    # 核对正式调用链确实将主问题选中候选传入时刻表数据。
    selected = {int(index) for index in re.findall(r"^选中候选 (\d+) ", report, re.MULTILINE)}
    timetable_selection = re.search(r"^选中候选：(.*)$", report, re.MULTILINE)
    assert timetable_selection and "时刻表数据（编号均为内部编号）" in report, report
    assert set(map(int, re.findall(r"\d+", timetable_selection.group(1)))) == selected, report
    expected_train_count = len(read_rows(directory, "timetable.csv")) + sum(
        int(frequency) for frequency in re.findall(r"^选中候选 \d+ .* frequency=(\d+) ", report, re.MULTILINE)
    )
    train_count = re.search(r"^列车=(\d+)，访问=", report, re.MULTILINE)
    assert train_count and int(train_count.group(1)) == expected_train_count, report
    costs = re.search(r"目标值=([^，]+)，旅客总车内时间=([^，]+)，未满足需求惩罚=([^（]+)", report)
    assert costs, report
    total, travel, penalty = map(float, costs.groups())
    assert abs(total - objective) < 1e-6 and abs(total - travel - penalty) < 1e-6, report
    actual_unmet = {}
    for match in re.finditer(r"^OD (\d+) \S+ 需求=(\S+) 未满足=(\S+)(.*)$", report, re.MULTILINE):
        od_id, demand, unmet, allocations = match.groups()
        served = sum(map(float, re.findall(r"候选\d+=(\S+)", allocations)))
        assert abs(served + float(unmet) - float(demand)) < 1e-6, match.group()
        actual_unmet[int(od_id)] = float(unmet)
    assert actual_unmet.keys() == unmet_by_od.keys(), report
    assert all(abs(actual_unmet[key] - value) < 1e-6 for key, value in unmet_by_od.items()), report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    executable = parser.parse_args().executable.resolve(strict=True)
    workspace = ROOT / "tmp" / f"master_problem_cases_{uuid.uuid4().hex}"
    workspace.mkdir(parents=True)

    check_solution(executable, DATA, 144900, dict.fromkeys(range(6), 0))

    # Line 1 stops at A, C, E. Its two legs take 33 and 37, and the full trip takes 74.
    disjoint = make_case(workspace, "seat_reuse", [("A", "C", 550), ("C", "E", 550)])
    check_solution(executable, disjoint, 550 * (33 + 37), {10: 0, 17: 0})
    overlap = make_case(workspace, "seat_overlap", [("A", "C", 550), ("A", "E", 550)])
    check_solution(executable, overlap, 550 * (330 + 74), {10: 550, 17: 0})

    # A->B has four existing trains and only one slot left for the two lines together.
    section = make_case(workspace, "shared_section", [("A", "C", 550), ("A", "E", 550)],
                        line_ids=("1", "6"))
    rows = read_rows(section, "sections.csv")
    for row in rows:
        if (row["from_station"], row["to_station"]) == ("A", "B"):
            row["maximum_train_count"] = "5"
    write_rows(section, "sections.csv", rows)
    check_solution(executable, section, 550 * (330 + 65), {10: 550, 17: 0})

    platform = make_case(workspace, "no_platform_time", [("A", "E", 550)])
    terminal_occupation = next(
        int(row["value"]) for row in read_rows(platform, "parameters.csv")
        if row["parameter"] == "minimum_terminal_platform_occupation_time"
    )
    existing = {"t1": 0, "t3": 0}
    for row in read_rows(platform, "timetable.csv"):
        paths = row["Paths"].split(";")
        times = derive_visit_times(row, terminal_occupation)
        for position in range(0, len(paths), 2):
            station, track = paths[position].rsplit("-", 1)
            if station == "A" and track in existing:
                existing[track] += times[position + 1] - times[position]
    station_a = next(row["station_id"] for row in read_rows(platform, "stations.csv")
                     if row["station_name"] == "A")
    rows = read_rows(platform, "platforms.csv")
    for row in rows:
        if row["station_id"] == station_a and row["platform_name"] in existing:
            row["total_available_time"] = str(existing[row["platform_name"]])
    write_rows(platform, "platforms.csv", rows)
    check_solution(executable, platform, 550 * 740, {10: 550})

    # Two 74-step tasks fit with a 15-step turn but exceed H=240 with a 100-step turn.
    for turnaround, objective, unmet in (("15", 1100 * 74, 0), ("100", 1100 * 740, 1100)):
        stock = make_case(workspace, f"workload_{turnaround}", [("A", "E", 1100)],
                          frequency="2", turnaround=turnaround)
        check_solution(executable, stock, objective, {10: unmet})

    print(f"PASS: 7 master problem scenarios. Fixtures retained: {workspace}")


if __name__ == "__main__":
    main()
