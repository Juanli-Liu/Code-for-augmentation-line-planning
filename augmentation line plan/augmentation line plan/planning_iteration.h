///////////////////////////////////////////////////////////////////////////
/// @file planning_iteration.h
/// @brief 持久主问题与第一层 MILP/SAT 的迭代协调、逐轮结果和落盘报告。
///////////////////////////////////////////////////////////////////////////
#ifndef PLANNING_ITERATION_H
#define PLANNING_ITERATION_H

#include "master_problem.h"
#include "timetable_milp.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

enum class PlanningStatus {
    Feasible, ProvenInfeasible, Undetermined, RoundLimit, TimeLimit, Error
};

enum class ConflictCutMode { FullSet = 1, Assumptions = 2 };

struct PlanningOptions {
    /// @brief 从已校验的 parameters.csv 初始化；求解器由必填的 1/2 参数决定。
    explicit PlanningOptions(const InputParameters& parameters);

    int maximum_rounds;                      ///< 最多求解主问题的次数。
    double total_time_limit_seconds;         ///< 全流程墙钟预算，包含建模与报告时间。
    double master_time_limit_seconds;        ///< 单轮主问题上限，还会被剩余总预算截断。
    TimetableMilpOptions timetable;           ///< 单轮第一层上限，同样受剩余预算限制。
    TimetableBackend timetable_backend = TimetableBackend::Milp; ///< 构造函数按输入的 1/2 设置第一层后端。
    std::filesystem::path output_root;       ///< 每次运行在其下创建独立目录；相对路径基于工作目录。
    ConflictCutMode conflict_cut_mode = ConflictCutMode::FullSet;
    double core_verification_time_limit_seconds = 60;
};

struct PlanningRound {
    int number = 0;
    std::optional<MasterProblemResult> master;
    std::optional<TimetableData> timetable_data; ///< 保留见证所引用的事件、访问和轨道编号。
    std::optional<TimetableResult> timetable; ///< 结果带后端标记，不混用原始状态码。
    CandidateConflict added_conflict;           ///< 仅成功追加约束后填写。
    double elapsed_seconds = 0;                ///< 本轮墙钟耗时。
    std::string conclusion;                    ///< 本轮继续/结束原因。
    std::optional<TimetableResult> core_verification; ///< 原始子集重新建模，关闭 assumptions 求解。
    double core_verification_seconds = 0;      ///< 含子集数据构建和求解。
    double master_seconds = 0;                 ///< 每轮主问题调用墙钟时间。
    std::string core_fallback_reason;
};

/// @brief 即使中途报错也保留已经完成的轮次，不用错误值替换整个运行历史。
struct PlanningResult {
    PlanningStatus status = PlanningStatus::Error;
    std::vector<PlanningRound> rounds;
    std::filesystem::path output_directory;
    std::optional<InputError> error;
    std::string reason;
    double elapsed_seconds = 0;                ///< 全流程墙钟时间，含建模和逐轮报告。
};

/// @brief 主问题只建一次；UNSAT后追加完整集合割，或独立复核通过的直接核割。
/// @details COPT 时限约束求解阶段；建模/文件写入不能抢占，阶段间检查墙钟预算。
/// 主流程显式列出第一层 A=可行、B=未定、C1=空集合不可行、C2=非空集合不可行；
/// 参数检查、计时、逐阶段报告和异常收尾由 .cpp 内部辅助函数负责。
PlanningResult RunPlanningIteration(const ProblemData& data, const PlanningOptions& options);

/// @brief 正式入口：读取 data.parameters 中的数据表配置，再运行完整迭代。
PlanningResult RunPlanningIteration(const ProblemData& data);

/// @brief 输出终止原因、轮次数和结果目录；详细记录在每轮报告中。
void PrintPlanningReport(const PlanningResult& result, std::ostream& output);

/// @brief 按本轮结果的后端标记输出 MILP/SAT 报告，供迭代记录及 main 控制台共用。
/// @details 原 timetable_solver 的报告分派已合并至 planning_iteration.cpp。
void PrintTimetableReport(const TimetableData& data, const TimetableResult& result, std::ostream& output);

#endif  // PLANNING_ITERATION_H
