///////////////////////////////////////////////////////////////////////////
/// @file timetable_data.cpp
/// @brief 构建并报告第一层时刻表验证所需的数据。
/// 第一步：建立每一趟车自身的数据 -- AppendTrainVisitsAndActivities()
/// 第二步：建立区间次序与站内轨道冲突。
///////////////////////////////////////////////////////////////////////////
#include "timetable_data.h"
#include "master_problem.h"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <iterator>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

// 一趟列车在有向区间两端的访问，下标均指向 TimetableData::visits。
struct SectionTraversal {
    std::size_t entry_visit_index;
    std::size_t exit_visit_index;
};

// 以有向区间内部编号分组，每组按原列车编号递增。
using SectionTraversals = std::map<int, std::vector<SectionTraversal>>;

// 追加一个事件并返回 events 下标。只有既有车的实际到发使用公布时刻窗口。
std::size_t AppendEvent(TimetableData& timetable, const InputParameters& parameters,
                        Event kind, std::size_t visit_index, std::optional<int> baseline_time) {
    TimeDomain domain{0, parameters.planning_horizon};
    if (baseline_time && (kind == Event::Arrival || kind == Event::Departure)) {
        const int published_time = *baseline_time;
        if (published_time < 0 || published_time > parameters.planning_horizon)
            throw std::invalid_argument("既有列车实际时刻超出规划时域。");
        // 先限制可用后移量，避免公布时刻和后移参数直接相加溢出。
        const int delay = std::min(parameters.existing_train_max_delay,
                                   parameters.planning_horizon - published_time);
        domain = {published_time, published_time + delay};
    }
    const std::size_t event_index = timetable.events.size();  // 新事件的稳定编号。
    timetable.events.push_back({kind, visit_index, domain, baseline_time});
    return event_index;
}

// 统一取得一次访问的可用轨道，供车站冲突计算交集。
std::vector<int> CollectAllowedPlatformIndices(const Train& train, std::size_t station_position) {
    if (const auto fixed = train.fixed_platform_indices()[station_position]) return {*fixed};

    std::vector<int> indices;  // 转为稳定内部编号，便于求共同轨道。
    for (const auto* platform : train.line()->allowed_platforms()[station_position])
        indices.push_back(platform->index());
    std::sort(indices.begin(), indices.end());
    return indices;
}

// 生成访问内两个事件的时差要求。通过是等式，停站和端点占轨取下界。
TimetableActivity MakeStationActivity(const Train& train, std::size_t station_position,
                                     const TimetableVisit& visit, int minimum_terminal_occupation) {
    const bool origin = station_position == 0;
    const bool destination = station_position + 1 == train.line()->stations().size();
    const auto& stops = train.line()->stop_positions();
    Activity kind = Activity::Pass;
    int duration = 0;
    if (origin || destination) {
        kind = origin ? Activity::OriginOccupation : Activity::DestinationOccupation;
        duration = minimum_terminal_occupation;
    } else if (std::binary_search(stops.begin(), stops.end(), station_position)) {
        // 即使停站下界为零，也按停站方案区分停车与等时通过。
        kind = Activity::Stop;
        duration = train.minimum_dwell_times()[station_position];
    }
    return {kind, visit.arrival_side_event_index, visit.departure_side_event_index, duration};
}

// 沿线路依次追加一趟车的访问、事件和活动，同时登记它实际经过的有向区间。
void AppendTrainVisitsAndActivities(TimetableData& timetable, SectionTraversals& traversals,
                                    const InputParameters& parameters, const Train& train) {
    const Line& line = *train.line();
    auto [train_record, inserted] = timetable.trains.emplace(
        train.index(), TimetableTrain{line.direction(), train.candidate_plan_index(), {}});
    if (!inserted) throw std::invalid_argument("参与验证的列车编号重复。");
    auto& route_visits = train_record->second.visit_indices;  // 当前车沿途的访问编号。

    for (std::size_t position = 0; position < line.stations().size(); ++position) {
        const bool origin = position == 0;
        const bool destination = position + 1 == line.stations().size();
        const std::size_t visit_index = timetable.visits.size();

        // 模型的到达侧和出发侧包含端点占轨边界，实际时刻窗口只用于真实到发。
        //建立到达侧和出发侧事件，并计算时间域
        const auto arrival_event_index = AppendEvent(timetable, parameters,
            origin ? Event::StartOccupation : Event::Arrival, visit_index,
            origin ? train.published_start_track_occupation_time() : train.published_arrival_times()[position]);
        const auto departure_event_index = AppendEvent(timetable, parameters,
            destination ? Event::ReleaseTrack : Event::Departure, visit_index,
            destination ? train.published_end_track_occupation_time() : train.published_departure_times()[position]);
        timetable.visits.push_back({train.index(), line.stations()[position]->index(),
            arrival_event_index, departure_event_index, train.fixed_platform_indices()[position],
            CollectAllowedPlatformIndices(train, position)});

        if (!origin) {
            const std::size_t previous_visit_index = route_visits.back();
            const std::size_t section_position = position - 1;  // 到当前站的区间在线路中的位置。
            const int section_index = line.sections()[section_position]->index();
            const auto previous_departure_event_index = timetable.visits[previous_visit_index].departure_side_event_index;
            timetable.activities.push_back({Activity::Run, previous_departure_event_index, arrival_event_index,
                train.section_running_times()[section_position]});
            traversals[section_index].push_back({previous_visit_index, visit_index});
        }

        timetable.activities.push_back(MakeStationActivity(train, position, timetable.visits[visit_index],
            parameters.minimum_terminal_occupation));
        route_visits.push_back(visit_index);
    }
}

// selected_trains 按原列车编号排序，因此每个区间的第一车编号较小。
void BuildSectionOrders(TimetableData& timetable, const SectionTraversals& traversals,
                        const InputParameters& parameters) {
    for (const auto& [section_index, uses] : traversals) {
        for (std::size_t i = 0; i < uses.size(); ++i) {
            for (std::size_t j = i + 1; j < uses.size(); ++j) {
                timetable.section_orders.push_back({section_index, uses[i].entry_visit_index,
                    uses[i].exit_visit_index, uses[j].entry_visit_index, uses[j].exit_visit_index,
                    parameters.arrival_headway, parameters.departure_headway});
            }
        }
    }
}

// 建立同站共轨冲突，并关联可用于确定占用顺序的区间。
void BuildTrackConflicts(TimetableData& timetable) {
	//第一步：按区间出口访问对和入口访问对建立映射，便于查找两车的共同区间 -- 区间出口访问对映射到入站顺序，入口访问对映射到出站顺序。
    using VisitPair = std::pair<std::size_t, std::size_t>;
    std::map<VisitPair, std::size_t> incoming, outgoing;
    for (std::size_t k = 0; k < timetable.section_orders.size(); ++k) {
        const auto& order = timetable.section_orders[k];
        incoming.emplace(VisitPair{order.first_exit_visit_index, order.second_exit_visit_index}, k);
        outgoing.emplace(VisitPair{order.first_entry_visit_index, order.second_entry_visit_index}, k);
    }
    // 第二步：按车站汇集访问，后续只比较同站列车。
    std::map<int, std::vector<std::size_t>> by_station;
    for (std::size_t i = 0; i < timetable.visits.size(); ++i) {
        by_station[timetable.visits[i].station_index].push_back(i);
    }
    // 访问按原列车编号生成，配对顺序与区间顺序中的两车一致。
    for (const auto& [station, visits] : by_station) {
        for (std::size_t i = 0; i < visits.size(); ++i) {
            for (std::size_t j = i + 1; j < visits.size(); ++j) {
                const auto& first = timetable.visits[visits[i]];
                const auto& second = timetable.visits[visits[j]];
                if (first.train_index == second.train_index) continue;
                // 没有共同允许轨道时，不会发生同轨冲突。
                std::vector<int> common;
                //第三步：求两车允许使用的轨道交集
                std::set_intersection(first.allowed_platform_indices.begin(), first.allowed_platform_indices.end(),
                    second.allowed_platform_indices.begin(), second.allowed_platform_indices.end(), std::back_inserter(common));
                if (common.empty()) continue;
                // 第四步：检查共同轨道必须满足模型的方向和相邻区间前提。
                const auto context = "车站 " + std::to_string(station) + " 列车 "
                    + std::to_string(first.train_index) + " 和 " + std::to_string(second.train_index);
                if (timetable.trains.at(first.train_index).direction != timetable.trains.at(second.train_index).direction) {
                    throw std::invalid_argument(context + " 方向不同但可用轨道相交。");
                }
                const VisitPair pair{visits[i], visits[j]};
                const auto in = incoming.find(pair);
                const auto out = outgoing.find(pair);
                if (in == incoming.end() && out == outgoing.end()) {
                    throw std::invalid_argument(context + " 无共同相邻区间但可用轨道相交。");
                }
                // 第五步：选定顺序记录，保存轨道冲突 -- 优先使用共同入站顺序，否则使用出站顺序；实际同轨时才激活占用约束。
                const auto order_index = in != incoming.end() ? in->second : out->second;
                timetable.track_conflicts.push_back({visits[i], visits[j], order_index, std::move(common)});
            }
        }
    }
}

// 为报告显示事件类型的中文名称。
const char* EventName(Event kind) {
    switch (kind) {
        case Event::StartOccupation: return "开始占轨";
        case Event::Arrival: return "实际到达";
        case Event::Departure: return "实际出发";
        case Event::ReleaseTrack: return "释放轨道";
    }
    return "未知事件";
}

// 为报告显示活动类型的中文名称。
const char* ActivityName(Activity kind) {
    switch (kind) {
        case Activity::Run: return "区间运行";
        case Activity::Stop: return "中间停站";
        case Activity::Pass: return "中间通过";
        case Activity::OriginOccupation: return "始发占轨";
        case Activity::DestinationOccupation: return "终到占轨";
    }
    return "未知活动";
}

}  // namespace

InputResult<TimetableData> BuildTimetableData(
    const ProblemData& data, const MasterSolution& master_solution) {
    // 候选由主问题决定，此处复制已选编号以规范记录顺序。
    auto candidates = master_solution.selected_candidate_indices;
    std::sort(candidates.begin(), candidates.end());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const int index = candidates[i];
        if (index < 0 || static_cast<std::size_t>(index) >= data.candidates.size())
            return InputError{"时刻表数据错误", "selected_candidate_indices", "候选编号越界：" + std::to_string(index)};
        if (i > 0 && candidates[i - 1] == index)
            return InputError{"时刻表数据错误", "selected_candidate_indices", "候选编号重复：" + std::to_string(index)};
    }

    try {
        const auto& parameters = data.parameters;
        TimetableData timetable{};
        timetable.planning_horizon = parameters.planning_horizon;
        timetable.time_step_minutes = parameters.time_step_minutes;
        timetable.time_step_seconds = parameters.time_step_seconds;
        timetable.track_headway = parameters.track_headway;
        timetable.selected_candidate_indices = std::move(candidates);

        // 仅在构建期间引用输入列车，返回结果使用内部编号。
        std::vector<const Train*> selected_trains;
        for (const auto& train : data.existing_trains) selected_trains.push_back(train.get());
        for (const int index : timetable.selected_candidate_indices) {
            const auto& tasks = data.candidates[index]->trains();
            selected_trains.insert(selected_trains.end(), tasks.begin(), tasks.end());
        }
        std::sort(selected_trains.begin(), selected_trains.end(), [](const Train* first, const Train* second) {
            return first->index() < second->index();
        });

        SectionTraversals traversals;
        for (const auto* train : selected_trains)
            AppendTrainVisitsAndActivities(timetable, traversals, parameters, *train);
        BuildSectionOrders(timetable, traversals, parameters);
        BuildTrackConflicts(timetable);
        return timetable;
    } catch (const std::exception& error) {
        return InputError{"时刻表数据错误", "BuildTimetableData", error.what()};
    }
}

void PrintTimetableDataReport(const TimetableData& timetable, std::ostream& output) {
    const auto seconds = [&](int value) {
        return static_cast<std::int64_t>(value) * timetable.SecondsPerStep();
    };
    output << "时刻表数据（编号均为内部编号）\n"
           << "规划时域=" << timetable.planning_horizon << " time_step，每步=" << timetable.SecondsPerStep()
           << " 秒，同轨间隔=" << timetable.track_headway << " time_step\n"
           << "选中候选：";
    for (const int index : timetable.selected_candidate_indices) output << ' ' << index;
    if (timetable.selected_candidate_indices.empty()) output << " 无";
    output << "\n列车=" << timetable.trains.size() << "，访问=" << timetable.visits.size()
           << "，事件=" << timetable.events.size() << "，活动=" << timetable.activities.size()
           << "，车站冲突对=" << timetable.track_conflicts.size()
           << "，区间顺序对=" << timetable.section_orders.size() << '\n';

    for (const auto& [index, train] : timetable.trains) {
        output << "列车 " << index << " 方向=" << (train.direction == Direction::Up ? "up" : "down");
        if (train.candidate_index) output << " 候选=" << *train.candidate_index;
        else output << " 既有";
        output << '\n';
        for (const std::size_t visit_index : train.visit_indices) {
            const auto& visit = timetable.visits[visit_index];
            output << "  访问 " << visit_index << " 车站=" << visit.station_index
                   << (visit.fixed_platform_index ? " 固定轨道=" : " 允许轨道=");
            for (const int platform : visit.allowed_platform_indices) output << ' ' << platform;
            output << '\n';
            for (const auto event_index : {visit.arrival_side_event_index, visit.departure_side_event_index}) {
                const auto& event = timetable.events[event_index];
                output << "    事件 " << event_index << ' ' << EventName(event.kind)
                       << " 时间域=[" << seconds(event.time_domain.lower) << ',' << seconds(event.time_domain.upper) << "] 秒";
                if (event.baseline_time) output << " 基准=" << seconds(*event.baseline_time) << " 秒";
                output << '\n';
            }
        }
    }
    for (std::size_t i = 0; i < timetable.activities.size(); ++i) {
        const auto& activity = timetable.activities[i];
        output << "活动 " << i << ' ' << ActivityName(activity.kind) << " 事件=" << activity.from_event_index
               << "->" << activity.to_event_index << " 时长" << (activity.has_fixed_duration() ? "=" : ">=")
               << seconds(activity.duration) << " 秒\n";
    }
    for (std::size_t i = 0; i < timetable.track_conflicts.size(); ++i) {
        const auto& conflict = timetable.track_conflicts[i];
        output << "轨道冲突 " << i << " 访问=" << conflict.first_visit_index << ',' << conflict.second_visit_index
               << " 区间次序=" << conflict.section_order_index << " 共同轨道=";
        for (const int platform : conflict.common_platform_indices) output << ' ' << platform;
        output << '\n';
    }
    for (std::size_t i = 0; i < timetable.section_orders.size(); ++i) {
        const auto& order = timetable.section_orders[i];
        output << "区间顺序 " << i << " 区间=" << order.section_index << " 访问="
               << order.first_entry_visit_index << ',' << order.first_exit_visit_index << ','
               << order.second_entry_visit_index << ',' << order.second_exit_visit_index
               << " 出发间隔=" << seconds(order.departure_headway)
               << " 到达间隔=" << seconds(order.arrival_headway) << " 秒\n";
    }
}
