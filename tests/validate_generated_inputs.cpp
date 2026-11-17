#include "../augmentation line plan/augmentation line plan/input_data.h"
#include "../augmentation line plan/augmentation line plan/master_problem.h"
#include "../augmentation line plan/augmentation line plan/timetable_data.h"
#include "../augmentation line plan/augmentation line plan/timetable_solution.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <variant>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root(argv[1]);
    std::ofstream report(root / "background_validation.csv");
    report << "case,loader,background_witness,existing_trains,candidates,events,conflicts\n";
    int checked = 0, failed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory() || !std::filesystem::exists(entry.path()/"parameters.csv")) continue;
        try {
            auto loaded = LoadProblemData(entry.path());
            if (std::holds_alternative<InputError>(loaded)) throw std::runtime_error("loader");
            auto& data = std::get<ProblemData>(loaded);
            auto built = BuildTimetableData(data, MasterSolution{});
            if (std::holds_alternative<InputError>(built)) throw std::runtime_error("builder");
            const auto& tt = std::get<TimetableData>(built);
            TimetableSolution witness;
            for (const auto& event : tt.events) witness.event_times.push_back(event.baseline_time.value());
            for (const auto& visit : tt.visits) witness.visit_platforms.push_back(visit.fixed_platform_index.value());
            for (const auto& conflict : tt.station_conflicts) {
                const auto& a = tt.visits.at(conflict.first_visit_index);
                const auto& b = tt.visits.at(conflict.second_visit_index);
                witness.orders.push_back({
                    witness.event_times[a.arrival_side_event_index] <= witness.event_times[b.arrival_side_event_index],
                    witness.event_times[a.departure_side_event_index] <= witness.event_times[b.departure_side_event_index]});
            }
            if (ValidateTimetableSolution(tt, witness)) throw std::runtime_error("witness");
            report << entry.path().filename().string() << ",PASS,PASS," << data.existing_trains.size()
                   << ',' << data.candidates.size() << ',' << tt.events.size() << ',' << tt.station_conflicts.size() << '\n';
            ++checked;
        } catch (...) {
            // Do not print input records or exception messages from confidential inputs.
            report << entry.path().filename().string() << ",FAIL,FAIL,,,,\n";
            ++failed;
        }
    }
    std::cout << "validated=" << checked << " failed=" << failed << '\n';
    return failed || !checked ? 1 : 0;
}
