///////////////////////////////////////////////////////////////////////////
/// @file planning_iteration.cpp
/// @brief 主问题与第一层迭代协调；包含 MILP/SAT 分派、计时、报告及不可行反馈。
///////////////////////////////////////////////////////////////////////////
#include "planning_iteration.h"
#include "timetable_sat.h"

#include "coptcpp_pch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;

/// @brief 仅保存计时与当前报告文件，不持有优化模型或业务判定。
struct IterationProgress {
    Clock::time_point start = Clock::now(); ///< 整次迭代的起点，用于总预算。
    Clock::time_point round_start = start;  ///< 当前轮起点，异常退出时也用于计算耗时。
    std::ofstream round_report;            ///< 当前轮的文件；每轮结束后关闭。
};

double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

/// @brief 将统一结果类型解包；失败交给主函数的 catch，成功移动结果，避免复制大对象。
template <typename T>
T TakeOrThrow(InputResult<T> outcome) {
    if (const auto* error = std::get_if<InputError>(&outcome)) throw *error;
    return std::move(std::get<T>(outcome));
}

// ---------------- 第一层求解分派（由原 timetable_solver 合并） ----------------

/// @brief 统一调用所选第一层后端；本函数只负责分派，两个后端各自在返回前检查可行见证。
/// @details PlanningOptions 已将 CSV 的 1/2 转为 Milp/Sat。MILP 约束仍在
/// timetable_milp.cpp，SAT 编码仍在 timetable_sat.cpp；外层加割使用共用三态结果。
InputResult<TimetableResult> SolveTimetable(const TimetableData& data,
    TimetableBackend backend, const TimetableMilpOptions& options, ConflictCutMode cut_mode) {
    if (backend == TimetableBackend::Sat)
        return SolveTimetableSat(data, {options.time_limit_seconds, options.solver_logging,
            cut_mode == ConflictCutMode::Assumptions});
    if (backend != TimetableBackend::Milp)
        return InputError{"第一层配置错误", "SolveTimetable", "未知求解后端。"};
    return SolveTimetableMilp(data, options);
}

// ---------------- 参数检查与时间控制 ----------------

void ValidateOptions(const PlanningOptions& options) {
    for (double limit : {options.total_time_limit_seconds, options.master_time_limit_seconds,
                         options.timetable.time_limit_seconds, options.core_verification_time_limit_seconds})
        if (!std::isfinite(limit) || limit < 0)
            throw std::invalid_argument("所有时间上限必须为非负有限秒数。");
    if (options.maximum_rounds <= 0) throw std::invalid_argument("最大轮数必须为正数。");
    if (options.timetable_backend != TimetableBackend::Milp && options.timetable_backend != TimetableBackend::Sat)
        throw std::invalid_argument("第一层求解后端无效。");
    if (options.conflict_cut_mode != ConflictCutMode::FullSet && options.conflict_cut_mode != ConflictCutMode::Assumptions)
        throw std::invalid_argument("冲突割模式无效。");
    if (options.conflict_cut_mode == ConflictCutMode::Assumptions && options.timetable_backend != TimetableBackend::Sat)
        throw std::invalid_argument("assumptions直接提核必须使用SAT后端。");
}

double RemainingSeconds(const PlanningOptions& options, const IterationProgress& progress) {
    return std::max(0.0, options.total_time_limit_seconds - SecondsSince(progress.start));
}

/// @brief 单轮时限不能超过尚未用完的总预算；配置本身保持不变。
double MasterTimeLimit(const PlanningOptions& options, const IterationProgress& progress) {
    return std::min(options.master_time_limit_seconds, RemainingSeconds(options, progress));
}

TimetableMilpOptions TimetableOptions(const PlanningOptions& options, const IterationProgress& progress) {
    auto timetable_options = options.timetable; // 只调整本次调用的副本。
    timetable_options.time_limit_seconds =
        std::min(timetable_options.time_limit_seconds, RemainingSeconds(options, progress));
    return timetable_options;
}

/// @brief 返回 true 表示应停止；只有当前轮文件已打开时才修改该轮结论。
bool StopIfTimeExpired(PlanningResult& result, const PlanningOptions& options,
                       const IterationProgress& progress, const char* reason) {
    if (RemainingSeconds(options, progress) > 0) return false;
    result.status = PlanningStatus::TimeLimit;
    result.reason = reason;
    if (progress.round_report.is_open()) result.rounds.back().conclusion = reason;
    return true;
}

struct ConflictDecision {
    CandidateConflict candidates;
    bool background_infeasible = false;
};

/// @brief 原始 UNSAT 是回退依据；子集必须独立重建并证 UNSAT，才可替换完整集合。
ConflictDecision VerifyDirectCore(const ProblemData& data, PlanningRound& round,
                                 const PlanningOptions& options, const IterationProgress& progress) {
    const auto& selected = round.master->solution->selected_candidate_indices;
    ConflictDecision decision{selected};
    if (options.conflict_cut_mode == ConflictCutMode::FullSet) return decision;

    // ① 核只能包含本轮选中候选。nullopt为提核失败，空向量则必须检查既有背景。
    const auto& original = *round.timetable;
    if (!original.candidate_core) {
        round.core_fallback_reason = original.core_error.empty() ? "未取得候选核" : original.core_error;
        return decision;
    }
    const auto& core = *original.candidate_core;
    auto ordered_selected = selected;
    std::sort(ordered_selected.begin(), ordered_selected.end());
    if (!std::is_sorted(core.begin(), core.end()) || std::adjacent_find(core.begin(), core.end()) != core.end()
        || !std::includes(ordered_selected.begin(), ordered_selected.end(), core.begin(), core.end())) {
        round.core_fallback_reason = "候选核不是本轮选中集合的无重复子集";
        return decision;
    }

    // ② 复核预算包含数据重建，且不能超过全流程剩余预算；零预算时不启动。
    const auto start = Clock::now();
    const double budget = std::min(options.core_verification_time_limit_seconds, RemainingSeconds(options, progress));
    if (budget <= 0) {
        round.core_fallback_reason = "复核预算不足，未启动子集复核";
        return decision;
    }
    try {
        MasterSolution selection{};
        selection.selected_candidate_indices = core;
        auto rebuilt = TakeOrThrow(BuildTimetableData(data, selection));
        const double remaining = std::min(std::max(0.0, budget - SecondsSince(start)), RemainingSeconds(options, progress));
        // 原始时间域、全部既有列车、完整业务约束；关闭assumptions，不继承原公式或解。
        round.core_verification = TakeOrThrow(SolveTimetableSat(rebuilt,
            {remaining, options.timetable.solver_logging, false}));

        // ③ 只接受确定的不可行证明；即使核等于完整集合，也按同一规则复核一次。
        if (round.core_verification->status == TimetableStatus::ProvenInfeasible) {
            decision.candidates = core;
            decision.background_infeasible = core.empty();
        } else {
            round.core_fallback_reason = round.core_verification->status == TimetableStatus::Feasible
                ? "候选核复核可行，回退已证不可行完整集合" : "候选核复核未定，回退已证不可行完整集合";
        }
    } catch (const InputError& error) {
        round.core_fallback_reason = "复核错误：" + error.context + ": " + error.message;
    } catch (const std::exception& error) {
        round.core_fallback_reason = "复核异常：" + std::string(error.what());
    }
    round.core_verification_seconds = SecondsSince(start);
    return decision;
}

// ---------------- 报告创建与逐阶段保存 ----------------

std::filesystem::path CreateRunDirectory(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    // 原子创建独立目录；并发运行也不覆盖旧结果。
    for (int suffix = 0; ; ++suffix) {
        const auto directory = root / ("run_" + std::to_string(stamp) + "_" + std::to_string(suffix));
        if (std::filesystem::create_directory(directory)) return std::filesystem::absolute(directory);
    }
}

std::ofstream OpenReport(const std::filesystem::path& path) {
    std::ofstream report;
    report.exceptions(std::ios::failbit | std::ios::badbit); // 写入失败必须被明确报告。
    report.open(path);
    return report;
}

void WriteUtf8Path(std::ostream& output, const std::filesystem::path& path) {
    const auto utf8 = path.u8string(); // Windows 中文路径按 UTF-8 输出。
    output.write(reinterpret_cast<const char*>(utf8.data()), static_cast<std::streamsize>(utf8.size()));
}

void InitializeReports(PlanningResult& result, const PlanningOptions& options, bool master_solver_logging) {
    result.output_directory = CreateRunDirectory(options.output_root);
    auto report = OpenReport(result.output_directory / "configuration.txt");
    report << "最大轮数=" << options.maximum_rounds
           << "\n总时间上限秒=" << options.total_time_limit_seconds
           << "\n每轮主问题时限秒=" << options.master_time_limit_seconds
           << "\n主问题日志开关=" << master_solver_logging
           << "\n每轮第一层时限秒=" << options.timetable.time_limit_seconds
           << "\n第一层日志开关=" << options.timetable.solver_logging
           << "\n第一层后端=" << (options.timetable_backend == TimetableBackend::Sat ? "sat" : "milp")
           << "\n冲突割模式=" << (options.conflict_cut_mode == ConflictCutMode::Assumptions ? "assumptions_direct" : "full_set")
           << "\n候选核复核预算秒=" << options.core_verification_time_limit_seconds
           << "\n结果根目录=";
    WriteUtf8Path(report, options.output_root);
    report << '\n';
    report.close();
}

/// @brief 在运行历史中创建本轮记录并打开文件，返回该记录的引用。
PlanningRound& BeginRound(PlanningResult& result, IterationProgress& progress, int number) {
    result.rounds.emplace_back();
    auto& round = result.rounds.back(); // 下一轮追加元素前有效，不跨轮次保存此引用。
    round.number = number;
    progress.round_start = Clock::now();
    progress.round_report = OpenReport(result.output_directory / ("round_" + std::to_string(number) + ".txt"));
    progress.round_report << "轮次=" << number << "\n阶段=主问题求解\n" << std::flush;
    return round;
}

void SaveMasterReport(const ProblemData& data, const PlanningRound& round, IterationProgress& progress) {
    PrintMasterProblemReport(data, *round.master, progress.round_report);
    progress.round_report << std::flush; // 第一层开始前，主问题结果已写入文件。
}

void SaveTimetableDataReport(const PlanningRound& round, IterationProgress& progress) {
    PrintTimetableDataReport(*round.timetable_data, progress.round_report);
    progress.round_report << "阶段=第一层验证\n" << std::flush;
}

void SaveTimetableResultReport(const PlanningRound& round, IterationProgress& progress) {
    PrintTimetableReport(*round.timetable_data, *round.timetable, progress.round_report);
    progress.round_report << std::flush; // 在加割前保存第一层的明确判定。
}

void SaveConflictReport(const PlanningRound& round, IterationProgress& progress) {
    const auto& conflict = round.added_conflict; // 调用时已经成功加入原模型，且非空。
    progress.round_report << "新增冲突割：sum(x_a, a in {";
    for (int index : conflict) progress.round_report << ' ' << index;
    progress.round_report << " }) <= " << conflict.size() - 1 << '\n';
}

/// @brief 统一保存当前轮的耗时与结论；所有正常退出分支均先调用此函数。
void FinishRound(PlanningResult& result, IterationProgress& progress) {
    auto& round = result.rounds.back();
    round.elapsed_seconds = SecondsSince(progress.round_start);
    progress.round_report << "本轮耗时=" << round.elapsed_seconds << " 秒\n结论=" << round.conclusion << '\n';
    progress.round_report << "主问题秒=" << round.master_seconds << " 复核秒=" << round.core_verification_seconds
        << "\n复核结果=";
    if (round.core_verification) {
        const auto& verified = *round.core_verification;
        progress.round_report << (verified.status == TimetableStatus::ProvenInfeasible ? "UNSAT"
            : verified.status == TimetableStatus::Feasible ? "SAT" : "UNKNOWN")
            << " 原始状态码=" << verified.solver_status;
    } else progress.round_report << "未执行";
    progress.round_report << "\n回退原因=" << round.core_fallback_reason << '\n';
    progress.round_report.close();
    result.reason = round.conclusion;
}

// ---------------- 主问题状态检查与异常收尾 ----------------

/// @brief true=有可用主问题解，可以继续第一层；false=正常终止；异常状态抛出错误。
bool CanValidateTimetable(PlanningResult& result, PlanningRound& round) {
    const auto& master = *round.master;
    if (master.solver_status == COPT_STATUS_NUMERICAL || master.solver_status == COPT_STATUS_IMPRECISE
        || master.solver_status == COPT_STATUS_UNBOUNDED)
        throw InputError{"主问题状态错误", "RunPlanningIteration", "数值异常、精度不足或意外无界。"};
    if (master.solver_status == COPT_STATUS_INFEASIBLE) {
        if (master.solution) throw std::runtime_error("主问题不可行状态与已有解矛盾。");
        result.status = PlanningStatus::ProvenInfeasible;
        round.conclusion = "主问题在累计有效冲突割下已证不可行。";
        return false;
    }
    if (!master.solution) {
        result.status = PlanningStatus::Undetermined;
        round.conclusion = "主问题未取得可用解，不能启动第一层或生成割。";
        return false;
    }
    return true; // 有可用解即可验证；是否已证最优在最终结论中区分。
}

void RecordFailure(PlanningResult& result, IterationProgress& progress, InputError error) {
    result.error = std::move(error);
    result.status = PlanningStatus::Error;
    result.reason = result.error->context + ": " + result.error->message;
    if (!result.rounds.empty()) {
        result.rounds.back().conclusion = result.reason;
        result.rounds.back().elapsed_seconds = SecondsSince(progress.round_start);
    }
    try {
        if (progress.round_report.is_open())
            progress.round_report << "错误=" << result.reason << '\n' << std::flush;
    } catch (const std::exception&) { /* 磁盘仍不可写时保留原错误，不以二次写入错误覆盖它。 */ }
}

void SaveSummary(PlanningResult& result) {
    if (result.output_directory.empty()) return;
    try {
        auto summary = OpenReport(result.output_directory / "summary.txt");
        PrintPlanningReport(result, summary);
        summary.close();
    } catch (const std::exception& error) {
        result.status = PlanningStatus::Error;
        result.error = InputError{"报告写入错误", "summary.txt", error.what()};
        result.reason += "；汇总报告写入失败：" + std::string(error.what());
    }
}

const char* StatusName(PlanningStatus status) {
    switch (status) {
        case PlanningStatus::Feasible: return "可行";
        case PlanningStatus::ProvenInfeasible: return "已证不可行";
        case PlanningStatus::Undetermined: return "未定";
        case PlanningStatus::RoundLimit: return "达到轮数上限";
        case PlanningStatus::TimeLimit: return "达到总时间上限";
        case PlanningStatus::Error: return "错误";
    }
    return "未知";
}
} // namespace

PlanningOptions::PlanningOptions(const InputParameters& parameters)
    : maximum_rounds(parameters.maximum_rounds),
      total_time_limit_seconds(parameters.total_time_limit_seconds),
      master_time_limit_seconds(parameters.master_time_limit_seconds),
      timetable{parameters.timetable_time_limit_seconds, parameters.timetable_solver_logging},
      output_root(std::u8string(parameters.planning_output_root.begin(), parameters.planning_output_root.end())) {
    if (parameters.timetable_solver == 2) timetable_backend = TimetableBackend::Sat;
    else if (parameters.timetable_solver != 1) throw std::invalid_argument("第一层求解器必须为 1（MILP）或 2（SAT）。");
    conflict_cut_mode = static_cast<ConflictCutMode>(parameters.conflict_cut_mode);
    core_verification_time_limit_seconds = parameters.core_verification_time_limit_seconds;
}

PlanningResult RunPlanningIteration(const ProblemData& data) {
    return RunPlanningIteration(data, PlanningOptions(data.parameters));
}

/// @brief 主流程按“初始化 → 求主问题 → 构建数据 → 第一层判定 → 加割或结束”排列。
PlanningResult RunPlanningIteration(const ProblemData& data, const PlanningOptions& options) {
    PlanningResult result;       // 整次运行的业务结果和全部轮次历史。
    IterationProgress progress;  // 辅助状态：计时起点及当前报告文件，不保存优化模型。
    try {
        // ① 检查配置、建立报告目录；主问题在循环外创建，只建模一次。
        ValidateOptions(options);
        InitializeReports(result, options, data.parameters.master_solver_logging);
        if (!StopIfTimeExpired(result, options, progress, "建模前已达到总时间上限。")) {
            //创建主问题模型
            auto session = TakeOrThrow(MasterProblemSession::Create(data));
            //进入循环迭代
            for (int round_index = 0; round_index < options.maximum_rounds; ++round_index) {
                if (StopIfTimeExpired(result, options, progress, "下一轮开始前已达到总时间上限。")) break;
                auto& round = BeginRound(result, progress, round_index + 1);

                // ② 求解原主问题（包含此前所有割）；没有可用解或预算耗尽则记录并结束。
                const auto master_start = Clock::now();
                round.master = TakeOrThrow(session->Solve(MasterTimeLimit(options, progress)));
                round.master_seconds = SecondsSince(master_start);
                SaveMasterReport(data, round, progress);
                if (!CanValidateTimetable(result, round)
                    || StopIfTimeExpired(result, options, progress, "主问题结束后已达到总时间上限，未启动第一层。")) {
                    FinishRound(result, progress);
                    break;
                }

                // ③ 根据本轮选中候选构建具体车次、事件、活动和冲突关系。
                round.timetable_data = TakeOrThrow(BuildTimetableData(data, *round.master->solution));
                SaveTimetableDataReport(round, progress);
                if (StopIfTimeExpired(result, options, progress, "时刻表构建后达到总时间上限，未启动第一层。")) {
                    FinishRound(result, progress);
                    break;
                }

                // ④ 用指定后端验证同一份数据；只依据三态判定决定是否加割。
                round.timetable = TakeOrThrow(SolveTimetable(*round.timetable_data,
                    options.timetable_backend, TimetableOptions(options, progress), options.conflict_cut_mode));
                SaveTimetableResultReport(round, progress);
                bool continue_iteration = false; // 默认本轮后结束，只有成功加割才继续。
                switch (round.timetable->status) {
                    case TimetableStatus::Feasible: // 情况 A：已有可行时刻表，保存方案后结束。
                        result.status = PlanningStatus::Feasible;
                        round.conclusion = round.master->solver_status == COPT_STATUS_OPTIMAL
                            ? "主问题达到求解器最优容差，且第一层可行；尚未验证具体动车组交路。"
                            : "取得第一层可行方案，主问题尚未证明最优。";
                        break;
                    case TimetableStatus::Undetermined: // 情况 B：超时等原因导致未定，不加割。
                        result.status = PlanningStatus::Undetermined;
                        round.conclusion = "第一层未定，不生成割。";
                        break;
                    case TimetableStatus::ProvenInfeasible: {
                        const auto& selected = round.master->solution->selected_candidate_indices;
                        if (selected.empty()) { // 情况 C1：没有新增列车仍不可行，报告基础图不可行。
                            result.status = PlanningStatus::ProvenInfeasible;
                            round.conclusion = "无新增候选时既有运行图仍已证不可行，不生成空集合割。";
                        } else { // 情况 C2：直接核复核成功后使用子集，否则保留已证明的完整集合。
                            const auto decision = VerifyDirectCore(data, round, options, progress);
                            if (decision.background_infeasible) {
                                result.status = PlanningStatus::ProvenInfeasible;
                                round.conclusion = "空核复核证明既有背景不可行，不生成空集合割。";
                                break;
                            }
                            if (!TakeOrThrow(session->AddConflictCut(decision.candidates)))
                                throw std::runtime_error("主问题返回了已被冲突割排除的集合。");
                            round.added_conflict = decision.candidates;
                            SaveConflictReport(round, progress);
                            round.conclusion = options.conflict_cut_mode == ConflictCutMode::Assumptions
                                && round.core_fallback_reason.empty()
                                ? "第一层已证不可行，直接核独立复核UNSAT后已加割。"
                                : "第一层已证不可行，已在原主问题追加完整候选集合割。";
                            continue_iteration = true;
                        }
                        break;
                    }
                }

                // ⑤ 所有正常分支先保存本轮；只有 C2 回到下一轮求解加过割的原模型。
				FinishRound(result, progress);//保存本轮耗时、结论、复核结果和回退原因
                if (!continue_iteration) break;
                if (round.number == options.maximum_rounds) {
                    result.status = PlanningStatus::RoundLimit;
                    result.reason = "达到最大轮数；最后一轮冲突割已保存，尚未完成全部方案验证。";
                }
            }
        }
    } catch (const InputError& error) {
        RecordFailure(result, progress, error); // 异常只记录并结束，不生成割。
    } catch (const std::exception& error) {
        RecordFailure(result, progress, {"迭代错误", "RunPlanningIteration", error.what()});
    }
    result.elapsed_seconds = SecondsSince(progress.start);
    SaveSummary(result); // 无论可行、不可行、未定、达到限制或异常，统一汇总已有结果。
    return result;
}

void PrintPlanningReport(const PlanningResult& result, std::ostream& output) {
    output << "迭代状态=" << StatusName(result.status) << "\n原因=" << result.reason
           << "\n已执行轮数=" << result.rounds.size() << "\n总耗时秒=" << result.elapsed_seconds << '\n';
    for (const auto& round : result.rounds) {
        output << "第 " << round.number << " 轮";
        if (round.master && round.master->solution)
            output << " 目标值=" << round.master->solution->objective_value;
        output << " 新增割数=" << (round.added_conflict.empty() ? 0 : 1)
               << " 结论=" << round.conclusion << '\n';
    }
    output << "结果目录=";
    WriteUtf8Path(output, result.output_directory);
    output << '\n';
}

/// @brief 原 timetable_solver 的报告分派；与内部 SolveTimetable 一起由本模块管理。
/// @details 此函数公开给 main 使用，按结果中的后端解释原始状态码及输出格式。
void PrintTimetableReport(const TimetableData& data, const TimetableResult& result, std::ostream& output) {
    if (result.backend == TimetableBackend::Sat) PrintTimetableSatReport(data, result, output);
    else PrintTimetableMilpReport(data, result, output);
}

