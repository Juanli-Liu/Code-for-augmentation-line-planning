"""Validate the input CSV relationships and independently derive candidate times."""

from collections import Counter, defaultdict
from itertools import product

from plot_existing_timetable import derive_visit_times, read_csv
from validate_existing_timetable import main as validate_timetable


def main():
    validate_timetable()
    parameters = {r['parameter']: r['value'] for r in read_csv('parameters.csv')}
    numeric = lambda key: int(parameters[key])
    assert numeric('existing_train_max_delay') >= 0
    assert numeric('startup_additional_time') == 2
    assert numeric('stopping_additional_time') == 3
    assert numeric('minimum_stop_platform_occupation_time') == 4
    terminal_occupation = numeric('minimum_terminal_platform_occupation_time')
    assert terminal_occupation >= 0
    stations = {r['station_name']: r['station_id'] for r in read_csv('stations.csv')}
    platforms = {r['platform_id']: r for r in read_csv('platforms.csv')}
    assert len(platforms) == len(read_csv('platforms.csv'))
    sections = {(r['from_station'], r['to_station']): r for r in read_csv('sections.csv')}
    lines = read_csv('lines.csv')
    allowed = read_csv('line_allowed_platforms.csv')
    assert len({r['line_id'] for r in lines}) == len(lines)
    allocations = defaultdict(list)
    for r in allowed:
        allocations[r['line_id'], r['station_name']].append(r)
    expected_keys = set()
    plans = defaultdict(set)
    derived = {}
    for line in lines:
        route = tuple(line['stations'].split(';'))
        stop = tuple(map(int, line['stop_plan'].split(';')))
        assert len(route) == len(stop) and stop[0] == stop[-1] == 1
        assert all(s in stations for s in route)
        assert all(v in (0, 1) for v in stop)
        assert stop not in plans[route], (route, stop, 'duplicate stop plan')
        plans[route].add(stop)
        down = int(stations[route[0]]) < int(stations[route[-1]])
        for station, stops_here in zip(route, stop):
            key = line['line_id'], station
            expected_keys.add(key)
            assert len(allocations[key]) == 1, key
            allocation = allocations[key][0]
            assert int(allocation['stop_plan']) == stops_here
            names = (['t1', 't3'] if down else ['t2', 't4']) if stops_here else (['b1'] if down else ['b2'])
            assert allocation['platform_name'].split(';') == names
            ids = allocation['platform_ids'].split(';')
            assert len(ids) == len(names) and len(set(ids)) == len(ids)
            for pid, name in zip(ids, names):
                assert platforms[pid]['station_id'] == stations[station]
                assert platforms[pid]['platform_name'] == name
        # Actual event-to-event runtime; additions depend on BOTH endpoint stop flags.
        runtime = [int(sections[a, b]['pure_running_time'])
                   + numeric('startup_additional_time') * stop[i]
                   + numeric('stopping_additional_time') * stop[i + 1]
                   for i, (a, b) in enumerate(zip(route, route[1:]))]
        # Task time is actual first departure to final arrival, excluding terminal occupation.
        task = sum(runtime) + numeric('minimum_stop_platform_occupation_time') * sum(stop[1:-1])
        derived[line['line_id']] = (route, stop, runtime, task)
    assert set(allocations) == expected_keys and len(allowed) == len(expected_keys)
    expected_routes = {tuple(s.split(';')) for s in ('A;B;C;D;E', 'E;D;C;B;A', 'B;C;D', 'D;C;B')}
    assert set(plans) == expected_routes
    for route, actual in plans.items():
        expected = {(1,) + middle + (1,) for middle in product((0, 1), repeat=len(route) - 2)}
        assert actual == expected, (route, 'incomplete enumeration')

    counts = Counter()
    occupied = Counter()
    for train in read_csv('timetable.csv'):
        paths = [s.rsplit('-', 1) for s in train['Paths'].split(';') if s]
        times = derive_visit_times(train, terminal_occupation)
        for i in range(0, len(paths), 2):
            station, track = paths[i]
            occupied[stations[station], track] += (times[i + 1] - times[i] if track.startswith('t')
                                                  else numeric('minimum_pass_platform_occupation_time'))
            if i + 2 < len(paths):
                counts[station, paths[i + 2][0]] += 1
    for p in platforms.values():
        assert occupied[p['station_id'], p['platform_name']] <= int(p['total_available_time'])
    frequencies = list(map(int, parameters['candidate_frequencies'].split(';')))
    feasible_twice = 0
    for line_id, (route, _, _, _) in derived.items():
        residual = min(int(sections[q]['maximum_train_count']) - counts[q]
                       for q in zip(route, route[1:]))
        feasible_twice += 2 in frequencies and residual >= 2
    for od in read_csv('ods.csv'):
        services = []
        for line_id, (route, stop, runtime, _) in derived.items():
            origin, dest = od['origin_station'], od['destination_station']
            if origin not in route or dest not in route:
                continue
            a, b = route.index(origin), route.index(dest)
            if a < b and stop[a] and stop[b]:
                travel = sum(runtime[a:b]) + numeric('minimum_stop_platform_occupation_time') * sum(stop[a + 1:b])
                assert travel > 0
                services.append((line_id, travel))
        assert services, ('OD has no service', od['od_id'])
        shortest = min(t for _, t in services)
        print(f'OD {od["od_id"]}: lines={len(services)}, shortest_travel_time={shortest}, '
              f'unmet_unit_penalty={numeric("unmet_demand_penalty_factor") * shortest}')
    print(f'PASS: {len(lines)} fully enumerated lines, {len(allowed)} line/station allocations, '
          f'{len(lines) * len(frequencies) * 2} candidate triples before capacity pruning.')
    print(f'Frequency 2 passes the individual section-capacity check for {feasible_twice}/{len(lines)} lines.')
    print('This does not assert joint insertability of arbitrary candidate combinations.')


if __name__ == '__main__':
    main()
