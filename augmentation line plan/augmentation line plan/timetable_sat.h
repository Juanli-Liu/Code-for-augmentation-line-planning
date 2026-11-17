///////////////////////////////////////////////////////////////////////////
/// @file timetable_sat.h
/// @brief 使用 CaDiCaL 时间顺序编码验证第一层，可选候选级 assumptions 直接提核。
///////////////////////////////////////////////////////////////////////////
#ifndef TIMETABLE_SAT_H
#define TIMETABLE_SAT_H

#include "timetable_solution.h"
#include <iosfwd>

/// @brief SAT 单次调用预算；包含变量分配及编码，求解期间通过回调检查。
//作用：指定时间上限和日志开关
struct TimetableSatOptions {
    double time_limit_seconds; ///< [求解设置] 非负有限墙钟秒数。
    bool solver_logging;       ///< [求解设置] 是否输出 CaDiCaL 日志。
    bool candidate_assumptions = false; ///< 全部选中候选均 assume 为真；不做逐候选缩减。
};

using TimetableSatResult = TimetableResult;

/// @brief 对同一份 TimetableData 编码并求解；SAT 见证必须通过独立检查。
/// @return SAT=可行、UNSAT=已证不可行、超时=未定；输入或编码异常返回 InputError。
InputResult<TimetableSatResult> SolveTimetableSat(
    const TimetableData& timetable, const TimetableSatOptions& options);

/// @brief 输出 SAT 状态、编码规模和逐车逐站的整数时刻及轨道。
void PrintTimetableSatReport(const TimetableData& timetable,
    const TimetableSatResult& result, std::ostream& output);

#endif  // TIMETABLE_SAT_H
