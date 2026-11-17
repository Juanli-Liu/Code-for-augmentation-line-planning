"""Read source locally; publish only field/category counts and alias-based gaps."""
import argparse
import collections
import contextlib
import io
import json
import re
import warnings
from pathlib import Path
from docx import Document
import openpyxl
from pypdf import PdfReader


def run(manifest_path, output):
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    paths = {x['file_id']:x['path'] for x in manifest}
    keywords = ['股道','到达','出发','始发','终到','车次','车站','通过','交路','动车组','编组','检修','停车','时刻']
    result = {}
    book = openpyxl.load_workbook(paths['F001'], read_only=True, data_only=True)
    hits = collections.Counter()
    for sheet in book:
        sheet.reset_dimensions()
        for row in sheet.iter_rows(values_only=True):
            for value in row:
                if isinstance(value,str):
                    hits.update(k for k in keywords if k in value)
    book.close()
    result['F001_keyword_cell_counts'] = hits
    doc = Document(paths['F002'])
    track_shapes = collections.Counter()
    train_shapes = collections.Counter()
    private = []
    pattern = r'(?:[GDCZTKSY]|0[GDC])\d{1,5}(?:/\d{1,5})*'
    for ti, table in enumerate(doc.tables,1):
        for ri, row in enumerate(table.rows[1:],2):
            station, track, train = [re.sub(r'\s+','',c.text) for c in row.cells[:3]]
            simple = bool(re.fullmatch(r'(?:\d+[A-Za-z]?|[ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩ]+|[IVX]+)',track))
            suffix = bool(re.fullmatch(r'(?:\d+[A-Za-z]?|[ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩ]+|[IVX]+)(?:道|股道)',track))
            track_shapes['simple' if simple else 'single_with_suffix' if suffix else 'empty' if not track else 'other_or_multiple'] += 1
            ids = re.findall(pattern, train)
            rest = re.sub(r'[、,，;；/\s]+','',re.sub(pattern,'',train))
            train_shapes['explicit_only' if ids and not rest else 'empty' if not train else 'conditional_or_other'] += 1
            if not(simple and ids and not rest):
                private.append(dict(table=ti,row=ri,station=station,track=track,train_text=train))
    result['F002_track_cell_categories'] = track_shapes
    result['F002_train_list_categories'] = train_shapes
    # Store original unparsed rows only locally for the owner's review, never stdout.
    (output/'PRIVATE_unparsed_track_rows.json').write_text(json.dumps(private,ensure_ascii=False,indent=2),encoding='utf-8')
    pdf_hits = collections.Counter()
    for page in PdfReader(paths['F005']).pages:
        text = page.extract_text() or ''
        pdf_hits.update({k:text.count(k) for k in keywords if k in text})
    result['F005_keyword_occurrences'] = pdf_hits
    result['interpretation'] = 'Keyword counts indicate possible roles only; zero hits do not prove absence. Conditional track rules are retained for source-owner review, not guessed.'
    (output/'resource_recheck_summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('manifest',type=Path)
    parser.add_argument('output',type=Path)
    args = parser.parse_args()
    try:
        with warnings.catch_warnings(), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            warnings.simplefilter('ignore')
            result = run(args.manifest,args.output)
        print(json.dumps(result,ensure_ascii=True))
    except Exception as error:
        print(json.dumps({'error_type':type(error).__name__}))
        raise SystemExit(1)
