"""Select real-record cohorts without retiming, rounding, or inventing resources.
Outputs remain confidential. Summary JSON is the only stdout payload.
"""
import argparse
import collections
import csv
import json
import pathlib

def event(v,k):
    return v[k].get('absolute_seconds')

def span(visits):
    times=[event(v,k) for v in visits for k in ('arrival_cell','departure_cell') if event(v,k) is not None]
    return (min(times),max(times)) if times else None

def subroute(block,route):
    stations=[v['station'] for v in block['visits']]
    for seq in (route,route[::-1]):
        for i in range(len(stations)-len(seq)+1):
            if tuple(stations[i:i+len(seq)])==seq:return block['visits'][i:i+len(seq)]
    return None

def run(root):
    eligible=json.loads((root/'eligible_records.json').read_text(encoding='utf-8'))
    all_records=json.loads((root/'real_timetable_records.json').read_text(encoding='utf-8'))
    windows=collections.Counter()
    for b in eligible:
        stations=[v['station'] for v in b['visits']]
        for i in range(len(stations)-5):
            seq=tuple(stations[i:i+6]);windows[min(seq,seq[::-1])]+=1
    route,support=windows.most_common(1)[0]
    complete=[]
    for b in eligible:
        visits=subroute(b,route)
        if visits is not None and span(visits):complete.append((b,visits,span(visits)))
    # Unknown records touching the chosen corridor must remain visible in coverage.
    unresolved=[b for b in all_records if b['issues'] and set(route)&{v['station'] for v in b['visits']}]
    partial=[b for b in eligible if set(route)&{v['station'] for v in b['visits']} and subroute(b,route) is None]
    cases=[]
    for hours in (2,4,8,24):
        options=[]
        max_end=max(t[2][1] for t in complete)
        for start in range(0,max(1,int(max_end)),3600):
            end=start+hours*3600
            selected=[(b,v,t) for b,v,t in complete if start<=t[0] and t[1]<=end]
            crossing=[(b,v,t) for b,v,t in complete if t[0]<end and t[1]>=start and not(start<=t[0] and t[1]<=end)]
            options.append((len(selected),-len(crossing),start,selected,crossing))
        _,_,start,selected,crossing=max(options,key=lambda x:(x[0],x[1],-x[2]))
        name=f'corridor6_{hours:02d}h';directory=root/name;directory.mkdir(exist_ok=True)
        runtimes=collections.defaultdict(list);dwells=[];nonminute=0;time_count=0;single=0
        with (directory/'real_visits.csv').open('w',encoding='utf-8',newline='') as stream:
            writer=csv.writer(stream);writer.writerow(['train','station','sequence','arrival_absolute_seconds','departure_absolute_seconds','arrival_notation','departure_notation','source_block','source_sheet','source_row','source_column'])
            for b,visits,t in selected:
                for i,v in enumerate(visits):
                    a,d=event(v,'arrival_cell'),event(v,'departure_cell')
                    writer.writerow([b['train'],v['station'],i,a if a is not None else '',d if d is not None else '',v['arrival_cell']['kind'],v['departure_cell']['kind'],b['block'],b['sheet'],v['row'],b['column']])
                    for e in (a,d):
                        if e is not None:time_count+=1;nonminute+=e%60!=0
                    if a is not None and d is not None:dwells.append(d-a)
                    else:single+=1
                    if i:
                        previous=visits[i-1]
                        dep=event(previous,'departure_cell');arr=event(v,'arrival_cell')
                        # No passing-event equality inferred when one side is omitted.
                        if dep is not None and arr is not None:
                            ps=event(previous,'arrival_cell');nd=event(v,'departure_cell')
                            key=(previous['station'],v['station'],ps is not None and dep>ps,nd is not None and nd>arr)
                            runtimes[key].append(arr-dep)
        varied=[k for k,v in runtimes.items() if len(set(v))>1]
        incompatible={'nonminute_event_count':nonminute,'total_recorded_events':time_count,
                      'positive_dwell_under_4min':sum(0<x<240 for x in dwells),
                      'observed_section_stop_groups':len(runtimes),'groups_with_variable_running_seconds':len(varied),
                      'single_sided_station_times':single,'fixed_track_assignment':'not_established',
                      'truncated_route_endpoint_semantics':'requires_boundary_handling_without_invented_15min_occupation'}
        metadata={'case':name,'source':'actual source cells, unique clock reconstruction only',
                  'time_adjustment':None,'rounding':None,'station_aliases':route,
                  'window_start_absolute_seconds':start,'window_end_absolute_seconds':start+hours*3600,
                  'selected_complete_traversals':len(selected),'boundary_crossing_complete_traversals':len(crossing),
                  'unresolved_source_blocks_touching_corridor_all_day':len(unresolved),
                  'partial_route_records_touching_corridor_all_day':len(partial),
                  'coverage_warning':'Selected cohort only; not a complete background until crossing, partial and unresolved services are included or accounted for.',
                  'model_compatibility':incompatible,'ready_for_current_solver':False}
        (directory/'case_audit.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
        # Keep excluded identities local, allowing later completion of the background.
        (directory/'coverage_exclusions.json').write_text(json.dumps({'crossing':[b['block'] for b,v,t in crossing],
                    'unresolved':[b['block'] for b in unresolved],'partial':[b['block'] for b in partial]},indent=2),encoding='utf-8')
        cases.append({k:v for k,v in metadata.items() if k not in ('station_aliases','window_start_absolute_seconds','window_end_absolute_seconds')})
    summary={'status':'REAL_COHORTS_NOT_YET_SOLVER_INPUT','corridor_stations':6,'eligible_full_traversals_all_day':len(complete),'cases':cases}
    (root/'corridor_screening_summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(summary,ensure_ascii=True))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=pathlib.Path);a=p.parse_args()
    try:run(a.root)
    except Exception as e:print(json.dumps({'error_type':type(e).__name__}));raise SystemExit(1)
