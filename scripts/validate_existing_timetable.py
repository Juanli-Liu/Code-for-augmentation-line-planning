"""Check the published timetable against infrastructure and first-layer constraints."""

from collections import Counter, defaultdict
from itertools import combinations

from plot_existing_timetable import derive_visit_times, read_csv


def main():
    parameters = {r['parameter']: int(r['value']) for r in read_csv('parameters.csv')
                  if ';' not in r['value']}
    stations = {r['station_name']: int(r['station_id']) for r in read_csv('stations.csv')}
    platforms = {(int(r['station_id']), r['platform_name']) for r in read_csv('platforms.csv')}
    section_rows = {(r['from_station'], r['to_station']): r
                    for r in read_csv('sections.csv')}
    sections = {q: int(r['maximum_train_count']) for q, r in section_rows.items()}
    visits = defaultdict(list)
    traversals = defaultdict(list)
    errors = []
    train_ids = set()
    direction_counts = Counter()
    event_count = 0
    times_seen = []
    for row in read_csv('timetable.csv'):
        train = row['trainID']
        assert train not in train_ids, f'Duplicate train: {train}'
        train_ids.add(train)
        paths = [p.rsplit('-', 1) for p in row['Paths'].split(';') if p]
        times = derive_visit_times(row, parameters['minimum_terminal_platform_occupation_time'])
        assert len(paths) == len(times) and len(paths) % 2 == 0, train
        assert paths[0][0] == row['FromNode'] and paths[-1][0] == row['ToNode'], train
        assert all(0 <= t <= parameters['planning_horizon'] for t in times), train
        assert all(a <= b for a, b in zip(times, times[1:])), train
        direction = section_rows[paths[0][0], paths[2][0]]['direction']
        assert direction in ('up', 'down'), (train, direction)
        direction_counts[direction] += 1
        times_seen.extend(times)
        for i in range(0, len(paths), 2):
            station, track = paths[i]
            assert paths[i] == paths[i + 1], (train, station)
            assert (stations[station], track) in platforms, (train, station, track)
            assert int(track[1:]) % 2 == (direction == 'down'), (train, station, 'wrong direction')
            arrival, departure = times[i:i + 2]
            terminal = i == 0 or i == len(paths) - 2
            if terminal:
                assert track.startswith('t'), (train, station, 'endpoint on main track')
            elif track.startswith('t'):
                assert departure - arrival >= parameters['minimum_stop_platform_occupation_time'], (train, station)
            else:
                assert arrival == departure, (train, station, 'pass duration')
            visits[station, direction].append((train, track, arrival, departure, i == 0, i == len(paths) - 2))
            event_count += 1
            if i + 2 < len(paths):
                nxt = paths[i + 2][0]
                assert (station, nxt) in sections, (train, station, nxt)
                assert section_rows[station, nxt]['direction'] == direction, (train, station, nxt)
                assert times[i + 2] > departure, (train, station, nxt)
                next_track = paths[i + 2][1]
                expected_runtime = int(section_rows[station, nxt]['pure_running_time'])
                if track.startswith('t'):
                    expected_runtime += parameters['startup_additional_time']
                if next_track.startswith('t'):
                    expected_runtime += parameters['stopping_additional_time']
                assert times[i + 2] - departure == expected_runtime, (
                    train, station, nxt, 'pure time + startup/stopping additions mismatch')
                traversals[station, nxt].append((train, departure, times[i + 2]))

    for (station, _), events in visits.items():
        for first, second in combinations(events, 2):
            a, b = first, second
            if not a[4] and not b[4] and abs(a[2] - b[2]) < parameters['arrival_headway']:
                errors.append(f'{station}: arrival headway, trains {a[0]}/{b[0]}')
            if not a[5] and not b[5] and abs(a[3] - b[3]) < parameters['departure_headway']:
                errors.append(f'{station}: departure headway, trains {a[0]}/{b[0]}')
            if a[1] == b[1]:
                a, b = sorted((a, b), key=lambda item: item[2])
                if b[2] - a[3] < parameters['track_headway']:
                    errors.append(f'{station}-{a[1]}: occupation gap, trains {a[0]}/{b[0]}')

    for section, trains in traversals.items():
        if len(trains) > sections[section]:
            errors.append(f'{section}: count exceeds capacity')
        for a, b in combinations(trains, 2):
            if (a[1] - b[1]) * (a[2] - b[2]) <= 0:
                errors.append(f'{section}: overtaking or simultaneous events, trains {a[0]}/{b[0]}')
    assert not errors, '\n'.join(errors)
    print(f'PASS: {len(train_ids)} trains ({dict(direction_counts)}), {event_count} station visits, '
          f'events within {min(times_seen)}..{max(times_seen)} of H={parameters["planning_horizon"]}.')
    print('Checked fixed tracks, stop/pass times, all-pair actual arrival/departure headways, '
          'track separation, section running times, capacities, and no overtaking.')
    for section in sections:
        count = len(traversals[section])
        print(f'{section[0]}->{section[1]}: existing={count}, limit={sections[section]}, remaining={sections[section] - count}')


if __name__ == '__main__':
    main()
