///////////////////////////////////////////////////////////////////////////
/// @file timetable_solution.cpp
/// @brief 用原始时间差和轨道编号独立检查可行见证。
///////////////////////////////////////////////////////////////////////////
#include "timetable_solution.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace {
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

std::optional<InputError> ValidateTimetableSolution(
    const TimetableData& data, const TimetableSolution& solution) {
    try {
        Require(solution.event_times.size() == data.events.size()
            && solution.visit_platforms.size() == data.visits.size()
            && solution.orders.size() == data.section_orders.size(), "见证维度错误。");
        // 提升为 64 位后再相减，避免极端输入的时间差溢出。
        const auto time = [&](std::size_t event) -> std::int64_t { return solution.event_times.at(event); };
        for (std::size_t e = 0; e < data.events.size(); ++e) {
            const auto& domain = data.events[e].time_domain; // 当前事件允许的闭区间。
            Require(time(e) >= domain.lower && time(e) <= domain.upper,
                "事件时间域违反：" + std::to_string(e));
        }
        for (const auto& activity : data.activities) {
            const auto duration = time(activity.to_event_index) - time(activity.from_event_index);
            Require(activity.has_fixed_duration() ? duration == activity.duration : duration >= activity.duration,
                "活动时长违反，起事件：" + std::to_string(activity.from_event_index));
        }
        for (std::size_t v = 0; v < data.visits.size(); ++v) {
            const auto& visit = data.visits[v]; // 一趟车在一站的访问，不是车站实体。
            const int track = solution.visit_platforms[v]; // 解中选定的原轨道编号。
            Require(std::find(visit.allowed_platform_indices.begin(), visit.allowed_platform_indices.end(), track)
                != visit.allowed_platform_indices.end(), "所选轨道不在允许集合。");
            Require(!visit.fixed_platform_index || *visit.fixed_platform_index == track, "既有固定轨道改变。");
        }
        for (std::size_t c = 0; c < data.section_orders.size(); ++c) {
            const auto& section = data.section_orders[c];
            const auto& first_entry = data.visits.at(section.first_entry_visit_index);
            const auto& first_exit = data.visits.at(section.first_exit_visit_index);
            const auto& second_entry = data.visits.at(section.second_entry_visit_index);
            const auto& second_exit = data.visits.at(section.second_exit_visit_index);
            Require(first_entry.train_index == first_exit.train_index
                && second_entry.train_index == second_exit.train_index
                && first_entry.train_index < second_entry.train_index
                && first_entry.station_index == second_entry.station_index
                && first_exit.station_index == second_exit.station_index, "区间访问关系错误：" + std::to_string(c));
            const bool first = solution.orders[c].first_before_second;
            const auto departure = time(second_entry.departure_side_event_index) - time(first_entry.departure_side_event_index);
            const auto arrival = time(second_exit.arrival_side_event_index) - time(first_exit.arrival_side_event_index);
            Require((first ? departure : -departure) >= section.departure_headway, "区间入口出发间隔违反：" + std::to_string(c));
            Require((first ? arrival : -arrival) >= section.arrival_headway, "区间出口到达间隔违反：" + std::to_string(c));
        }
        for (const auto& conflict : data.track_conflicts) {
            const auto& first = data.visits.at(conflict.first_visit_index);
            const auto& second = data.visits.at(conflict.second_visit_index);
            const auto& section = data.section_orders.at(conflict.section_order_index);
            Require(first.train_index < second.train_index && first.station_index == second.station_index,
                "轨道冲突访问关系错误。");
            const bool incoming = section.first_exit_visit_index == conflict.first_visit_index
                && section.second_exit_visit_index == conflict.second_visit_index;
            const bool outgoing = section.first_entry_visit_index == conflict.first_visit_index
                && section.second_entry_visit_index == conflict.second_visit_index;
            Require(incoming || outgoing, "轨道冲突与区间次序不匹配。");
            if (solution.visit_platforms.at(conflict.first_visit_index)
                == solution.visit_platforms.at(conflict.second_visit_index)) {
                const bool first_before = solution.orders.at(conflict.section_order_index).first_before_second;
                const auto gap = first_before
                    ? time(second.arrival_side_event_index) - time(first.departure_side_event_index)
                    : time(first.arrival_side_event_index) - time(second.departure_side_event_index);
                Require(gap >= data.track_headway, "同轨占用间隔违反。");
            }
        }
        return std::nullopt;
    } catch (const std::exception& error) {
        return InputError{"时刻表见证错误", "ValidateTimetableSolution", error.what()};
    }
}
