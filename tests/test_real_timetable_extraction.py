"""Synthetic parser tests and provenance checks; no raw source values printed."""
import csv
import json
import pathlib
import sys
import unittest

sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
from extract_real_timetable import parse_clock,resolve
from openpyxl.cell.rich_text import CellRichText, TextBlock
from openpyxl.cell.text import InlineFont

class ClockTests(unittest.TestCase):
    def test_seconds_require_format_evidence(self):
        self.assertEqual(parse_clock('08:1230')['kind'],'unverified_numeric_suffix')
        c=parse_clock(CellRichText(['08:12',TextBlock(InlineFont(sz=6),'30')]))
        self.assertEqual((c['hour'],c['minute'],c['second']),(8,12,30))

    def test_hour_rollover_unique(self):
        times,status,days=resolve([parse_clock(x) for x in ['08:58','02','09:05']])
        self.assertEqual(times,[32280,32520,32700])
        self.assertEqual(days,0)

    def test_ambiguous_hour_not_guessed(self):
        self.assertIsNone(resolve([parse_clock(x) for x in ['08:00','10','10:20']])[0])

    def test_midnight(self):
        times,status,days=resolve([parse_clock(x) for x in ['23:58','02','00:05']])
        self.assertEqual(times,[86280,86520,86700])
        self.assertEqual(days,1)

    def test_missing_terminal_not_clock(self):
        self.assertEqual(parse_clock('--')['kind'],'dash')
        self.assertEqual(parse_clock('…')['kind'],'ellipsis')

def verify_exports(root, exports=None):
    exports = exports or root
    records=json.loads((root/'real_timetable_records.json').read_text(encoding='utf-8'))
    by_block={b['block']:b for b in records}
    checked=0
    for case in exports.glob('corridor*_*h'):
        audit = json.loads((case/'case_audit.json').read_text(encoding='utf-8'))
        seen = set()
        with (case/'real_visits.csv').open(encoding='utf-8-sig',newline='') as stream:
            for row in csv.DictReader(stream):
                key = (row['source_block'], int(row['source_row']))
                assert key not in seen
                seen.add(key)
                b=by_block[row['source_block']]
                assert not b['issues']
                v=next(v for v in b['visits'] if v['row']==int(row['source_row']))
                assert v['station']==row['station'] and b['train']==row['train']
                for side in ('arrival','departure'):
                    value=v[side+'_cell'].get('absolute_seconds')
                    assert row[side+'_absolute_seconds']==('' if value is None else str(value))
                    if side+'_relative_seconds' in row:
                        assert row[side+'_relative_seconds'] == ('' if value is None else str(value-audit['window_start_absolute_seconds']))
                    if value is not None:
                        cell=v[side+'_cell']
                        assert value%3600==cell['minute']*60+cell['second']
                        if cell['hour'] is not None:assert (value//3600)%24==cell['hour']
                checked+=1
        if 'station_count' in audit:
            # Independently reconstruct the cohort; no eligible partial/crossing record may vanish.
            route = set(audit['station_aliases'])
            expected = set()
            unresolved = set()
            for b in records:
                touching = [v for v in b['visits'] if v['station'] in route]
                if not touching:
                    continue
                if b['issues']:
                    unresolved.add(b['block'])
                    continue
                values = [v[k].get('absolute_seconds') for v in touching for k in ('arrival_cell','departure_cell')]
                values = [v for v in values if v is not None]
                if values and min(values) <= audit['window_end_absolute_seconds'] and max(values) >= audit['window_start_absolute_seconds']:
                    expected.update((b['block'],v['row']) for v in touching)
            assert seen == expected
            with (case/'unresolved_source_blocks.csv').open(encoding='utf-8-sig',newline='') as stream:
                assert {row['source_block'] for row in csv.DictReader(stream)} == unresolved
        running_path = case/'observed_running_times.csv'
        if running_path.exists():
            with running_path.open(encoding='utf-8-sig', newline='') as stream:
                for row in csv.DictReader(stream):
                    b = by_block[row['source_block']]
                    pair = [i for i,v in enumerate(b['visits']) if v['row'] == int(row['from_source_row'])]
                    i = pair[0]
                    before, after = b['visits'][i:i+2]
                    assert after['row'] == int(row['to_source_row'])
                    assert before['station'] == row['from_station'] and after['station'] == row['to_station']
                    assert int(row['running_seconds']) == after['arrival_cell']['absolute_seconds']-before['departure_cell']['absolute_seconds'] > 0
    result={'export_rows_verified':checked,'retimed_or_rounded_events':0,'synthetic_parser_tests':5}
    (exports/'export_validation.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result))

if __name__=='__main__':
    success=unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(ClockTests)).wasSuccessful()
    if not success:raise SystemExit(1)
    if len(sys.argv)>1:verify_exports(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]) if len(sys.argv)>2 else None)
