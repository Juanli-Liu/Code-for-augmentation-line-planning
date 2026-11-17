"""Draw train paths and fixed track occupations on ONE time-space axis."""

import csv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'augmentation line plan' / 'Data'
OUTPUT = DATA / 'timetable_diagram_integrated_240min_start_stop.pdf'


def read_csv(name):
    with (DATA / name).open(encoding='utf-8-sig', newline='') as source:
        return list(csv.DictReader(source))


def derive_visit_times(row, terminal_occupation):
    """Recover the two boundary events from the published actual times."""
    fields = row['Times'].split(';')
    path_count = len(row['Paths'].split(';'))
    assert path_count >= 4 and path_count % 2 == 0, row['trainID']
    assert len(fields) in (path_count - 2, path_count), row['trainID']
    actual_fields = fields[1:-1] if len(fields) == path_count else fields
    actual = list(map(int, actual_fields))
    return [actual[0] - terminal_occupation, *actual, actual[-1] + terminal_occupation]


def main():
    from reportlab.lib import colors
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.ttfonts import TTFont
    from reportlab.pdfgen import canvas

    font_path = Path('C:/Windows/Fonts/msyh.ttc')
    pdfmetrics.registerFont(TTFont('Chinese', str(font_path), subfontIndex=0))
    stations = read_csv('stations.csv')
    platforms = read_csv('platforms.csv')
    trains = read_csv('timetable.csv')
    width, height = 1300, 930
    left, right = 125, 1248
    bottom, top = 110, 805
    parameters = {row['parameter']: row['value'] for row in read_csv('parameters.csv')}
    max_time = int(parameters['planning_horizon'])
    terminal_occupation = int(parameters['minimum_terminal_platform_occupation_time'])
    gap = (top - bottom - 48) / (len(stations) - 1)
    coordinates = {}
    station_groups = []
    for index, station in enumerate(stations):
        tracks = [p['platform_name'] for p in platforms if p['station_id'] == station['station_id']]
        center = bottom + 24 + index * gap
        lanes = [(track, center + ((len(tracks) - 1) / 2 - j) * 8.5)
                 for j, track in enumerate(tracks)]
        for track, y in lanes:
            coordinates[(station['station_name'], track)] = y
        station_groups.append((station['station_name'], lanes))

    parsed = []
    for row in trains:
        paths = [item for item in row['Paths'].split(';') if item]
        times = derive_visit_times(row, terminal_occupation)
        assert len(paths) == len(times) and len(paths) % 2 == 0
        assert all(a <= b for a, b in zip(times, times[1:])), row['trainID']
        points = []
        for path, time in zip(paths, times):
            station, track = path.rsplit('-', 1)
            assert (station, track) in coordinates, path
            assert 0 <= time <= max_time
            points.append((time, coordinates[(station, track)]))
        assert paths[0].rsplit('-', 1)[0] == row['FromNode']
        assert paths[-1].rsplit('-', 1)[0] == row['ToNode']
        parsed.append((row, points))

    doc = canvas.Canvas(str(OUTPUT), pagesize=(width, height))
    doc.setTitle('既有列车运行轨迹与固定到发线分配')
    doc.setFont('Chinese', 21)
    doc.drawCentredString(width / 2, 891, '既有列车运行轨迹与固定到发线分配')
    doc.setFont('Chinese', 10)
    doc.setFillColor(colors.HexColor('#475569'))
    doc.drawCentredString(width / 2, 869, '同一坐标系：斜线表示区间运行，轨道上的水平线表示停站占用，空心圆表示通过')

    x_pos = lambda time: left + time / max_time * (right - left)
    for time in range(0, max_time + 1, 5):
        major = time % 10 == 0
        doc.setStrokeColor(colors.HexColor('#d0d3d7' if major else '#edf0f2'))
        doc.setLineWidth(0.65 if major else 0.35)
        doc.line(x_pos(time), bottom, x_pos(time), top)
        if major:
            doc.setFillColor(colors.HexColor('#334155'))
            doc.setFont('Helvetica', 10)
            doc.drawCentredString(x_pos(time), bottom - 19, str(time))

    for group_index, (station, lanes) in enumerate(station_groups):
        low = min(y for _, y in lanes)
        high = max(y for _, y in lanes)
        doc.setFillColor(colors.HexColor('#f7f8fa'))
        doc.rect(left, low - 3, right - left, high - low + 6, fill=1, stroke=0)
        for track, y in lanes:
            main_track = track.startswith('b')
            doc.setStrokeColor(colors.HexColor('#737b85' if main_track else '#aeb5bd'))
            doc.setLineWidth(0.6 if main_track else 0.4)
            doc.setDash([] if main_track else [1.1, 1.7])
            doc.line(left, y, right, y)
            doc.setFont('Helvetica', 8)
            doc.setFillColor(colors.HexColor('#27303a'))
            doc.drawRightString(left - 8, y - 2.5, f'{station}-{track}')
        doc.setDash([])

    palette = ['#1565c0', '#b720a5', '#00877c', '#7542bf', '#bd6500',
               '#dc402d', '#725517', '#208728', '#3e556f', '#d32f72']
    for train_index, (row, points) in enumerate(parsed):
        color = colors.HexColor(palette[train_index % len(palette)])
        doc.setStrokeColor(color)
        doc.setLineWidth(1.65)
        path = doc.beginPath()
        path.moveTo(x_pos(points[0][0]), points[0][1])
        for time, y in points[1:]:
            path.lineTo(x_pos(time), y)
        doc.drawPath(path)
        for index in range(0, len(points), 2):
            arrival, departure = points[index], points[index + 1]
            mid_x = x_pos((arrival[0] + departure[0]) / 2)
            y = arrival[1]
            if arrival[0] == departure[0]:
                doc.setFillColor(colors.white)
                doc.circle(mid_x, y, 2.1, fill=1, stroke=1)
                label_x, label_y = mid_x + 4, y + 2.0
                centered = False
            else:
                doc.setStrokeColor(color)
                doc.setLineWidth(2.3)
                doc.line(x_pos(arrival[0]), y, x_pos(departure[0]), y)
                label_x, label_y = mid_x, y + 2.0
                centered = True
            doc.setFillColor(color)
            doc.setFont('Helvetica-Bold', 7)
            if centered:
                doc.drawCentredString(label_x, label_y, row['trainID'])
            else:
                doc.drawString(label_x, label_y, row['trainID'])

    doc.setDash([])
    doc.setStrokeColor(colors.HexColor('#334155'))
    doc.setLineWidth(0.8)
    doc.rect(left, bottom, right - left, top - bottom)
    doc.setFillColor(colors.HexColor('#17202a'))
    doc.setFont('Chinese', 11)
    doc.drawCentredString((left + right) / 2, 65, '时间（分钟，自规划起点计）')
    doc.saveState()
    doc.translate(35, (bottom + top) / 2)
    doc.rotate(90)
    doc.drawCentredString(0, 0, '车站及轨道')
    doc.restoreState()

    for index, (row, _) in enumerate(parsed):
        x = 200 + (index % 5) * 210
        legend_y = 845 - (index // 5) * 19
        color = colors.HexColor(palette[index % len(palette)])
        doc.setStrokeColor(color)
        doc.setLineWidth(2)
        doc.line(x, legend_y, x + 25, legend_y)
        doc.setFillColor(color)
        doc.setFont('Helvetica-Bold', 9)
        doc.drawString(x + 30, legend_y - 3, row['trainID'])
        doc.setFont('Helvetica', 8)
        doc.drawString(x + 51, legend_y - 3, f"{row['FromNode']}>{row['ToNode']}")
    doc.setFillColor(colors.HexColor('#475569'))
    doc.setFont('Chinese', 9)
    doc.drawCentredString(width / 2, 36, '下行：奇数轨道；上行：偶数轨道。b 为正线，t 为到发线。各站轨道展开间距为示意，不代表距离。')
    doc.save()
    print(f'Saved {OUTPUT}; {len(parsed)} trains, {sum(len(p) // 2 for _, p in parsed)} station visits.')


if __name__ == '__main__':
    main()
