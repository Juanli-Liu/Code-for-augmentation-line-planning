///////////////////////////////////////////////////////////////////////////
/// @file timetable_milp.cpp
/// @brief 按论文第一层 MIP 建立整数时刻、列车次序和到发线分配模型。
/// @details 输入来自第二阶段；不继承客流、聚合轨道分配或动车组接续变量。
///////////////////////////////////////////////////////////////////////////
#include "timetable_milp.h"

#include "coptcpp_pch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr double kValueTolerance = 1e-6; // COPT 整数容差、约束容差及取值检查使用同一精度。

/// @brief 保存求解器句柄；每个 vector 均与 TimetableData 的对应集合对齐。
struct TimetableVariables {
    std::vector<Var> times;                   // tau_e：每个事件的整数时刻。
    std::vector<Var> arrival_orders;          // lambda_arr：下标 c 对应 station_conflicts[c] 的到达侧次序。
    std::vector<Var> departure_orders;        // lambda_dep：同一下标 c 的出发侧次序，终到时比较释放轨道事件。
    std::vector<std::map<int, Var>> tracks;   // z_v,g：访问下标 -> 原轨道编号 -> 二元变量。
};

/// @brief 使 tau_after - tau_before >= gap 在未激活时完全放松的最小非负 M。
/// @details 最坏时差为 L_after-U_before，故 M=max(0,U_before+gap-L_after)。
/// 先转 double 再运算，避免整数上界与间隔相加溢出。正反方向分别调用。
double DifferenceBigM(const TimetableData& data, std::size_t before_event,
                      std::size_t after_event, int gap) {
    if (gap < 0) throw std::invalid_argument("安全间隔不得为负。");
    const double before_upper = data.events.at(before_event).time_domain.upper;
    const double after_lower = data.events.at(after_event).time_domain.lower;
    return std::max(0.0, before_upper + gap - after_lower);
}

/// @brief 建立全部变量及每次车站访问的轨道唯一分配约束，设置零目标。
TimetableVariables AddVariablesAndTrackAssignmentConstraints(Model& model, const TimetableData& data) {
    TimetableVariables variables;
    // e 为事件下标；时间变量上下界直接承接数据层，不把端点占轨基准固定下来。
    for (std::size_t e = 0; e < data.events.size(); ++e) {
        const auto& domain = data.events[e].time_domain; // 包含既有实际事件的受限后移窗口。
        if (domain.lower < 0 || domain.upper < domain.lower || domain.upper > data.planning_horizon)
            throw std::invalid_argument("事件 " + std::to_string(e) + " 的时间域无效。");
        variables.times.push_back(model.AddVar(domain.lower, domain.upper, 0.0, COPT_INTEGER,
            ("tau_" + std::to_string(e)).c_str()));
    }
    // c 为同站同向冲突记录下标，两个次序变量始终引用同一对 first/second 访问。
    // 始发/终到也保留两个变量：对应侧分别表示开始占轨/释放轨道的次序。
    // 数据层将含始发的到达间隔、含终到的出发间隔置零，避免施加实际到发间隔。
    for (std::size_t c = 0; c < data.station_conflicts.size(); ++c) {
        const auto suffix = std::to_string(c); // 同一冲突的到达和出发变量共享编号。
        variables.arrival_orders.push_back(model.AddVar(0.0, 1.0, 0.0, COPT_BINARY, ("arr_order_" + suffix).c_str()));
        variables.departure_orders.push_back(model.AddVar(0.0, 1.0, 0.0, COPT_BINARY, ("dep_order_" + suffix).c_str()));
    }
    variables.tracks.resize(data.visits.size());
    // v 为列车的一次车站访问下标，不是车站编号；同站不同列车拥有不同的轨道变量。
    for (std::size_t v = 0; v < data.visits.size(); ++v) {
        const auto& visit = data.visits[v];
        // choices 是 tracks[v] 的引用（map<int, Var>），不是新的优化变量或容器副本。
        // 键是原轨道编号，值是对应 z_v,g 的 COPT 句柄；emplace 直接写入 tracks[v]。
        auto& choices = variables.tracks[v];
        for (int platform : visit.allowed_platform_indices) {
            // 显式固定既有轨道；当前数据构建器已把既有可用集合缩为单元素。
            // BINARY 限制取值为 0/1；上下界进一步决定可自由选择 [0,1]，
            // 还是必须选中 [1,1] / 禁止选中 [0,0]，不会改变变量的二元类型。
            const double lower = visit.fixed_platform_index == platform ? 1.0 : 0.0;
            const double upper = visit.fixed_platform_index && *visit.fixed_platform_index != platform ? 0.0 : 1.0;
            const auto name = "track_" + std::to_string(v) + "_" + std::to_string(platform);
            if (choices.contains(platform)) throw std::invalid_argument("允许轨道编号重复。");
            choices.emplace(platform, model.AddVar(lower, upper, 0.0, COPT_BINARY, name.c_str()));
        }
        if (visit.fixed_platform_index && !choices.contains(*visit.fixed_platform_index))
            throw std::invalid_argument("固定轨道不在允许轨道集合中。");
        Expr assigned; // 当前访问的 sum_g z_v,g；下方将其约束为 1。
        for (const auto& [platform, selected] : choices) assigned += selected;
        // eq:ttp-track-assignment。空集合对应 0=1，由模型给出不可行判定。
        model.AddConstr(assigned == 1.0, ("track_assignment_" + std::to_string(v)).c_str());
    }
    model.SetObjSense(COPT_MINIMIZE); // 所有变量目标系数均为 0：只判定可行性。
    return variables;
}

/// @brief 区间运行/通过使用等式，中间停车/始终点占轨使用时长下界。
void AddActivityConstraints(Model& model, const TimetableData& data, const TimetableVariables& variables) {
    // a 为活动下标；difference 始终表示后事件减前事件，单位为 time_step。
    for (std::size_t a = 0; a < data.activities.size(); ++a) {
        const auto& activity = data.activities[a];
        if (activity.duration < 0) throw std::invalid_argument("活动时长不得为负。");
        const auto difference = variables.times.at(activity.to_event_index)
                              - variables.times.at(activity.from_event_index);
        const auto name = "activity_" + std::to_string(a);
        // eq:ttp-trip-constraint、eq:ttp-pass-through-constraint。
        if (activity.has_fixed_duration()) model.AddConstr(difference == activity.duration, name.c_str());
        // eq:ttp-dwell-constraint、eq:ttp-origin-occupation、eq:ttp-destination-occupation。
        else model.AddConstr(difference >= activity.duration, name.c_str());
    }
}

/// @brief 用一个次序变量表达两种先后关系；order=1 表示 first 先于 second。
void AddOrderedHeadway(Model& model, const TimetableData& data, const TimetableVariables& variables,
                       std::size_t first_event, std::size_t second_event, int gap,
                       const Var& order, const std::string& name) {
    const auto& first_time = variables.times.at(first_event);
    const auto& second_time = variables.times.at(second_event);
    const double forward_m = DifferenceBigM(data, first_event, second_event, gap);
    const double reverse_m = DifferenceBigM(data, second_event, first_event, gap);
    model.AddConstr(second_time - first_time + forward_m * (1.0 - order) >= gap, (name + "_forward").c_str());
    model.AddConstr(first_time - second_time + reverse_m * order >= gap, (name + "_reverse").c_str());
}

/// @brief 按已构建的同站同向冲突集合添加到达、出发间隔，端点有效间隔可为零。
void AddHeadwayConstraints(Model& model, const TimetableData& data, const TimetableVariables& variables) {
    for (std::size_t c = 0; c < data.station_conflicts.size(); ++c) {
        const auto& conflict = data.station_conflicts[c];
        const auto& first = data.visits.at(conflict.first_visit_index);
        const auto& second = data.visits.at(conflict.second_visit_index);
        // eq:ttp-arrival-headway-forward/reverse。零间隔仍定义到达侧的先后次序。
        AddOrderedHeadway(model, data, variables, first.arrival_side_event_index, second.arrival_side_event_index,
            conflict.arrival_headway, variables.arrival_orders[c], "arrival_" + std::to_string(c));
        // eq:ttp-departure-headway-forward/reverse。
        AddOrderedHeadway(model, data, variables, first.departure_side_event_index, second.departure_side_event_index,
            conflict.departure_headway, variables.departure_orders[c], "departure_" + std::to_string(c));
    }
}

/// @brief eq:ttp-section-order-consistency：同一有向区间入口与出口的两车次序相同。
void AddSectionOrderConstraints(Model& model, const TimetableData& data, const TimetableVariables& variables) {
    for (std::size_t k = 0; k < data.section_orders.size(); ++k) {
        const auto& order = data.section_orders[k]; // 两端冲突已经由数据层统一 first/second 列车方向。
        model.AddConstr(variables.departure_orders.at(order.entry_departure_conflict_index)
            == variables.arrival_orders.at(order.exit_arrival_conflict_index), ("section_order_" + std::to_string(k)).c_str());
    }
}

/// @brief 两车选择同一轨道时，按到达次序要求前车释放后再经过 track_headway。
void AddTrackCompatibilityConstraints(Model& model, const TimetableData& data, const TimetableVariables& variables) {
    for (std::size_t c = 0; c < data.station_conflicts.size(); ++c) {
        const auto& conflict = data.station_conflicts[c];
        const auto& first = data.visits.at(conflict.first_visit_index);
        const auto& second = data.visits.at(conflict.second_visit_index);
        const auto& order = variables.arrival_orders[c]; // 复用 lambda_arr，不另建占轨次序变量。
        // M 的前事件必须是释放侧，后事件必须是开始占轨侧。
        const double forward_m = DifferenceBigM(data, first.departure_side_event_index,
                                                second.arrival_side_event_index, data.track_headway);
        const double reverse_m = DifferenceBigM(data, second.departure_side_event_index,
                                                first.arrival_side_event_index, data.track_headway);
        for (int platform : conflict.common_platform_indices) {
            const auto& first_track = variables.tracks.at(conflict.first_visit_index).at(platform);
            const auto& second_track = variables.tracks.at(conflict.second_visit_index).at(platform);
            const auto suffix = std::to_string(c) + "_" + std::to_string(platform);
            // eq:ttp-track-compatibility-forward：z_i=z_j=lambda_arr=1 时松弛项为零。
            model.AddConstr(variables.times.at(second.arrival_side_event_index)
                - variables.times.at(first.departure_side_event_index)
                + forward_m * (3.0 - first_track - second_track - order) >= data.track_headway,
                ("track_forward_" + suffix).c_str());
            // eq:ttp-track-compatibility-reverse：z_i=z_j=1、lambda_arr=0 时松弛项为零。
            model.AddConstr(variables.times.at(first.arrival_side_event_index)
                - variables.times.at(second.departure_side_event_index)
                + reverse_m * (2.0 - first_track - second_track + order) >= data.track_headway,
                ("track_reverse_" + suffix).c_str());
        }
    }
}

/// @brief 检查有限性、整数误差及范围后再转换，不能无条件四舍五入求解器结果。
int ReadInteger(Var& variable, int lower, int upper, const std::string& context) {
    const double value = variable.Get(COPT_DBLINFO_VALUE);
    const double rounded = std::round(value);
    if (!std::isfinite(value) || std::abs(value - rounded) > kValueTolerance || rounded < lower || rounded > upper)
        throw std::runtime_error(context + " 的求解器取值不满足整数范围。");
    return static_cast<int>(rounded);
}

/// @brief 将求解器句柄转成普通整数结果；这里只检查取值，不另行重复求解模型。
TimetableSolution ExtractSolution(const TimetableData& data, TimetableVariables& variables) {
    TimetableSolution solution;
    for (std::size_t e = 0; e < data.events.size(); ++e) {
        const auto& domain = data.events[e].time_domain;
        solution.event_times.push_back(ReadInteger(variables.times[e], domain.lower, domain.upper, "事件 " + std::to_string(e)));
    }
    for (std::size_t v = 0; v < data.visits.size(); ++v) {
        std::optional<int> chosen; // 当前访问最终选择的原轨道编号。
        for (auto& [platform, variable] : variables.tracks[v]) {
            if (ReadInteger(variable, 0, 1, "访问 " + std::to_string(v) + " 的轨道选择") == 1) {
                if (chosen) throw std::runtime_error("一次访问选中了多条轨道。");
                chosen = platform;
            }
        }
        if (!chosen) throw std::runtime_error("一次访问未选中轨道。");
        if (data.visits[v].fixed_platform_index && chosen != data.visits[v].fixed_platform_index)
            throw std::runtime_error("既有列车固定轨道发生变化。");
        solution.visit_platforms.push_back(*chosen);
    }
    for (std::size_t c = 0; c < data.station_conflicts.size(); ++c) {
        solution.orders.push_back({ReadInteger(variables.arrival_orders[c], 0, 1, "到达次序") == 1,
                                  ReadInteger(variables.departure_orders[c], 0, 1, "出发次序") == 1});
    }
    return solution;
}

/// @brief 区分证明、已有解和未定；异常状态不得伪装成不可行证明。
TimetableStatus ClassifyStatus(int solver_status, bool has_solution) {
    if (solver_status == COPT_STATUS_NUMERICAL || solver_status == COPT_STATUS_IMPRECISE
        || solver_status == COPT_STATUS_UNBOUNDED)
        throw std::runtime_error("时刻表求解异常，COPT 状态码=" + std::to_string(solver_status));
    if (solver_status == COPT_STATUS_INFEASIBLE) {
        if (has_solution) throw std::runtime_error("求解器不可行状态与已有解标志矛盾。");
        return TimetableStatus::ProvenInfeasible;
    }
    if (has_solution) return TimetableStatus::Feasible; // 包括超时或中断时已取得整数可行解。
    if (solver_status == COPT_STATUS_OPTIMAL) throw std::runtime_error("求解器返回最优但没有可读取的解。");
    return TimetableStatus::Undetermined; // 含 TIMEOUT、INTERRUPTED、INF_OR_UNB，均不得生成割。
}

}  // namespace

InputResult<TimetableMilpResult> SolveTimetableMilp(const TimetableData& timetable, const TimetableMilpOptions& options) {
    try {
        if (!std::isfinite(options.time_limit_seconds) || options.time_limit_seconds < 0.0)
            throw std::invalid_argument("第一层时限必须为非负有限秒数。");
        Envr environment;
        Model model = environment.CreateModel("timetable_feasibility");
        model.SetDblParam(COPT_DBLPARAM_TIMELIMIT, options.time_limit_seconds);
        model.SetDblParam(COPT_DBLPARAM_INTTOL, kValueTolerance);
        model.SetDblParam(COPT_DBLPARAM_FEASTOL, kValueTolerance);
        model.SetIntParam(COPT_INTPARAM_LOGGING, options.solver_logging ? 1 : 0);
        auto variables = AddVariablesAndTrackAssignmentConstraints(model, timetable);
        AddActivityConstraints(model, timetable, variables);
        AddHeadwayConstraints(model, timetable, variables);
        AddSectionOrderConstraints(model, timetable, variables);
        AddTrackCompatibilityConstraints(model, timetable, variables);
        model.Solve();

        const int raw_status = model.GetIntAttr(COPT_INTATTR_STATUS);
        const bool has_solution = model.GetIntAttr(COPT_INTATTR_HASSOL) != 0;
        TimetableMilpResult result{ClassifyStatus(raw_status, has_solution), raw_status,
                                  model.GetDblAttr(COPT_DBLATTR_SOLVINGTIME), std::nullopt};
        if (result.status == TimetableStatus::Feasible) {
            result.solution = ExtractSolution(timetable, variables);
            // 与 SAT 一致：提取解后先按原约束独立检查，通过后才向调用方返回可行结果。
            // 检查失败返回诊断错误，不作为“已证不可行”，外层不得据此加割。
            if (const auto error = ValidateTimetableSolution(timetable, *result.solution)) return *error;
        }
        return result;
    } catch (const CoptException& error) {
        return InputError{"求解器错误", "SolveTimetableMilp", "COPT 错误码 " + std::to_string(error.GetCode()) + "，" + error.what()};
    } catch (const std::exception& error) {
        return InputError{"时刻表求解错误", "SolveTimetableMilp", error.what()};
    }
}

void PrintTimetableMilpReport(const TimetableData& timetable, const TimetableMilpResult& result, std::ostream& output) {
    const char* status = result.status == TimetableStatus::Feasible ? "可行"
        : result.status == TimetableStatus::ProvenInfeasible ? "已证不可行" : "未定";
    output << "第一层时刻表状态=" << status << "，COPT 状态码=" << result.solver_status
           << "，求解耗时=" << result.solving_seconds << " 秒\n";
    if (result.solver_status == COPT_STATUS_TIMEOUT) output << "终止原因=达到时限\n";
    if (result.solver_status == COPT_STATUS_INTERRUPTED) output << "终止原因=求解中断\n";
    if (!result.solution) return;
    const auto& solution = *result.solution;
    output << "具体时刻表（编号均为内部编号，时刻单位为秒）\n";
    for (const auto& [train_index, train] : timetable.trains) {
        output << "列车 " << train_index << " 方向=" << (train.direction == Direction::Up ? "up" : "down");
        if (train.candidate_index) output << " 候选=" << *train.candidate_index;
        else output << " 既有";
        output << '\n';
        for (const auto visit_index : train.visit_indices) {
            const auto& visit = timetable.visits.at(visit_index);
            const auto arrival_event = visit.arrival_side_event_index;
            const auto departure_event = visit.departure_side_event_index;
            // 先提升到 64 位再换算秒，报告换算不改变模型的整数时间单位。
            const auto arrival_seconds = static_cast<std::int64_t>(solution.event_times.at(arrival_event)) * timetable.SecondsPerStep();
            const auto departure_seconds = static_cast<std::int64_t>(solution.event_times.at(departure_event)) * timetable.SecondsPerStep();
            output << "  访问 " << visit_index << " 车站=" << visit.station_index
                   << " 轨道=" << solution.visit_platforms.at(visit_index)
                   << (timetable.events.at(arrival_event).kind == Event::StartOccupation ? " 开始占轨=" : " 实际到达=") << arrival_seconds
                   << (timetable.events.at(departure_event).kind == Event::ReleaseTrack ? " 释放轨道=" : " 实际出发=") << departure_seconds << '\n';
        }
    }
}
