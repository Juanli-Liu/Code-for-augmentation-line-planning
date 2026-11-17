"""Offline synthetic scale cases calibrated to the current model.

Only the anonymous six-station corridor is source-derived. All timings, resources,
demand and service choices are synthetic. No original time records are copied.
"""
import argparse
import collections
import csv
import itertools
import json
import pathlib
import random


def write_csv(path, header, rows):
    with path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.writer(stream)
        writer.writerow(header)
        writer.writerows(rows)


def generate(extracted, template, output):
    source = json.loads(extracted.read_text(encoding='utf-8'))
    windows = collections.Counter()
    for block in source:
        route = [v['station'] for v in block['visits']
                 if any(v[k]['kind'] == 'clock' for k in ('arrival_cell', 'departure_cell'))]
        for i in range(len(route)-5):
            seq = tuple(route[i:i+6])
            if len(set(seq)) == 6:
                windows[min(seq, seq[::-1])] += 1
    corridor, support = windows.most_common(1)[0]
    output.mkdir(parents=True, exist_ok=False)
    configs = {}
    base = (20, 24, 2, 480)
    for axis, values in enumerate(((10,20,40,80), (12,24,48,80), (1,2,3,4), (240,480,720,960))):
        for value in values:
            config = list(base)
            config[axis] = value
            configs.setdefault(tuple(config), []).append(('trains','lines','frequency','horizon')[axis])
    all_lines = []
    for origin in range(6):
        for dest in range(6):
            if origin == dest:
                continue
            path = tuple(range(origin, dest+(1 if dest>origin else -1), 1 if dest>origin else -1))
            for middle in itertools.product((0,1), repeat=len(path)-2):
                all_lines.append((path, (1,)+middle+(1,)))
    full = [(tuple(range(6)), (1,)*6), (tuple(range(5,-1,-1)), (1,)*6)]
    with template.open(encoding='utf-8-sig', newline='') as stream:
        params = list(csv.reader(stream))
    manifest = []
    for seed in (11, 23, 37):
        rng = random.Random(seed)
        pool = [line for line in all_lines if line not in full]
        rng.shuffle(pool)
        pool = full + pool
        demand = {(o,d):rng.randint(80,200) for o in range(6) for d in range(6) if o!=d}
        for (n, line_count, freq, horizon), axes in configs.items():
            name = f's{seed}_n{n}_l{line_count}_f{freq}_h{horizon}'
            directory = output/name
            directory.mkdir()
            station = lambda i: f'S{i:02d}'
            tracks = ['t1','t2','t3','t4','b1','b2']
            write_csv(directory/'stations.csv', ['station_id','station_name'], [(i,station(i)) for i in range(6)])
            write_csv(directory/'platforms.csv', ['platform_id','station_id','platform_name','total_available_time'],
                      [(6*i+j,i,t,horizon) for i in range(6) for j,t in enumerate(tracks)])
            sections = [(i,i+1,'down') for i in range(5)] + [(i+1,i,'up') for i in range(5)]
            write_csv(directory/'sections.csv', ['section_id','from_station','to_station','maximum_train_count','pure_running_time','direction'],
                      [(i,station(o),station(d),horizon//3,8,direction) for i,(o,d,direction) in enumerate(sections)])
            timetable = []
            for k in range(n):
                up = k%2
                ordinal = k//2
                route = tuple(range(5,-1,-1)) if up else tuple(range(6))
                track = ('t2','t4')[ordinal%2] if up else ('t1','t3')[ordinal%2]
                t = 15 + ordinal*9 + seed%3
                times = [t]
                for position in range(1,6):
                    t += 13  # 8 pure + 2 start + 3 stop.
                    times.append(t)
                    if position != 5:
                        t += 4
                        times.append(t)
                assert t+15 <= horizon
                paths = ';'.join(f'{station(s)}-{track}' for s in route for _ in range(2))
                timetable.append((f'T{k:05d}',station(route[0]),station(route[-1]),paths,';'.join(map(str,times))))
            write_csv(directory/'timetable.csv', ['trainID','FromNode','ToNode','Paths','Times'], timetable)
            lines, allowed = [], []
            for i,(route,stops) in enumerate(pool[:line_count]):
                lines.append((i,';'.join(map(station,route)),';'.join(map(str,stops))))
                up = route[0]>route[-1]
                for s,stop in zip(route,stops):
                    ids = ([1,3] if up else [0,2]) if stop else ([5] if up else [4])
                    allowed.append((i,station(s),stop,';'.join(str(6*s+j) for j in ids),';'.join(tracks[j] for j in ids)))
            write_csv(directory/'lines.csv', ['line_id','stations','stop_plan'], lines)
            write_csv(directory/'line_allowed_platforms.csv', ['line_id','station_name','stop_plan','platform_ids','platform_name'], allowed)
            write_csv(directory/'ods.csv', ['od_id','origin_station','destination_station','demand'],
                      [(i,station(o),station(d),q) for i,((o,d),q) in enumerate(demand.items())])
            overrides = {'planning_horizon':str(horizon),'candidate_frequencies':';'.join(map(str,range(1,freq+1))),
                         'long_seat_capacity':'100','short_seat_capacity':'200','long_available_units':'8','short_available_units':'8',
                         'timetable_solver':'2','conflict_cut_mode':'1','planning_output_root':str(directory.resolve()/'results'),
                         'existing_train_max_delay':'5','total_time_limit_seconds':'300','master_time_limit_seconds':'30',
                         'timetable_time_limit_seconds':'30','maximum_rounds':'100'}
            updated = [params[0]] + [[r[0],overrides.get(r[0],r[1]),r[2], 'Synthetic scale experiment parameter'] for r in params[1:]]
            write_csv(directory/'parameters.csv', updated[0],updated[1:])
            manifest.append({'case':name,'seed':seed,'existing_trains':n,'candidate_lines':line_count,
                             'frequency_options':freq,'candidates':line_count*freq*2,'horizon':horizon,'axes':axes})
    metadata = {'classification':'CONFIDENTIAL_LOCAL_EXPERIMENT',
                'source_derived':{'anonymous_corridor':corridor,'matching_windows':support},
                'synthetic':['all event times','all tracks and assignments','all section capacities and running times',
                             'OD demand','candidate stop patterns and frequencies','rolling stock capacities and quantities'],
                'design':'One factor at a time; common base n20/l24/f2/h480; three fixed seeds; duplicate base cases stored once.',
                'timing_rule':'All-stop background; 9-minute departure spacing per direction; alternate two directional stop tracks; 8+2+3 minute section runs; 4-minute dwell; 15-minute terminal occupation.',
                'scope':'Source-informed synthetic six-station corridor. Not a faithful replay, not a large-network benchmark.',
                'source_time_cells_used':False,
                'validation':'Run validate_generated_inputs for production loader and original timetable constraint checks.',
                'cases':manifest}
    (output/'generation_manifest.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'cases':len(manifest),'stations':6,'existing_train_range':[10,80],
                      'candidate_range':[min(x['candidates'] for x in manifest),max(x['candidates'] for x in manifest)],
                      'horizon_range':[240,960],'source_time_cells_used':False}))


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--extracted',type=pathlib.Path,required=True)
    parser.add_argument('--template',type=pathlib.Path,required=True)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    args=parser.parse_args()
    generate(args.extracted,args.template,args.output)
