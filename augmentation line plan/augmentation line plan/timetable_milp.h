///////////////////////////////////////////////////////////////////////////
/// @file timetable_milp.h
/// @brief 第一层非周期时刻表 MILP 的求解、结果及报告接口。
///////////////////////////////////////////////////////////////////////////
#ifndef TIMETABLE_MILP_H
#define TIMETABLE_MILP_H

#include "timetable_solution.h"

#include <iosfwd>
#include <optional>
#include <vector>


/// @brief 求解控制参数，时间上限仅用于第一层 COPT 求解，不包含主问题。
struct TimetableMilpOptions {
    double time_limit_seconds; ///< [求解设置] 从数据表配置传入，非负有限秒数。
    bool solver_logging;       ///< [求解设置] 从数据表配置传入，是否输出 COPT 求解日志。
};

/// @brief 保留原 MILP 接口名称，结果结构由两种后端共用。
using TimetableMilpResult = TimetableResult;

/// @brief 对 BuildTimetableData 的输出建立零目标整数时刻表模型。
/// @details 可行解在本函数内提取并通过 ValidateTimetableSolution 独立检查后返回。
/// @param timetable 已构建的事件、活动、轨道和次序关系。
/// @param options 第一层时限与日志开关。
/// @return 可行、已证不可行或未定；调用/取值异常或见证检查失败返回 InputError。
InputResult<TimetableMilpResult> SolveTimetableMilp(
    const TimetableData& timetable, const TimetableMilpOptions& options);

/// @brief 输出第一层状态及逐车逐站时刻、轨道；编号沿用 TimetableData。
/// @param timetable 与结果对应的建模输入，用于解释事件含义及换算分钟。
/// @param result 求解结果。
/// @param output 报告输出流。
void PrintTimetableMilpReport(const TimetableData& timetable,
                              const TimetableMilpResult& result, std::ostream& output);

#endif  // TIMETABLE_MILP_H
