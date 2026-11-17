"""Local, provenance-preserving extraction. Never emits raw records to stdout.

Unlike the provisional extractor, detect EVERY repeated header and never cross it.
All exported operational data remains confidential, including anonymous timings.
"""
import argparse
import collections
import contextlib
import hashlib
import io
import json
import math
import pathlib
import re
import warnings
import openpyxl

def clean(v):
    return re.sub(r'[\s\u2800]+', '', str(v if v is not None else ''))

def parse_clock(v):
    s=clean(v)
    if not s: return {'kind':'blank'}
    if s in ('…','...','……'): return {'kind':'ellipsis'}
    if s=='--': return {'kind':'dash'}
    # Small-font numeric suffix is retained as a provisional seconds component.
    small=[]
    if isinstance(v,list):
        for run in v:
            if hasattr(run,'font') and run.font.sz is not None and run.font.sz<=6:
                small.append(str(run.text))
    m=re.fullmatch(r'(?:(\d{1,2})[:：])?(\d{2})(\d{2})?',s)
    if not m: return {'kind':'unrecognized'}
    h,mi,se=m.groups()
    if (h and int(h)>23) or int(mi)>59 or (se and int(se)>59): return {'kind':'invalid_clock'}
    if se and small!=[se]: return {'kind':'unverified_numeric_suffix'}
    return {'kind':'clock','hour':int(h) if h else None,'minute':int(mi),
            'second':int(se or 0),'seconds_style_verified':bool(se)}

def resolve(clocks):
    """Require a unique nondecreasing assignment between explicit hour anchors.

    No arbitrary maximum inter-event travel time is imposed. Ambiguous omitted
    hours remain unresolved. Cross-midnight anchor unwraps are explicitly marked.
    """
    anchors={}; prev=-1; day=0
    for i,c in enumerate(clocks):
        hour=c['hour'] if c['hour'] is not None else c.get('reference_hour')
        if hour is None: continue
        value=day*86400+hour*3600+c['minute']*60+c['second']
        if value<prev:
            day+=1; value+=86400
        anchors[i]=value;prev=value
    if not anchors:return None,'no_hour_anchor',day
    earliest=[];prev=None
    for i,c in enumerate(clocks):
        off=c['minute']*60+c['second']
        if i in anchors: value=anchors[i]
        elif prev is None:value=None
        else:value=off+3600*math.ceil((prev-off)/3600)
        if value is not None and prev is not None and value<prev:return None,'anchor_conflict',day
        earliest.append(value);prev=value
    latest=[None]*len(clocks);nxt=None
    for i in range(len(clocks)-1,-1,-1):
        c=clocks[i];off=c['minute']*60+c['second']
        value=anchors[i] if i in anchors else (None if nxt is None else off+3600*math.floor((nxt-off)/3600))
        if value is not None and nxt is not None and value>nxt:return None,'anchor_conflict',day
        latest[i]=value;nxt=value
    if earliest!=latest or any(x is None for x in earliest):return None,'ambiguous_omitted_hour',day
    if earliest[-1]-earliest[0]>=86400:return None,'duration_at_least_one_day',day
    return earliest,'unique_clock_sequence',day

def main(args):
    root=args.output;root.mkdir(exist_ok=True)
    manifest=json.loads(args.manifest.read_text(encoding='utf-8'))
    source=next(x['path'] for x in manifest if x['file_id']=='F003')
    book=openpyxl.load_workbook(source,read_only=False,data_only=True,rich_text=True,keep_links=False)
    trains={};stations={};blocks=[];issues=[];raw_blocks=[]
    references=collections.defaultdict(list)
    ref_path=root/'PRIVATE_endpoint_reference.json'
    if ref_path.exists():
        for row in json.loads(ref_path.read_text(encoding='utf-8')):references[clean(row[1])].append(row)
    reference_anchors=0
    alias=lambda mapping,key,prefix:mapping.setdefault(key,f'{prefix}{len(mapping)+1:05d}')
    for si,sheet in enumerate(book,1):
        rows=list(sheet.values)
        for col in range(sheet.max_column-1):
            headers=[r for r,row in enumerate(rows) if col<len(row) and clean(row[col])=='车站车次']
            for hi,header in enumerate(headers):
                limit=headers[hi+1] if hi+1<len(headers) else len(rows)
                train_name=clean(rows[header][col+1])
                bid=f'B{len(blocks)+1:05d}'
                train=alias(trains,train_name,'T')
                visits=[];raw=[];flags=[]
                if not re.fullmatch(r'(?:[GDCZTKSY]|0[GDC])?\d{1,5}(?:/\d{1,5})*',train_name):flags.append('unrecognized_train_label')
                for r in range(header+1,limit):
                    label=clean(rows[r][col])
                    if not label:continue
                    a=rows[r][col+1]
                    # A station row followed by a blank station label forms one pair.
                    if r+1>=limit or clean(rows[r+1][col]):
                        flags.append('unpaired_station_row');continue
                    d=rows[r+1][col+1]
                    if not clean(a) and not clean(d):continue
                    station=alias(stations,label,'S')
                    pa,pd=parse_clock(a),parse_clock(d)
                    visits.append({'station':station,'row':r+1,'arrival_cell':pa,'departure_cell':pd})
                    raw.append({'station':label,'row':r+1,'arrival_cell_text':str(a or ''),'departure_cell_text':str(d or '')})
                # Supplement omitted endpoint hours only with unambiguous, same-station
                # reference clocks agreeing in BOTH minute and second. Source cells stay intact.
                for index,key,station_col,time_col in ((0,'departure_cell',5,9),(-1,'arrival_cell',7,10)):
                    if not visits:continue
                    cell=visits[index][key]
                    if cell['kind']!='clock' or cell['hour'] is not None:continue
                    candidates=set()
                    for ref in references[train_name]:
                        m=re.fullmatch(r'(\d{2}):(\d{2}):(\d{2})',str(ref[time_col]))
                        if m and clean(ref[station_col])==raw[index]['station'] and int(m[2])==cell['minute'] and int(m[3])==cell['second']:
                            candidates.add(int(m[1]))
                    if len(candidates)==1:
                        cell['reference_hour']=next(iter(candidates));cell['reference_file_id']='F004';reference_anchors+=1
                clock_refs=[(v,k) for v in visits for k in ('arrival_cell','departure_cell') if v[k]['kind']=='clock']
                times,status,days=resolve([v[k] for v,k in clock_refs])
                for vi,v in enumerate(visits):
                    for k in ('arrival_cell','departure_cell'):
                        terminal_missing = (vi==len(visits)-1 and k=='departure_cell' and v[k]['kind']=='dash')
                        if v[k]['kind'] not in ('clock','blank','ellipsis') and not terminal_missing:flags.append(v[k]['kind'])
                if times is not None:
                    for (v,k),t in zip(clock_refs,times):v[k]['absolute_seconds']=t
                else:flags.append(status)
                if len(visits)<2:flags.append('too_few_visits')
                if len({v['station'] for v in visits})!=len(visits):flags.append('repeated_station')
                block={'block':bid,'train':train,'sheet':si,'header_row':header+1,'column':col+2,
                       'clock_status':status,'day_rollovers':days,'issues':sorted(set(flags)),'visits':visits}
                blocks.append(block)
                raw_blocks.append({'block':bid,'train':train_name,'sheet_name':sheet.title,'visits':raw})
    book.close()
    # Group exact repeated records separately from conflicting definitions.
    groups=collections.defaultdict(list)
    for b in blocks:groups[b['train']].append(b)
    unique=[];duplicate_count=0;conflicting=[]
    for train,group in groups.items():
        variants={}
        for b in group:
            signature=json.dumps([{k:v for k,v in visit.items() if k!='row'} for visit in b['visits']],sort_keys=True)
            variants.setdefault(signature,[]).append(b)
        if len(variants)>1:
            conflicting.append(train)
            for b in group:b['issues'].append('conflicting_train_definitions')
        else:
            unique.append(group[0]);duplicate_count+=len(group)-1
    # Only records with unique chronology and no structural problems enter screening.
    eligible=[b for b in unique if not b['issues']]
    errors=collections.Counter(issue for b in blocks for issue in b['issues'])
    cells=collections.Counter(v[k]['kind'] for b in blocks for v in b['visits'] for k in ('arrival_cell','departure_cell'))
    out={'source_sha256':hashlib.sha256(pathlib.Path(source).read_bytes()).hexdigest(),
         'blocks':len(blocks),'train_labels':len(trains),'station_labels':len(stations),
         'visit_pairs':sum(len(b['visits']) for b in blocks),'reference_endpoint_hour_anchors':reference_anchors,'identical_duplicate_blocks':duplicate_count,
         'conflicting_train_labels':len(conflicting),'eligible_unique_records':len(eligible),
         'issues':dict(errors),'cell_kinds':dict(cells),
         'time_interpretation':'small-font suffix interpreted as seconds; hour omission resolved only when unique; endpoint cross-check required',
         'not_model_input':True}
    for name,obj in [('PRIVATE_raw_blocks.json',raw_blocks),('PRIVATE_identity_mapping.json',{'trains':trains,'stations':stations}),
                     ('real_timetable_records.json',blocks),('eligible_records.json',eligible),('extraction_audit.json',out)]:
        (root/name).write_text(json.dumps(obj,ensure_ascii=False,indent=2),encoding='utf-8')
    return out

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--manifest',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True)
    args=p.parse_args()
    try:
        with warnings.catch_warnings(),contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
            warnings.simplefilter('ignore');result=main(args)
        print(json.dumps(result,ensure_ascii=True))
    except Exception as error:
        print(json.dumps({'error_type':type(error).__name__}));raise SystemExit(1)
