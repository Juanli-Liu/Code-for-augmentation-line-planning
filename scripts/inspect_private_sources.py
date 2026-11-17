"""Offline structural inventory. Never print source paths, names, or cell values.

Only explicit generic field labels and aggregate statistics leave the local parser.
The private manifest stays in the output directory; do not publish that directory.
"""
import argparse
import collections
import contextlib
import datetime
import io
import json
import logging
import pathlib
import re
import warnings

FIELDS = set('序号 车次 车站 站名 到达 出发 到时 开时 到达时刻 出发时刻 到达时间 出发时间 始发站 终到站 始发 终到 股道 停站 停时 车型 编组 交路 交路号 担当 日期 里程 运行时间 备注 到 开 发站 到站 列车车次'.split())
TRAIN = re.compile(r'[GDCZTKSY]?\d{1,5}(?:/\d{1,5})*', re.I)
TIME = re.compile(r'(?:[0-2]?\d)[:：.][0-5]\d(?::[0-5]\d)?')

def category(value):
    if value is None or str(value).strip() == '':
        return 'empty'
    if isinstance(value, (datetime.time, datetime.datetime)):
        return 'time_or_date'
    if isinstance(value, (int, float)):
        return 'number'
    text = re.sub(r'\s+', '', str(value))
    if text in FIELDS:
        return 'field:' + text
    if TIME.fullmatch(text):
        return 'time'
    if TRAIN.fullmatch(text):
        return 'id_like'
    if text.startswith('='):
        return 'formula'
    return 'text'

def summarize_rows(rows):
    kinds = collections.Counter()
    skeleton = []
    count = width = 0
    labels = set()
    for row in rows:
        count += 1
        width = max(width, len(row))
        cats = [category(v) for v in row]
        kinds.update(cats)
        labels.update(c[6:] for c in cats if c.startswith('field:'))
        if len(skeleton) < 8 and any(c != 'empty' for c in cats):
            skeleton.append({'row': count, 'cells': cats[:32]})
    return dict(rows=count, columns=width, types=dict(kinds), recognized_fields=sorted(labels), skeleton=skeleton)

def inspect(path):
    with path.open('rb') as stream:
        magic = stream.read(8)
    if path.suffix.lower() in ('.xlsx', '.xls') and magic.startswith(b'PK'):
        import openpyxl
        with path.open('rb') as stream:
            book = openpyxl.load_workbook(stream, read_only=True, data_only=True, keep_links=False)
            try:
                for sheet in book:
                    # Some source exports declare A1:C11 while containing many more cells.
                    sheet.reset_dimensions()
                return {'format': 'xlsx', 'sheets': [dict(sheet_id=f'S{i:03d}', **summarize_rows(sheet.iter_rows(values_only=True))) for i, sheet in enumerate(book, 1)]}
            finally:
                book.close()
    if path.suffix.lower() == '.xls':
        return {'format': 'xls', 'status': 'legacy_reader_needed', 'ole_container': magic.startswith(bytes.fromhex('D0CF11E0A1B11AE1'))}
    if path.suffix.lower() == '.docx':
        from docx import Document
        doc = Document(path)
        return {'format': 'docx', 'paragraphs': len(doc.paragraphs), 'tables': [dict(table_id=f'T{i:03d}', **summarize_rows([[c.text for c in row.cells] for row in table.rows])) for i, table in enumerate(doc.tables, 1)]}
    if path.suffix.lower() == '.pdf':
        from pypdf import PdfReader
        reader = PdfReader(path)
        pages = []
        for i, page in enumerate(reader.pages, 1):
            text = page.extract_text() or ''
            pages.append({'page': i, 'characters': len(text), 'time_tokens': len(TIME.findall(text)), 'train_tokens': len(re.findall(r'\b[GDC]\d{1,5}\b', text))})
        return {'format': 'pdf', 'pages': pages, 'encrypted': reader.is_encrypted}
    return {'status': 'unsupported'}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    manifest, summaries = [], []
    logging.disable(logging.CRITICAL)
    for i, path in enumerate(sorted(p for p in args.source.rglob('*') if p.is_file()), 1):
        file_id = f'F{i:03d}'
        manifest.append({'file_id': file_id, 'path': str(path.resolve())})
        record = {'file_id': file_id, 'extension': path.suffix.lower(), 'bytes': path.stat().st_size}
        try:
            # Libraries can include source text in warnings and diagnostics.
            with warnings.catch_warnings(), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                warnings.simplefilter('ignore')
                record.update(inspect(path))
        except Exception as error:
            record['error_type'] = type(error).__name__
        summaries.append(record)
        brief = {k: v for k, v in record.items() if k not in ('sheets', 'tables', 'pages')}
        for key in ('sheets', 'tables', 'pages'):
            if key in record:
                brief[key + '_count'] = len(record[key])
        print(json.dumps(brief, ensure_ascii=True), flush=True)
    (args.output / 'PRIVATE_manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    (args.output / 'structure_summary.json').write_text(json.dumps(summaries, ensure_ascii=False, indent=2), encoding='utf-8')

if __name__ == '__main__':
    main()
