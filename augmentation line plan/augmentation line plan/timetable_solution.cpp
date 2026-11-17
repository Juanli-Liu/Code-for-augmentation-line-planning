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
            && solution.orders.size() == data.station_conflicts.size(), "见证维度错误。");
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
        for (std::size_t c = 0; c < data.station_conflicts.size(); ++c) {
            const auto& conflict = data.station_conflicts[c]; // 两车在同站同向的关系。
            const auto& first = data.visits.at(conflict.first_visit_index);
            const auto& second = data.visits.at(conflict.second_visit_index);
            const auto first_arrival = time(first.arrival_side_event_index);
            const auto second_arrival = time(second.arrival_side_event_index);
            const auto first_departure = time(first.departure_side_event_index);
            const auto second_departure = time(second.departure_side_event_index);
            const auto& order = solution.orders[c]; // 解所声称的两个先后次序。
            Require((order.first_arrives_first ? second_arrival - first_arrival : first_arrival - second_arrival)
                >= conflict.arrival_headway, "到达安全间隔违反。");
            Require((order.first_departs_first ? second_departure - first_departure : first_departure - second_departure)
                >= conflict.departure_headway, "出发安全间隔违反。");
            if (solution.visit_platforms[conflict.first_visit_index] == solution.visit_platforms[conflict.second_visit_index]) {
                Require((order.first_arrives_first ? second_arrival - first_departure : first_arrival - second_departure)
                    >= data.track_headway, "同轨占用间隔违反。");
            }
        }
        for (const auto& section : data.section_orders) {
            Require(solution.orders.at(section.entry_departure_conflict_index).first_departs_first
                == solution.orders.at(section.exit_arrival_conflict_index).first_arrives_first, "区间内发生越行。");
        }
        return std::nullopt;
    } catch (const std::exception& error) {
        return InputError{"时刻表见证错误", "ValidateTimetableSolution", error.what()};
    }
}
