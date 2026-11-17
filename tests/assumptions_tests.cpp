///////////////////////////////////////////////////////////////////////////
/// @file assumptions_tests.cpp
/// @brief 真实 COPT/CaDiCaL 的直接核、独立复核、加割及基线对照。
///////////////////////////////////////////////////////////////////////////
#include "../augmentation line plan/augmentation line plan/planning_iteration.h"
#include "../augmentation line plan/augmentation line plan/timetable_sat.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

void CheckMasterPassengers(const ProblemData& data, const MasterSolution& solution);

namespace {
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("assumptions: " + message);
}

template <typename T>
T Take(InputResult<T> result) {
    if (const auto* error = std::get_if<InputError>(&result))
        throw std::runtime_error(error->context + ": " + error->message);
    return std::move(std::get<T>(result));
}

std::string SetText(const std::vector<int>& candidates) {
    std::ostringstream output;
    for (int candidate : candidates) output << candidate << ';';
    return output.str();
}

TimetableData Build(const ProblemData& data, const std::vector<int>& candidates) {
    MasterSolution selection{};
    selection.selected_candidate_indices = candidates;
    return Take(BuildTimetableData(data, selection));
}

/// @brief 构造最小事件网络以隔离某类约束，仍调用生产编码和真实求解器。
TimetableData SmallData() {
    TimetableData data{};
    data.planning_horizon = 8;
    data.time_step_minutes = 1;
    data.track_headway = 1;
    return data;
}

std::size_t AddVisit(TimetableData& data, int train_index, std::optional<int> candidate,
                     int arrival, int departure, int track = 0) {
    const auto visit = data.visits.size();
    const auto event = data.events.size();
    data.events.push_back({Event::Arrival, visit, {arrival, arrival}, {}});
    data.events.push_back({Event::Departure, visit, {departure, departure}, {}});
    data.visits.push_back({train_index, 0, event, event + 1, {}, {track}});
    data.activities.push_back({Activity::Stop, event, event + 1, 1});
    auto [it, inserted] = data.trains.emplace(train_index, TimetableTrain{Direction::Down, candidate, {}});
    Require(it->second.candidate_index == candidate, "手工列车归属错误");
    it->second.visit_indices.push_back(visit);
    if (candidate && std::find(data.selected_candidate_indices.begin(), data.selected_candidate_indices.end(), *candidate)
        == data.selected_candidate_indices.end()) {
        data.selected_candidate_indices.push_back(*candidate);
        std::sort(data.selected_candidate_indices.begin(), data.selected_candidate_indices.end());
    }
    return visit;
}

void RequireCore(const TimetableData& data, const std::vector<int>& expected, const std::string& name) {
    const auto direct = Take(SolveTimetableSat(data, {30, false, true}));
    const auto ordinary = Take(SolveTimetableSat(data, {30, false}));
    const auto milp = Take(SolveTimetableMilp(data, {30, false}));
    Require(direct.status == TimetableStatus::ProvenInfeasible && direct.status == ordinary.status
        && direct.status == milp.status, name + " 三后端判定不一致");
    Require(direct.candidate_core && *direct.candidate_core == expected, name + " 核归属错误");
    for (const auto& mapping : direct.candidate_assumptions)
        Require(!mapping.train_indices.empty() && mapping.literal > 1 && mapping.guarded_clause_count > 0,
            name + " 激活映射缺失");
}

void CheckGuardCases() {
    // ① 单候选自身矛盾不能泄漏成背景矛盾；不相关候选不应进入这个唯一核。
    auto data = SmallData();
    AddVisit(data, 10, 2, 1, 2);
    AddVisit(data, 20, 9, 4, 5, 1);
    data.activities[0].duration = 2;
    RequireCore(data, {2}, "候选自身停站");
    data.activities[0] = {Activity::Run, 0, 1, 2};
    RequireCore(data, {2}, "运行等式");
    data.activities[0] = {Activity::Pass, 0, 1, 0};
    RequireCore(data, {2}, "通过等式");
    for (auto kind : {Activity::OriginOccupation, Activity::DestinationOccupation}) {
        data.activities[0] = {kind, 0, 1, 2};
        RequireCore(data, {2}, "端点占轨");
    }
    data.activities[0].duration = 1;
    data.visits[0].allowed_platform_indices.clear();
    RequireCore(data, {2}, "空轨道集合");

    // ② 同候选多车、不同候选、既有与新增三种关系，分别对应一个、两个、一个激活文字。
    for (int relation = 0; relation < 3; ++relation) {
        data = SmallData();
        AddVisit(data, 10, relation == 2 ? std::nullopt : std::optional<int>(2), 1, 3);
        AddVisit(data, 20, relation == 0 ? 2 : 4, 2, 4);
        AddVisit(data, 30, 9, 6, 7, 2);
        data.station_conflicts = {{0, 1, 1, 1, {0}}};
        const std::vector<int> expected = relation == 0 ? std::vector<int>{2}
            : relation == 1 ? std::vector<int>{2, 4} : std::vector<int>{4};
        RequireCore(data, expected, "同轨参与者");
        data.visits[1].allowed_platform_indices = {1};
        data.station_conflicts[0].common_platform_indices.clear();
        data.station_conflicts[0].arrival_headway = 2;
        RequireCore(data, expected, "到达间隔参与者");
        data.station_conflicts[0].arrival_headway = 1;
        data.station_conflicts[0].departure_headway = 2;
        RequireCore(data, expected, "出发间隔参与者");
    }

    // ③ 禁越行关联两端冲突；两车分轨，排除占轨冲突对测试的干扰。
    data = SmallData();
    AddVisit(data, 10, 2, 1, 2, 0);
    AddVisit(data, 10, 2, 6, 7, 0);
    AddVisit(data, 20, 4, 2, 3, 1);
    AddVisit(data, 20, 4, 5, 6, 1);
    AddVisit(data, 30, 9, 0, 1, 2);
    data.activities.push_back({Activity::Run, 1, 2, 4});
    data.activities.push_back({Activity::Run, 5, 6, 2});
    data.station_conflicts = {{0, 2, 1, 1, {}}, {1, 3, 1, 1, {}}};
    Require(Take(SolveTimetableSat(data, {30, false, true})).status == TimetableStatus::Feasible,
        "禁越行测试基础图应可行");
    data.section_orders = {{0, 0, 1}};
    RequireCore(data, {2, 4}, "禁越行参与者");

    // ④ 背景矛盾产生空核；不能误归给一个可行候选。
    data = SmallData();
    AddVisit(data, 10, {}, 1, 1);
    AddVisit(data, 20, 9, 4, 5);
    RequireCore(data, {}, "背景空核");
    const auto unknown = Take(SolveTimetableSat(data, {0, false, true}));
    Require(unknown.status == TimetableStatus::Undetermined && !unknown.candidate_core, "未定不应有核");
    data.trains.at(20).candidate_index = 99;
    Require(std::holds_alternative<InputError>(SolveTimetableSat(data, {30, false, true})), "错误归属未被拒绝");
    std::cout << "PASS assumptions ownership, same/multi-candidate guards, activities, tracks, headways, section order, empty core\n";
}

unsigned Mask(const std::vector<int>& candidates) {
    unsigned mask = 0;
    for (int candidate : candidates) {
        Require(candidate >= 0 && candidate <= 10 && candidate % 2 == 0, "复杂算例候选编号错误");
        mask |= 1u << (candidate / 2);
    }
    return mask;
}

/// @brief 所有核都由原构建器重建，普通SAT与MILP复核；不得用缩减搜索代替直接核。
std::vector<bool> CheckAllSubsets(ProblemData& data, std::ostream& csv) {
    std::vector<bool> feasible(64);
    for (int delay : {0, 5}) {
        data.parameters.existing_train_max_delay = delay;
        int feasible_count = 0;
        for (unsigned mask = 0; mask < 64; ++mask) {
            std::vector<int> selected;
            for (int bit = 0; bit < 6; ++bit) if (mask & (1u << bit)) selected.push_back(2 * bit);
            const auto timetable = Build(data, selected);
            const auto direct = Take(SolveTimetableSat(timetable, {30, false, true}));
            const auto full = Take(SolveTimetableSat(timetable, {30, false}));
            const auto milp = Take(SolveTimetableMilp(timetable, {30, false}));
            Require(direct.status != TimetableStatus::Undetermined && direct.status == full.status
                && direct.status == milp.status, "全子集三方判定不一致");
            feasible[mask] = direct.status == TimetableStatus::Feasible;
            feasible_count += feasible[mask] ? 1 : 0;
            if (feasible[mask]) {
                Require(direct.solution && !ValidateTimetableSolution(timetable, *direct.solution), "见证错误");
                Require(!direct.candidate_core, "可行时不应有核");
            } else {
                Require(direct.candidate_core && !direct.candidate_core->empty(), "背景可行时不应出现空核");
                Require((Mask(*direct.candidate_core) & mask) == Mask(*direct.candidate_core), "核超出原集合");
                const auto rebuilt = Build(data, *direct.candidate_core);
                Require(Take(SolveTimetableSat(rebuilt, {30, false})).status == TimetableStatus::ProvenInfeasible,
                    "直接核普通SAT复核失败");
                Require(Take(SolveTimetableMilp(rebuilt, {30, false})).status == TimetableStatus::ProvenInfeasible,
                    "直接核MILP复核失败");
            }
            csv << delay << ',' << mask << ',' << (feasible[mask] ? "SAT" : "UNSAT") << ','
                << (direct.candidate_core ? SetText(*direct.candidate_core) : "") << ','
                << direct.encoding_seconds << ',' << direct.solving_seconds << ',' << direct.core_extraction_seconds << '\n';
        }
        Require(feasible_count == 36, "全子集可行数量改变");
    }
    std::cout << "PASS assumptions 128 subsets: MILP/full SAT/direct SAT agree; all UNSAT cores independently rechecked\n";
    return feasible;
}

/// @brief 保存逐轮可比较指标；业务细节仍由生产报告输出。
void WriteMetrics(const std::string& name, const std::string& mode, const PlanningResult& run,
                  std::ostream& totals, std::ostream& rounds) {
    std::size_t cuts = 0, cut_size = 0;
    double master = 0, encoding = 0, search = 0, extraction = 0, verification = 0;
    for (const auto& round : run.rounds) {
        const auto& result = *round.timetable;
        cuts += !round.added_conflict.empty();
        cut_size += round.added_conflict.size();
        master += round.master_seconds;
        encoding += result.encoding_seconds;
        search += result.solving_seconds;
        extraction += result.core_extraction_seconds;
        verification += round.core_verification_seconds;
        rounds << name << ',' << mode << ',' << round.number << ','
            << SetText(round.master->solution->selected_candidate_indices) << ','
            << (result.status == TimetableStatus::Feasible ? "SAT" : "UNSAT") << ','
            << (result.candidate_core ? SetText(*result.candidate_core) : "") << ','
            << (round.core_verification ? (round.core_verification->status == TimetableStatus::ProvenInfeasible
                ? "UNSAT" : round.core_verification->status == TimetableStatus::Feasible ? "SAT" : "UNKNOWN") : "not_run") << ','
            << SetText(round.added_conflict) << ',' << round.core_fallback_reason << ','
            << round.master_seconds << ',' << result.encoding_seconds << ',' << result.solving_seconds << ','
            << result.core_extraction_seconds << ',' << round.core_verification_seconds << ',' << round.elapsed_seconds << '\n';
    }
    totals << name << ',' << mode << ',' << run.rounds.size() << ',' << cuts << ',' << cut_size << ','
        << (cuts ? static_cast<double>(cut_size) / cuts : 0) << ','
        << run.rounds.back().master->solution->objective_value << ',' << master << ',' << encoding << ',' << search << ','
        << extraction << ',' << verification << ',' << run.elapsed_seconds << '\n';
}

void CheckRuns(const ProblemData& data, const std::string& name, double objective,
               const std::filesystem::path& output, const std::vector<bool>* truth,
               std::ostream& totals, std::ostream& rounds) {
    for (auto mode : {ConflictCutMode::FullSet, ConflictCutMode::Assumptions}) {
        PlanningOptions options(data.parameters);
        options.timetable_backend = TimetableBackend::Sat;
        options.conflict_cut_mode = mode;
        options.output_root = output / name;
        const auto run = RunPlanningIteration(data, options);
        Require(run.status == PlanningStatus::Feasible, name + " 迭代未完成：" + run.reason);
        Require(std::abs(run.rounds.back().master->solution->objective_value - objective) < 1e-5, "最终目标改变");
        std::vector<CandidateConflict> cuts;
        for (const auto& round : run.rounds) {
            const auto& selected = round.master->solution->selected_candidate_indices;
            CheckMasterPassengers(data, *round.master->solution);
            for (const auto& cut : cuts)
                Require(!std::includes(selected.begin(), selected.end(), cut.begin(), cut.end()), "后续方案违反已有割");
            if (round.added_conflict.empty()) {
                Require(round.timetable->solution && !ValidateTimetableSolution(*round.timetable_data, *round.timetable->solution),
                    "最终可行见证失败");
                continue;
            }
            Require(round.timetable->status == TimetableStatus::ProvenInfeasible, "非UNSAT加割");
            if (mode == ConflictCutMode::Assumptions) {
                Require(round.core_fallback_reason.empty() && round.core_verification
                    && round.core_verification->status == TimetableStatus::ProvenInfeasible, "正常提核发生回退");
                Require(round.timetable->candidate_core && round.added_conflict == *round.timetable->candidate_core,
                    "实际割不是直接核");
            } else Require(round.added_conflict == selected, "基线不是完整集合割");
            if (truth) {
                const auto cut_mask = Mask(round.added_conflict);
                for (unsigned mask = 0; mask < truth->size(); ++mask)
                    if ((mask & cut_mask) == cut_mask) Require(!truth->at(mask), "新割排除了已知可行组合");
            }
            cuts.push_back(round.added_conflict);
        }
        const std::string mode_name = mode == ConflictCutMode::FullSet ? "full_set" : "assumptions_direct";
        WriteMetrics(name, mode_name, run, totals, rounds);
        std::cout << "PASS " << name << ' ' << mode_name << " rounds=" << run.rounds.size()
            << " objective=" << objective << " seconds=" << run.elapsed_seconds << '\n';
    }
}

void CheckLimitsAndCsv(const std::filesystem::path& directory, const std::filesystem::path& output) {
    auto data = Take(LoadProblemData(directory));
    PlanningOptions options(data.parameters);
    options.timetable_backend = TimetableBackend::Sat;
    options.conflict_cut_mode = ConflictCutMode::Assumptions;
    options.output_root = output / "limits";
    options.core_verification_time_limit_seconds = 0;
    const auto fallback = RunPlanningIteration(data, options);
    Require(fallback.status == PlanningStatus::Feasible && fallback.rounds.size() == 2
        && fallback.rounds[0].added_conflict == CandidateConflict{0}
        && !fallback.rounds[0].core_fallback_reason.empty(), "复核零预算未回退完整集合");
    options.core_verification_time_limit_seconds = 1e-12; // 重建消耗预算后，真实SAT调用以零剩余预算返回UNKNOWN。
    const auto verification_unknown = RunPlanningIteration(data, options);
    Require(verification_unknown.status == PlanningStatus::Feasible
        && verification_unknown.rounds[0].core_verification
        && verification_unknown.rounds[0].core_verification->status == TimetableStatus::Undetermined
        && verification_unknown.rounds[0].added_conflict == CandidateConflict{0}
        && !verification_unknown.rounds[0].core_fallback_reason.empty(), "复核UNKNOWN未回退完整集合");
    options.timetable.time_limit_seconds = 0;
    const auto unknown = RunPlanningIteration(data, options);
    Require(unknown.status == PlanningStatus::Undetermined && unknown.rounds.size() == 1
        && unknown.rounds[0].added_conflict.empty(), "原问题UNKNOWN加了割");
    options.total_time_limit_seconds = 0;
    Require(RunPlanningIteration(data, options).status == PlanningStatus::TimeLimit, "总预算未生效");
    options.total_time_limit_seconds = 60;
    options.timetable.time_limit_seconds = 30;
    options.core_verification_time_limit_seconds = 30;
    options.maximum_rounds = 1;
    const auto limited = RunPlanningIteration(data, options);
    Require(limited.status == PlanningStatus::RoundLimit && limited.rounds[0].core_verification
        && limited.rounds[0].added_conflict == CandidateConflict{0}, "轮数上限丢失已复核核");

    // 提高第一层端点占轨下界，令既有图自身不可行；主问题仍是原来的宽松必要条件。
    data.parameters.minimum_terminal_occupation = data.parameters.planning_horizon + 1;
    const auto background = RunPlanningIteration(data, options);
    Require(background.status == PlanningStatus::ProvenInfeasible && background.rounds.size() == 1
        && !background.rounds[0].master->solution->selected_candidate_indices.empty()
        && background.rounds[0].timetable->candidate_core
        && background.rounds[0].timetable->candidate_core->empty()
        && background.rounds[0].core_verification
        && background.rounds[0].core_verification->status == TimetableStatus::ProvenInfeasible
        && background.rounds[0].added_conflict.empty(), "空核未独立验证背景或错误加割");
    options.timetable_backend = TimetableBackend::Milp;
    Require(RunPlanningIteration(data, options).status == PlanningStatus::Error, "MILP提核配置未拒绝");

    // 副本经过真实CSV加载器和正式入口；原输入不改动。
    const auto fixture = output / "csv_assumptions";
    std::filesystem::create_directory(fixture);
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".csv") std::filesystem::copy_file(entry.path(), fixture / entry.path().filename());
    const auto write = [&](const std::string& mode, const std::string& backend, const std::string& budget) {
        std::ifstream source(directory / "parameters.csv");
        std::ofstream target(fixture / "parameters.csv");
        target.exceptions(std::ios::failbit | std::ios::badbit);
        std::string line;
        while (std::getline(source, line)) {
            if (line.starts_with("conflict_cut_mode,") || line.starts_with("core_verification_time_limit_seconds,")) continue;
            if (line.starts_with("timetable_solver,")) line = "timetable_solver," + backend + ",backend,后端";
            target << line << '\n';
        }
        target << "conflict_cut_mode," << mode << ",mode,冲突割模式\n"
            << "core_verification_time_limit_seconds," << budget << ",second,子集复核预算\n";
    };
    write("2", "2", "30");
    auto configured = Take(LoadProblemData(fixture));
    configured.parameters.planning_output_root = (output / "csv_run").string();
    const auto configured_run = RunPlanningIteration(configured);
    Require(configured_run.status == PlanningStatus::Feasible && configured_run.rounds[0].core_verification,
        "CSV未实际启用直接核流程");
    for (const std::string mode : {"0", "3", "-1", "2.5"}) {
        write(mode, "2", "30");
        Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "非法割模式未拒绝");
    }
    write("2", "1", "30");
    Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "CSV MILP提核未拒绝");
    write("2", "2", "-1");
    Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "负复核预算未拒绝");
    std::cout << "PASS assumptions CSV production entry, zero/UNKNOWN verification fallback, empty-background recheck, total/round limits\n";
}
} // namespace

void RunAssumptionsTests(const std::filesystem::path& root, const std::filesystem::path& output) {
    CheckGuardCases();
    std::ofstream subsets(output / "assumptions_subsets.csv");
    std::ofstream totals(output / "cut_comparison.csv");
    std::ofstream rounds(output / "cut_rounds.csv");
    for (auto* stream : {&subsets, &totals, &rounds}) stream->exceptions(std::ios::failbit | std::ios::badbit);
    subsets << "delay,mask,status,direct_core,encoding_seconds,search_seconds,extraction_seconds\n";
    totals << "case,mode,rounds,cuts,total_cut_size,mean_cut_size,objective,master_seconds,encoding_seconds,search_seconds,extraction_seconds,verification_seconds,total_seconds\n";
    rounds << "case,mode,round,selected,status,direct_core,verification,actual_cut,fallback,master_seconds,encoding_seconds,search_seconds,extraction_seconds,verification_seconds,round_seconds\n";
    auto complex = Take(LoadProblemData(root / "augmentation line plan" / "Test_data_complex"));
    const auto truth = CheckAllSubsets(complex, subsets);
    CheckRuns(complex, "complex", 91700, output, &truth, totals, rounds);
    const auto small_directory = root / "augmentation line plan" / "Test_data";
    const auto small = Take(LoadProblemData(small_directory));
    CheckRuns(small, "small", 3400, output, nullptr, totals, rounds);
    const auto original = Take(LoadProblemData(root / "augmentation line plan" / "Data"));
    CheckRuns(original, "original", 144900, output, nullptr, totals, rounds);
    CheckLimitsAndCsv(small_directory, output);
    subsets.close();
    totals.close();
    rounds.close();
}
