"""Local-only real corridor exports. Never fill missing clocks, tracks or boundary rules."""
import argparse
import collections
import csv
import json
from pathlib import Path


def span(visits):
    values = [v[k].get('absolute_seconds') for v in visits for k in ('arrival_cell', 'departure_cell')]
    values = [v for v in values if v is not None]
    return (min(values), max(values)) if values else None


def write_csv(path, columns, rows):
    with path.open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def run(source, output):
    output.mkdir(parents=True, exist_ok=False)
    eligible = json.loads((source / 'eligible_records.json').read_text(encoding='utf-8'))
    records = json.loads((source / 'real_timetable_records.json').read_text(encoding='utf-8'))
    identity = json.loads((source / 'PRIVATE_identity_mapping.json').read_text(encoding='utf-8'))
    track_rows = json.loads((source / 'PRIVATE_explicit_track_rows.json').read_text(encoding='utf-8'))
    tracks = collections.defaultdict(set)
    for row in track_rows:
        s, t = identity['stations'].get(row['station']), identity['trains'].get(row['train'])
        if s and t:
            tracks[(s, t)].add(row['track'])
    summaries = []
    for length in (10, 15, 20, 25):
        counts = collections.Counter()
        for block in eligible:
            stations = [v['station'] for v in block['visits']]
            for i in range(len(stations) - length + 1):
                seq = tuple(stations[i:i + length])
                counts[min(seq, seq[::-1])] += 1
        route, support = counts.most_common(1)[0]
        positions = {s: i for i, s in enumerate(route)}
        # Include ALL eligible touching records, not only full end-to-end traversals.
        touching = [(b, [v for v in b['visits'] if v['station'] in positions]) for b in eligible]
        touching = [(b, v, span(v)) for b, v in touching if v and span(v)]
        unresolved = [b for b in records if b['issues'] and any(v['station'] in positions for v in b['visits'])]
        for hours in (2, 4, 8, 24):
            def select(start):
                return [(b, v, t) for b, v, t in touching if t[0] <= start + hours * 3600 and t[1] >= start]
            starts = range(0, max(t[2][1] for t in touching) + 1, 3600)
            start = max(starts, key=lambda x: (len(select(x)), -x))
            end = start + hours * 3600
            selected = select(start)
            case = output / f'corridor{length}_{hours:02d}h'
            case.mkdir()
            visits, running, missing = [], [], []
            stats = collections.Counter()
            for b, selected_visits, bounds in selected:
                stats['trains'] += 1
                stats['full_corridor_records'] += len(selected_visits) == length
                stats['crosses_time_window'] += bounds[0] < start or bounds[1] > end
                original_positions = {v['row']: i for i, v in enumerate(b['visits'])}
                for i, v in enumerate(selected_visits):
                    a, d = (v[k].get('absolute_seconds') for k in ('arrival_cell', 'departure_cell'))
                    options = tracks[(v['station'], b['train'])]
                    status = 'unique_explicit_match' if len(options) == 1 else 'conflicting_matches' if options else 'not_established'
                    stats['visits'] += 1
                    stats['track_' + status] += 1
                    stats['nonminute_events'] += sum(x is not None and x % 60 != 0 for x in (a, d))
                    stats['single_sided_visits'] += a is None or d is None
                    stats['dwell_under_240_seconds'] += a is not None and d is not None and 0 < d - a < 240
                    original = original_positions[v['row']]
                    boundary = ('source_origin' if original == 0 else 'corridor_entry') if i == 0 else ('source_destination' if original == len(b['visits']) - 1 else 'corridor_exit') if i == len(selected_visits) - 1 else 'interior'
                    visits.append(dict(train=b['train'], station=v['station'], sequence=i,
                        corridor_position=positions[v['station']], arrival_absolute_seconds=a,
                        departure_absolute_seconds=d, arrival_relative_seconds=None if a is None else a-start,
                        departure_relative_seconds=None if d is None else d-start,
                        observed_dwell_seconds=None if a is None or d is None else d-a,
                        arrival_notation=v['arrival_cell']['kind'], departure_notation=v['departure_cell']['kind'],
                        boundary=boundary, track_status=status, source_block=b['block'], source_sheet=b['sheet'],
                        source_row=v['row'], source_column=b['column']))
                    reasons = []
                    if status != 'unique_explicit_match': reasons.append('fixed_track_unconfirmed')
                    if a is None or d is None: reasons.append('missing_arrival_or_departure_requires_notation_semantics')
                    if boundary != 'interior': reasons.append('boundary_occupation_unconfirmed')
                    if reasons:
                        missing.append(dict(train=b['train'], station=v['station'], source_block=b['block'], reasons=';'.join(reasons)))
                    if i:
                        prev = selected_visits[i-1]
                        dep = prev['departure_cell'].get('absolute_seconds')
                        consecutive = original_positions[prev['row']] + 1 == original and abs(positions[prev['station']] - positions[v['station']]) == 1
                        if consecutive and dep is not None and a is not None and a > dep:
                            running.append(dict(train=b['train'], from_station=prev['station'], to_station=v['station'],
                                running_seconds=a-dep, source_block=b['block'], from_source_row=prev['row'], to_source_row=v['row']))
                        else:
                            stats['unconfirmed_section_runtimes'] += 1
            write_csv(case/'real_visits.csv', list(visits[0]), visits)
            write_csv(case/'observed_running_times.csv', ['train','from_station','to_station','running_seconds','source_block','from_source_row','to_source_row'], running)
            write_csv(case/'missing_items.csv', ['train','station','source_block','reasons'], missing)
            write_csv(case/'unresolved_source_blocks.csv', ['source_block','reasons'], [dict(source_block=b['block'], reasons=';'.join(b['issues'])) for b in unresolved])
            audit = dict(case=case.name, station_count=length, station_aliases=route,
                window_start_absolute_seconds=start, window_end_absolute_seconds=end,
                time_step_seconds=1, existing_timing_mode=1, existing_train_max_delay=0,
                candidate_minimum_stop_seconds=240, full_corridor_support_all_day=support,
                unresolved_touching_blocks_all_day=len(unresolved), observed_running_times=len(running),
                **stats, ready_for_solver=False, retimed_events=0, rounded_events=0,
                policy='Exact source clocks; all eligible intersecting routes retained, including outside-window events of crossing trains. Unresolved records cannot be excluded as irrelevant. No track or boundary assumptions.')
            (case/'case_audit.json').write_text(json.dumps(audit, ensure_ascii=False, indent=2), encoding='utf-8')
            summaries.append({k: v for k, v in audit.items() if k not in ('station_aliases','window_start_absolute_seconds','window_end_absolute_seconds','policy')})
    (output/'summary.json').write_text(json.dumps(summaries, indent=2), encoding='utf-8')
    (output/'README.txt').write_text(
        '真实时刻数据包（待补齐资源及边界信息，尚非完整求解输入）\n'
        '本目录包含10/15/20/25站、2/4/8/24小时时窗，共16组。站数各自选择完整贯通记录最多的走廊，彼此不保证嵌套。\n'
        'real_visits.csv：真实逐站秒级时刻、相对时刻及原始单元格定位。相对时刻只减统一窗口起点，不改变时间差。\n'
        'observed_running_times.csv：仅在相邻站真实出发与到达时刻均明确时计算逐车运行秒数。\n'
        'observed_dwell_seconds保留真实停站时长；候选停站下界仍为240秒。缺失时刻不填零、不插值。\n'
        '纳入资料可唯一解析且接触走廊的所有记录，包括局部经过和跨时窗列车；跨窗事件保留原值，可能超出相对[0,H]。\n'
        'missing_items.csv列出固定股道、缺失到发语义、端点占轨待确认项。unresolved_source_blocks.csv列出仍无法唯一解析的相关原始记录（全天保守清单）。\n'
        '这些清单没有被静默排除或用假设补齐，故ready_for_solver=false。不能将本包宣称为完整既有运行图或用于正式性能结论。\n'
        '原始提取与身份映射保留在邻接real_timetable_20261116_v1目录。所有处理本地进行。\n', encoding='utf-8-sig')
    print(json.dumps({'packages':len(summaries),'summary':summaries}))


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    try:
        run(args.source, args.output)
    except Exception as error:
        print(json.dumps({'error_type':type(error).__name__}))
        raise SystemExit(1)
