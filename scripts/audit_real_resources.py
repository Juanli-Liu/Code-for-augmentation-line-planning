"""Conservative local join of explicit station/track/train rows to real cohorts."""
import argparse
import collections
import csv
import json
import pathlib
import re
from docx import Document

def clean(v):return re.sub(r'\s+','',str(v or ''))

def run(root,manifest_path):
    manifest=json.loads(manifest_path.read_text(encoding='utf-8'))
    doc=Document(next(x['path'] for x in manifest if x['file_id']=='F002'))
    mapping=json.loads((root/'PRIVATE_identity_mapping.json').read_text(encoding='utf-8'))
    tracks=collections.defaultdict(set);counts=collections.Counter();private=[]
    pattern=r'(?:[GDCZTKSY]|0[GDC])\d{1,5}(?:/\d{1,5})*'
    for ti,table in enumerate(doc.tables,1):
        for ri,row in enumerate(table.rows[1:],2):
            station,track,train_text=[clean(c.text) for c in row.cells[:3]]
            ids=re.findall(pattern,train_text)
            rest=re.sub(pattern,'',train_text)
            unconditional=not re.sub(r'[、,，;；/\s]+','',rest)
            single=bool(re.fullmatch(r'(?:\d+[A-Za-z]?|[ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩ]+|[IVX]+)',track))
            counts['rows']+=1
            if not(single and unconditional and ids):counts['rows_not_unambiguously_fixed']+=1;continue
            for train in ids:
                tracks[(station,train)].add(track)
                private.append({'station':station,'train':train,'track':track,'table':ti,'row':ri})
    reverse_station={v:k for k,v in mapping['stations'].items()}
    reverse_train={v:k for k,v in mapping['trains'].items()}
    summaries=[]
    for case in sorted(root.glob('corridor6_*h')):
        rows=list(csv.DictReader((case/'real_visits.csv').open(encoding='utf-8',newline='')))
        stats=collections.Counter(); joined=[]
        for row in rows:
            options=tracks[(reverse_station[row['station']],reverse_train[row['train']])]
            status='unique_explicit_match' if len(options)==1 else 'conflicting_matches' if options else 'not_established'
            stats[status]+=1
            joined.append({'train':row['train'],'station':row['station'],'status':status})
        (case/'track_coverage.json').write_text(json.dumps(joined,indent=2),encoding='utf-8')
        summaries.append({'case':case.name,'visits':len(rows),**stats})
    (root/'PRIVATE_explicit_track_rows.json').write_text(json.dumps(private,ensure_ascii=False,indent=2),encoding='utf-8')
    result={'interpretation':'Only single-track, unconditional explicit train lists accepted; unparsed text is not converted into fixed assignments.',
            'document_rows':dict(counts),'case_coverage':summaries}
    (root/'track_coverage_summary.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=pathlib.Path);p.add_argument('--manifest',type=pathlib.Path,required=True);a=p.parse_args()
    try:run(a.root,a.manifest)
    except Exception as e:print(json.dumps({'error_type':type(e).__name__}));raise SystemExit(1)
