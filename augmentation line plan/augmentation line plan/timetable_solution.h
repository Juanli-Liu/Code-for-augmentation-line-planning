///////////////////////////////////////////////////////////////////////////
/// @file timetable_solution.h
/// @brief MILP/SAT 共用的业务状态、时刻表见证及独立检查接口。
///////////////////////////////////////////////////////////////////////////
#ifndef TIMETABLE_SOLUTION_H
#define TIMETABLE_SOLUTION_H

#include "timetable_data.h"
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

/// @brief 只有已证不可行可以用于生成候选集合割。
enum class TimetableStatus { Feasible, ProvenInfeasible, Undetermined };
/// @brief 第一层求解后端；原始状态码必须结合此字段解释。
enum class TimetableBackend { Milp, Sat };

/// @brief true 表示冲突记录中的 first_visit 先于 second_visit。
struct TimetableOrder {
    bool first_arrives_first; ///< [内部计算] 到达侧次序。
    bool first_departs_first; ///< [内部计算] 出发侧次序。
};

/// @brief 下标与 TimetableData 一一对应，不保存求解器对象。
struct TimetableSolution {
    std::vector<int> event_times;     ///< [内部计算] events 下标 -> 整数时刻，单位 time_step。
    std::vector<int> visit_platforms; ///< [内部编号] visits 下标 -> 原轨道编号。
    std::vector<TimetableOrder> orders; ///< [内部计算] station_conflicts 下标 -> 到发次序。
};

/// @brief 可审计的候选激活映射；跨候选子句会计入每个参与候选。
struct CandidateAssumption {
    int candidate_index;
    int literal;
    std::vector<int> train_indices;
    std::size_t guarded_clause_count = 0;
};

/// @brief 共用求解结果；前四个字段保留原 MILP 初始化顺序和含义。
struct TimetableResult {
    TimetableStatus status;
    int solver_status;                       ///< [内部计算] COPT 状态或 CaDiCaL 的 0/10/20。
    double solving_seconds;                  ///< [内部计算] 纯求解阶段秒数。
    std::optional<TimetableSolution> solution; ///< [内部计算] 仅可行时填写。
    TimetableBackend backend = TimetableBackend::Milp;
    double encoding_seconds = 0;             ///< [内部计算] SAT 编码阶段秒数。
    std::size_t variable_count = 0;           ///< [内部计算] SAT 已分配变量数。
    std::size_t clause_count = 0;             ///< [内部计算] SAT 实际提交子句数。
    std::string solver_version;              ///< [内部计算] SAT 求解器版本。
    std::vector<CandidateAssumption> candidate_assumptions;
    std::optional<std::vector<int>> candidate_core; ///< UNSAT 后的直接核；空向量表示背景核，nullopt 表示未提取。
    double core_extraction_seconds = 0;
    std::string core_error;                  ///< 提核失败不抹去原始 UNSAT 判定。
};

/// @brief 按整数时刻直接检查原约束；不调用求解器、不复用 SAT 编码。
/// @return 检查通过返回空值，否则返回第一条违反约束的诊断。
std::optional<InputError> ValidateTimetableSolution(
    const TimetableData& data, const TimetableSolution& solution);

#endif  // TIMETABLE_SOLUTION_H
