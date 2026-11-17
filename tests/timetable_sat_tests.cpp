///////////////////////////////////////////////////////////////////////////
/// @file timetable_sat_tests.cpp
/// @brief 直接调用生产 C++ 模块，验收 SAT 编码、MILP 对照和完整加割闭环。
/// @details 所有新报告写入独立目录，不覆盖输入算例及既有验收结果。
///////////////////////////////////////////////////////////////////////////
#include "../augmentation line plan/augmentation line plan/planning_iteration.h"
#include "../augmentation line plan/augmentation line plan/timetable_sat.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

void RunAssumptionsTests(const std::filesystem::path& root, const std::filesystem::path& output);

namespace {
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
T Take(InputResult<T> result) {
    if (const auto* error = std::get_if<InputError>(&result))
        throw std::runtime_error(error->context + ": " + error->message);
    return std::move(std::get<T>(result));
}

/// @brief 比较业务结论，分别独立检查见证；未定不能算作一致性通过。
bool Compare(const TimetableData& data, const std::string& name, std::ostream& csv) {
    const auto milp = Take(SolveTimetableMilp(data, {30, false})); // 直接使用原 MILP 函数。
    const auto sat = Take(SolveTimetableSat(data, {30, false}));   // 同一输入对象，不另行构建。
    Require(milp.status != TimetableStatus::Undetermined && sat.status != TimetableStatus::Undetermined,
        name + ": 出现未定，对照未完成");
    Require(milp.status == sat.status, name + ": MILP 与 SAT 判定不一致");
    for (const auto* result : {&milp, &sat}) {
        Require(result->solution.has_value() == (result->status == TimetableStatus::Feasible), name + ": 见证状态不一致");
        if (result->solution) {
            const auto error = ValidateTimetableSolution(data, *result->solution);
            Require(!error, name + ": " + (error ? error->message : ""));
        }
    }
    csv << name << ',' << (sat.status == TimetableStatus::Feasible ? "feasible" : "infeasible")
        << ',' << milp.solver_status << ',' << sat.solver_status << ',' << sat.variable_count
        << ',' << sat.clause_count << ',' << sat.encoding_seconds << ',' << sat.solving_seconds
        << ',' << milp.solving_seconds << '\n';
    return sat.status == TimetableStatus::Feasible;
}

TimetableData EmptyData(int horizon = 8) {
    TimetableData data{}; // 后续小例只填写所测约束需要的数据，避免额外限制掩盖错误。
    data.planning_horizon = horizon;
    data.time_step_minutes = 1;
    data.track_headway = 1;
    return data;
}

/// @brief 添加一对到发事件及一次访问，轨道编号 0 在这些小例中表示共同轨道。
void AddVisit(TimetableData& data, TimeDomain arrival, TimeDomain departure) {
    const auto visit = data.visits.size(); // 新访问的稳定下标。
    const auto event = data.events.size(); // 新访问的到达事件下标，出发紧随其后。
    data.events.push_back({Event::Arrival, visit, arrival, std::nullopt});
    data.events.push_back({Event::Departure, visit, departure, std::nullopt});
    data.visits.push_back({static_cast<int>(visit), 0, event, event + 1, std::nullopt, {0}});
    data.activities.push_back({Activity::Stop, event, event + 1, 1});
}

void CheckSmallCases(std::ostream& csv) {
    // 以算术真值穷举固定时间差，覆盖正负反向下界以及 k=0/H 越界常量。
    int arithmetic_cases = 0;
    for (int before = 0; before <= 4; ++before) {
        for (int after = 0; after <= 4; ++after) {
            for (int duration = 0; duration <= 6; ++duration) {
                for (const auto kind : {Activity::Run, Activity::Stop}) {
                    auto data = EmptyData(4);
                    data.events = {{Event::Departure, 0, {before, before}, {}},
                                   {Event::Arrival, 0, {after, after}, {}}};
                    data.activities = {{kind, 0, 1, duration}};
                    const auto result = Take(SolveTimetableSat(data, {5, false}));
                    const bool expected = kind == Activity::Run ? after - before == duration : after - before >= duration;
                    Require(result.status == (expected ? TimetableStatus::Feasible : TimetableStatus::ProvenInfeasible),
                        "固定时间差算术验收失败");
                    ++arithmetic_cases;
                }
            }
        }
    }
    auto data = EmptyData(4);
    data.events = {{Event::Departure, 0, {0, 4}, {}}, {Event::Arrival, 0, {0, 4}, {}}};
    data.activities = {{Activity::Run, 0, 1, 4}};
    Require(Compare(data, "boundary_0_H", csv), "跨完整时域的运行应可行");
    data.activities[0].duration = 5;
    Require(!Compare(data, "run_exceeds_H", csv), "超过时域的运行应不可行");
    data.activities[0] = {Activity::Pass, 0, 1, 0};
    Require(Compare(data, "pass_equal", csv), "通过等式应可行");
    data.events[1].time_domain = {4, 4};
    data.events[0].time_domain = {0, 3};
    Require(!Compare(data, "pass_unequal", csv), "通过不能产生停站");
    data = EmptyData(0);
    data.events = {{Event::Arrival, 0, {0, 0}, {}}};
    Require(Compare(data, "zero_horizon", csv), "零时域边界处理失败");
    data = EmptyData();
    AddVisit(data, {1, 1}, {3, 3});
    AddVisit(data, {2, 2}, {4, 4});
    data.station_conflicts = {{0, 1, 1, 1, {0}}};
    Require(!Compare(data, "overlap_same_track", csv), "重叠占轨应不可行");
    data.visits[1].allowed_platform_indices.push_back(1);
    Require(Compare(data, "overlap_choose_other_track", csv), "选择另一轨道应恢复可行");
    data.visits[1].fixed_platform_index = 0;
    Require(!Compare(data, "fixed_track_forbids_escape", csv), "固定轨道约束未生效");
    data.visits[1].fixed_platform_index.reset();
    data.visits[1].allowed_platform_indices = {1};
    data.station_conflicts[0].common_platform_indices.clear();
    data.station_conflicts[0].arrival_headway = 2;
    Require(!Compare(data, "disjoint_tracks_still_headway", csv), "无共轨仍需到达间隔");
    data.station_conflicts[0].arrival_headway = 1;
    data.station_conflicts[0].departure_headway = 2;
    Require(!Compare(data, "departure_headway", csv), "出发间隔未生效");
    data.station_conflicts.clear();
    Require(Compare(data, "independent_direction", csv), "无冲突对的独立轨道应可行");
    data.visits[1].allowed_platform_indices.clear();
    Require(!Compare(data, "empty_tracks", csv), "无可用轨道应不可行");

    // 两车的实际到发初始均固定；只放宽第一辆车的后移窗口即可恢复可行。
    data = EmptyData();
    AddVisit(data, {3, 3}, {4, 4});
    AddVisit(data, {2, 2}, {3, 3});
    data.station_conflicts = {{0, 1, 1, 1, {0}}};
    Require(!Compare(data, "delay0_infeasible", csv), "零后移窗口应不可行");
    data.events[0].time_domain.upper = 4;
    data.events[1].time_domain.upper = 5;
    Require(Compare(data, "delay1_recovers", csv), "后移一单位应恢复可行");
    const auto delayed = Take(SolveTimetableSat(data, {5, false}));
    Require(delayed.solution->event_times[0] == 4 && delayed.solution->event_times[1] == 5,
        "恢复可行未实际使用后移窗口");
    auto damaged = *delayed.solution; // 人为破坏一个见证，确认独立检查器会拒绝。
    damaged.event_times[0] = 3;
    Require(ValidateTimetableSolution(data, damaged).has_value(), "检查器未发现同轨间隔违反");

    data = EmptyData(30);
    AddVisit(data, {0, 30}, {15, 15});
    data.events[0].kind = Event::StartOccupation;
    data.events[0].baseline_time = 10; // 占轨参考值不应当作固定时刻。
    data.activities[0] = {Activity::OriginOccupation, 0, 1, 15};
    Require(Compare(data, "origin_boundary_free", csv), "始发占轨应可调整到0");
    data.events[1].time_domain = {14, 14};
    Require(!Compare(data, "origin_occupation_short", csv), "始发占轨不足15应不可行");
    data.events[0] = {Event::Arrival, 0, {15, 15}, 15};
    data.events[1] = {Event::ReleaseTrack, 0, {0, 30}, 20};
    data.activities[0] = {Activity::DestinationOccupation, 0, 1, 15};
    Require(Compare(data, "destination_boundary_free", csv), "终到释放应可调整到30");
    data.events[0].time_domain = {16, 16};
    Require(!Compare(data, "destination_occupation_short", csv), "终到占轨超时域应不可行");

    data = EmptyData();
    for (int time : {1, 5, 2, 4}) {
        AddVisit(data, {time, time}, {time, time});
        data.activities.back().kind = Activity::Pass;
        data.activities.back().duration = 0;
    }
    data.station_conflicts = {{0, 2, 1, 1, {}}, {1, 3, 1, 1, {}}};
    // 两站中不同车使用不同轨道，以免同轨条件掩盖禁越行关系。
    data.visits[2].allowed_platform_indices = {1};
    data.visits[3].allowed_platform_indices = {1};
    data.activities.push_back({Activity::Run, 1, 2, 4});
    data.activities.push_back({Activity::Run, 5, 6, 2});
    Require(Compare(data, "overtaking_without_order_link", csv), "隔离测试的基础时间应可行");
    data.section_orders = {{0, 0, 1}};
    Require(!Compare(data, "overtaking_forbidden", csv), "禁越行约束未生效");

    Require(std::holds_alternative<InputError>(SolveTimetableSat(data, {-1, false})), "负预算未拒绝");
    Require(std::holds_alternative<InputError>(SolveTimetableSat(data,
        {std::numeric_limits<double>::quiet_NaN(), false})), "NaN预算未拒绝");
    data.events[0].time_domain = {3, 2};
    Require(std::holds_alternative<InputError>(SolveTimetableSat(data, {5, false})), "非法时间域未拒绝");
    std::cout << "PASS arithmetic cases=" << arithmetic_cases << "; hand-built MILP/SAT cases and witness rejection\n";
}

/// @brief 对原 OD 映射检查每轮客流，避免只比较求解状态而忽略外层解。
void CheckPassengers(const ProblemData& data, const MasterSolution& solution) {
    double objective = 0; // 以输入系数重算目标。
    std::vector<std::vector<double>> loads(data.candidates.size()); // 候选 -> 停车区段载客量。
    for (const auto& candidate : data.candidates)
        loads[candidate->index()].resize(candidate->line()->od_indices_by_stop_section().size());
    Require(solution.od_allocations.size() == data.ods.size(), "OD结果数量错误");
    for (const auto& allocation : solution.od_allocations) {
        const int od = allocation.od_index;
        double total = allocation.unmet_demand; // 已分配加未满足应等于需求。
        Require(total >= -1e-6, "未满足人数为负");
        objective += total * data.ods.at(od)->unmet_unit_penalty();
        for (const auto& flow : allocation.services) {
            Require(flow.passengers >= -1e-6 && std::find(solution.selected_candidate_indices.begin(),
                solution.selected_candidate_indices.end(), flow.candidate_index) != solution.selected_candidate_indices.end(),
                "未选候选承运客流");
            total += flow.passengers;
            const auto& services = data.od_services.at(od);
            const auto service = std::find_if(services.begin(), services.end(), [&](const auto& item) {
                return item.candidate->index() == flow.candidate_index;
            });
            Require(service != services.end(), "候选无法服务OD");
            objective += flow.passengers * service->travel_time;
            const auto& coverage = service->candidate->line()->od_indices_by_stop_section();
            for (std::size_t leg = 0; leg < coverage.size(); ++leg)
                if (std::find(coverage[leg].begin(), coverage[leg].end(), od) != coverage[leg].end())
                    loads[flow.candidate_index][leg] += flow.passengers;
        }
        Require(std::abs(total - data.ods.at(od)->demand()) < 1e-5, "OD需求不守恒");
    }
    for (const auto& candidate : data.candidates)
        for (double load : loads[candidate->index()])
            Require(load <= candidate->frequency() * candidate->rolling_stock()->seat_capacity() + 1e-5, "区段座位超载");
    Require(std::abs(objective - solution.objective_value) < 1e-5, "目标重算错误");
}

PlanningResult CheckIteration(const ProblemData& data, TimetableBackend backend,
    const std::filesystem::path& output, double expected_objective) {
    PlanningOptions options(data.parameters); // 此组验收完整集合割，不继承演示算例的提核模式。
    options.timetable_backend = backend;
    options.conflict_cut_mode = ConflictCutMode::FullSet;
    options.timetable.solver_logging = false;
    options.output_root = output;
    const auto result = RunPlanningIteration(data, options);
    Require(result.status == PlanningStatus::Feasible, "外层迭代未取得可行方案：" + result.reason);
    double previous = -1; // 每轮主问题最优目标不得随着有效割加入而下降。
    std::set<std::vector<int>> visited;
    std::vector<CandidateConflict> cuts;
    for (const auto& round : result.rounds) {
        Require(round.master && round.master->solution && round.timetable && round.timetable_data, "轮次结果缺失");
        const auto& master = *round.master->solution;
        Require(round.timetable->backend == backend, "实际调用了错误后端");
        Require(master.objective_value >= previous - 1e-5, "目标下降");
        previous = master.objective_value;
        Require(visited.insert(master.selected_candidate_indices).second, "加割后选择重复");
        CheckPassengers(data, master);
        for (const auto& cut : cuts)
            Require(!std::includes(master.selected_candidate_indices.begin(), master.selected_candidate_indices.end(),
                cut.begin(), cut.end()), "后续选择违反已有割");
        if (round.timetable->status == TimetableStatus::ProvenInfeasible) {
            Require(round.added_conflict == master.selected_candidate_indices && !round.added_conflict.empty(), "不是完整集合割");
            cuts.push_back(round.added_conflict);
        } else {
            Require(round.timetable->status == TimetableStatus::Feasible && round.added_conflict.empty(), "错误加割");
            Require(!ValidateTimetableSolution(*round.timetable_data, *round.timetable->solution), "最终见证错误");
        }
        Require(std::filesystem::file_size(result.output_directory / ("round_" + std::to_string(round.number) + ".txt")) > 0,
            "逐轮报告未保存");
    }
    Require(std::abs(previous - expected_objective) < 1e-5, "最终目标与基准不同");
    std::cout << "PASS iteration backend=" << (backend == TimetableBackend::Sat ? "sat" : "milp")
              << " rounds=" << result.rounds.size() << " objective=" << previous << '\n';
    return result;
}

void CheckComplex(const std::filesystem::path& directory, const std::filesystem::path& output, std::ostream& csv) {
    auto data = Take(LoadProblemData(directory)); // 基础数据只读；后移参数在内存副本中切换。
    const std::vector<int> active{0, 2, 4, 6, 8, 10};
    Require(data.candidates.size() == 12 && data.existing_trains.size() == 2, "复杂算例规模改变");
    std::vector<bool> feasible(64); // 当前后移上限下的完整组合真值表，用于验收割。
    for (int delay : {0, 5}) {
        data.parameters.existing_train_max_delay = delay;
        int count = 0; // 本组64组合的可行数量。
        for (unsigned mask = 0; mask < 64; ++mask) {
            MasterSolution selection{};
            for (unsigned bit = 0; bit < active.size(); ++bit)
                if (mask & (1u << bit)) selection.selected_candidate_indices.push_back(active[bit]);
            const auto timetable = Take(BuildTimetableData(data, selection));
            feasible[mask] = Compare(timetable, "complex_delay" + std::to_string(delay) + "_mask" + std::to_string(mask), csv);
            if (feasible[mask]) ++count;
        }
        Require(count == 36, "复杂算例可行数量偏离MILP基准");
        std::cout << "PASS 64 combinations delay=" << delay << ": 36 feasible / 28 infeasible / 0 unknown\n";
    }
    for (auto backend : {TimetableBackend::Milp, TimetableBackend::Sat}) {
        const auto run = CheckIteration(data, backend, output, 91700);
        Require(run.rounds.size() == 21, "复杂算例轮数偏离基准");
        Require(run.rounds.back().master->solution->selected_candidate_indices == std::vector<int>({0, 2, 6, 8}),
            "最终候选集合错误");
        for (const auto& round : run.rounds) {
            if (round.added_conflict.empty()) continue;
            unsigned cut_mask = 0; // 候选编号映射到6个比特，逐一核对该割排除的超集。
            for (int candidate : round.added_conflict) {
                const auto position = std::find(active.begin(), active.end(), candidate);
                Require(position != active.end(), "割包含未枚举候选");
                cut_mask |= 1u << static_cast<unsigned>(position - active.begin());
            }
            for (unsigned mask = 0; mask < 64; ++mask)
                if ((mask & cut_mask) == cut_mask) Require(!feasible[mask], "完整集合割排除了可行组合");
        }
    }
}

void CheckConfigurationAndUnknown(const std::filesystem::path& directory, const std::filesystem::path& output) {
    auto data = Take(LoadProblemData(directory));
    auto timetable = Take(BuildTimetableData(data, MasterSolution{})); // 零候选也包含既有图。
    const auto unknown = Take(SolveTimetableSat(timetable, {0, false}));
    Require(unknown.status == TimetableStatus::Undetermined && !unknown.solution && unknown.solver_status == 0,
        "SAT零预算必须未定");
    PlanningOptions options(data.parameters);
    options.timetable_backend = TimetableBackend::Sat;
    options.timetable.time_limit_seconds = 0;
    options.output_root = output;
    const auto run = RunPlanningIteration(data, options);
    Require(run.status == PlanningStatus::Undetermined && run.rounds.size() == 1
        && run.rounds[0].timetable->backend == TimetableBackend::Sat && run.rounds[0].added_conflict.empty(),
        "SAT未定生成了割或未保留未定状态");
    options.timetable.time_limit_seconds = 30;
    options.maximum_rounds = 1;
    const auto limited = RunPlanningIteration(data, options); // 两轮算例只能完成首轮加割。
    Require(limited.status == PlanningStatus::RoundLimit && limited.rounds.size() == 1
        && limited.rounds[0].added_conflict == CandidateConflict{0}, "SAT轮数限制丢失判定或冲突割");
    options.total_time_limit_seconds = 0;
    const auto no_budget = RunPlanningIteration(data, options);
    Require(no_budget.status == PlanningStatus::TimeLimit && no_budget.rounds.empty(), "SAT总预算限制未生效");

    // 通过真实CSV加载路径验证后端选择，不修改原始算例。
    const auto fixture = output / "csv_backend";
    std::filesystem::create_directory(fixture);
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".csv") std::filesystem::copy_file(entry.path(), fixture / entry.path().filename());
    const auto parameters = fixture / "parameters.csv";
    const auto write_backend = [&](const std::string& value, bool omit = false) {
        std::ifstream source(directory / "parameters.csv"); // 总从原表生成副本，避免非法值污染下一次测试。
        std::ofstream target(parameters);
        target.exceptions(std::ios::failbit | std::ios::badbit);
        std::string line;
        bool found = false; // 必须修改已有行，不能追加一个重复参数。
        Require(static_cast<bool>(std::getline(source, line)) && line.find("Meaning") != std::string::npos,
            "参数表缺少Meaning列");
        target << line << '\n';
        while (std::getline(source, line)) {
            if (line.starts_with("timetable_solver,")) {
                found = true;
                if (omit) continue;
                const auto value_begin = line.find(',') + 1; // 仅替换value列，保留unit和Meaning。
                const auto value_end = line.find(',', value_begin);
                Require(value_end != std::string::npos, "求解器参数行格式错误");
                line.replace(value_begin, value_end - value_begin, value);
            }
            target << line << '\n';
        }
        Require(found, "原始参数表缺少求解器选择");
    };
    for (int selector : {1, 2}) {
        write_backend(std::to_string(selector));
        auto configured = Take(LoadProblemData(fixture));
        const auto expected = selector == 1 ? TimetableBackend::Milp : TimetableBackend::Sat;
        Require(PlanningOptions(configured.parameters).timetable_backend == expected, "CSV数字未正确映射后端");
        configured.parameters.planning_output_root = output.string();
        const auto selected = RunPlanningIteration(configured); // 与正式main相同的无覆盖入口。
        Require(selected.status == PlanningStatus::Feasible && selected.rounds.size() == 2, "数字配置下的闭环未完成");
        for (const auto& round : selected.rounds)
            Require(round.timetable && round.timetable->backend == expected, "实际求解后端与CSV参数不符");
    }
    for (const std::string value : {"0", "3", "-1", "sat", "2.5"}) {
        write_backend(value);
        Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "非法后端未被拒绝：" + value);
    }
    write_backend("1", true);
    Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "缺失求解器参数被静默接受");
    std::cout << "PASS limits, Meaning column, CSV 1=MILP/2=SAT production entry, invalid/missing selector rejection\n";
}
void CheckPublishedSeconds(const std::filesystem::path& directory, const std::filesystem::path& output,
                           std::ostream& csv) {
    const auto fixture = output / "published_seconds";
    std::filesystem::create_directory(fixture);
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".csv") std::filesystem::copy_file(entry.path(), fixture / entry.path().filename());
    const auto write_parameters = [&](int mode, bool duplicate_unit = false) {
        std::ifstream source(directory / "parameters.csv");
        std::ofstream target(fixture / "parameters.csv");
        std::string line;
        while (std::getline(source, line)) {
            if (line.starts_with("time_step_minutes,")) {
                target << "time_step_seconds,1,second_per_time_step,Test seconds\n";
                if (!duplicate_unit) continue;
            }
            target << line << '\n';
        }
        target << "existing_timing_mode," << mode << ",mode,Test published timing\n";
    };
    // Synthetic fixture deliberately differs from infrastructure runtime and 4-step candidate dwell.
    {
        std::ofstream timetable(fixture / "timetable.csv");
        timetable << "trainID,FromNode,ToNode,Paths,Times\n"
                  << "E0,C,A,C-t2;C-t2;B-t2;B-t2;A-t2;A-t2,16;32;34;51\n";
    }
    write_parameters(0);
    Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "Legacy timing accepted inconsistent published times");
    write_parameters(1, true);
    Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "Conflicting time units accepted");
    write_parameters(2);
    Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "Invalid existing timing mode accepted");
    write_parameters(1);
    const auto problem = Take(LoadProblemData(fixture));
    Require(problem.parameters.SecondsPerStep() == 1 && problem.parameters.minimum_stop_occupation == 4,
            "Seconds input changed candidate policy");
    const auto data = Take(BuildTimetableData(problem, MasterSolution{}));
    Require(data.SecondsPerStep() == 1, "Seconds unit lost in timetable conversion");
    std::vector<int> runtimes, dwells, published;
    for (const auto& a : data.activities) {
        if (a.kind == Activity::Run) runtimes.push_back(a.duration);
        if (a.kind == Activity::Stop) dwells.push_back(a.duration);
    }
    for (const auto& e : data.events)
        if (e.kind == Event::Arrival || e.kind == Event::Departure) published.push_back(*e.baseline_time);
    Require(runtimes == std::vector<int>({16, 17}) && dwells == std::vector<int>({2})
            && published == std::vector<int>({16, 32, 34, 51}), "Published timing was changed");
    Require(Compare(data, "published_seconds", csv), "Published second-resolution timetable rejected");
    std::cout << "PASS second-resolution published running/dwell times, unchanged candidate policy, MILP/SAT witnesses\n";
}
} // namespace

// 与直接核验收复用客流、分段容量和目标重算检查，避免两套不同的校验口径。
void CheckMasterPassengers(const ProblemData& data, const MasterSolution& solution) {
    CheckPassengers(data, solution);
}

int wmain(int argc, wchar_t* argv[]) {
    try {
        Require(argc == 2, "用法：timetable_sat_tests.exe <项目根目录>");
        const auto root = std::filesystem::absolute(argv[1]);
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        const auto output = root / "tmp" / ("sat_acceptance_" + std::to_string(stamp));
        std::filesystem::create_directories(output);
        std::ofstream csv(output / "comparison.csv");
        csv.exceptions(std::ios::failbit | std::ios::badbit);
        csv << "case,status,copt_status,cadical_status,variables,clauses,encoding_seconds,sat_seconds,milp_seconds\n";
        CheckSmallCases(csv);
        CheckPublishedSeconds(root / "augmentation line plan" / "Test_data", output, csv);
        CheckComplex(root / "augmentation line plan" / "Test_data_complex", output, csv);
        const auto small = Take(LoadProblemData(root / "augmentation line plan" / "Test_data"));
        for (auto backend : {TimetableBackend::Milp, TimetableBackend::Sat}) {
            const auto run = CheckIteration(small, backend, output, 3400);
            Require(run.rounds.size() == 2 && run.rounds[0].added_conflict == CandidateConflict{0}
                && run.rounds[1].master->solution->selected_candidate_indices == std::vector<int>{2}, "两轮闭环错误");
        }
        const auto baseline = Take(LoadProblemData(root / "augmentation line plan" / "Data"));
        for (auto backend : {TimetableBackend::Milp, TimetableBackend::Sat})
            CheckIteration(baseline, backend, output, 144900);
        CheckConfigurationAndUnknown(root / "augmentation line plan" / "Test_data", output);
        RunAssumptionsTests(root, output);
        csv.close();
        std::cout << "ALL C++ SAT ACCEPTANCE TESTS PASSED\nREPORT_DIRECTORY=" << output.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
