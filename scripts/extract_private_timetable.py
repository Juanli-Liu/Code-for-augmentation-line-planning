"""Extract timetable cells locally without exposing operational records to stdout.

This is an intermediate extraction, not validated model input. Missing hours are
preserved as missing; no chronology, passing-event, or rounding assumptions are
silently applied. Output files remain confidential even with anonymous IDs.
"""
import argparse
import collections
import contextlib
import io
import json
import pathlib
import re
import warnings

import openpyxl


def clean(value):
    return re.sub(r'[\s\u2800]+', '', str(value or ''))


def clock_parts(value):
    value = clean(value)
    if not value:
        return {'kind': 'blank'}
    if value in ('…', '...', '……'):
        return {'kind': 'ellipsis'}
    match = re.fullmatch(r'(?:(\d{1,2})[:：])?(\d{2})(\d{2})?', value)
    if not match:
        return {'kind': 'unrecognized'}
    hour, minute, second = match.groups()
    if (hour is not None and int(hour) > 23) or int(minute) > 59 or (second and int(second) > 59):
        return {'kind': 'out_of_range'}
    return {'kind': 'clock', 'hour': int(hour) if hour is not None else None,
            'minute': int(minute), 'second': int(second) if second else 0,
            'seconds_explicit': second is not None}


def extract(source, output):
    output.mkdir(parents=True, exist_ok=False)
    book = openpyxl.load_workbook(source, read_only=True, data_only=True, keep_links=False)
    stations, trains, records = {}, {}, []
    kinds = collections.Counter()
    ambiguous = []
    try:
        for sheet_index, sheet in enumerate(book, 1):
            sheet.reset_dimensions()
            rows = list(sheet.values)
            if len(rows) < 2:
                continue
            for col in range(1, len(rows[1]), 3):
                original_train = clean(rows[1][col])
                if not re.fullmatch(r'(?:[GDCZTKSY]|0[GDC])?\d{1,5}(?:/\d{1,5})*', original_train):
                    ambiguous.append({'sheet': sheet_index, 'column': col + 1, 'reason': 'unrecognized_train_header'})
                    continue
                train = trains.setdefault(original_train, f'T{len(trains)+1:05d}')
                visits = []
                for row_index in range(3, len(rows), 2):
                    row = rows[row_index]
                    if col >= len(row) or not clean(row[col-1]):
                        continue
                    first = row[col]
                    second = rows[row_index+1][col] if row_index+1 < len(rows) and col < len(rows[row_index+1]) else None
                    arrival, departure = clock_parts(first), clock_parts(second)
                    if arrival['kind'] == departure['kind'] == 'blank':
                        continue
                    original_station = clean(row[col-1])
                    station = stations.setdefault(original_station, f'S{len(stations)+1:04d}')
                    kinds.update([arrival['kind'], departure['kind']])
                    visits.append({'station': station, 'source_row': row_index+1,
                                   'arrival_cell': arrival, 'departure_cell': departure})
                records.append({'train': train, 'sheet': sheet_index, 'column': col+1, 'visits': visits})
    finally:
        book.close()
    clocks = [v[k] for r in records for v in r['visits'] for k in ('arrival_cell', 'departure_cell') if v[k]['kind']=='clock']
    summary = {'status': 'intermediate_not_model_input', 'table_blocks': len(records),
               'unique_train_labels': len(trains), 'unique_station_labels': len(stations),
               'visit_pairs': sum(len(r['visits']) for r in records), 'cell_kinds': dict(kinds),
               'clock_cells': len(clocks), 'omitted_hour_cells': sum(c['hour'] is None for c in clocks),
               'nonzero_second_cells': sum(c['second'] != 0 for c in clocks),
               'unrecognized_headers': len(ambiguous),
               'limitations': ['Clock suffix interpreted provisionally as seconds; verify source convention.',
                              'Omitted hours and day rollovers unresolved.',
                              'Ellipses are preserved, not automatically treated as passing.',
                              'Routes, duplicates, tracks and chronology not yet validated.']}
    for name, obj in [('PRIVATE_identity_mapping.json', {'stations': stations, 'trains': trains}),
                      ('timetable_cells_anonymous.json', records), ('extraction_summary.json', summary),
                      ('header_issues.json', ambiguous)]:
        (output/name).write_text(json.dumps(obj, ensure_ascii=False, indent=2), encoding='utf-8')
    return summary


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--manifest', type=pathlib.Path, required=True)
    parser.add_argument('--file-id', required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args=parser.parse_args()
    try:
        manifest=json.loads(args.manifest.read_text(encoding='utf-8'))
        source=next(x['path'] for x in manifest if x['file_id']==args.file_id)
        with warnings.catch_warnings(), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            warnings.simplefilter('ignore')
            summary=extract(source,args.output)
        print(json.dumps(summary,ensure_ascii=True))
    except Exception as error:
        print(json.dumps({'error_type':type(error).__name__}))
        raise SystemExit(1)


if __name__=='__main__':
    main()
