///////////////////////////////////////////////////////////////////////////
/// @file input_data_tests.cpp
/// @brief 用已知算例验证对象关联、时间语义、业务 ID 映射和移动后引用。
///////////////////////////////////////////////////////////////////////////
#include "../augmentation line plan/augmentation line plan/input_data.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckSectionDirections(const ProblemData& data) {
    Require(data.sections.at(0)->direction() == Direction::Down
        && data.sections.at(1)->direction() == Direction::Up,
        "A、B 间两个有向区间的方向错误");
    Require(data.lines.at(0)->direction() == Direction::Down
        && data.lines.at(3)->direction() == Direction::Up,
        "候选线路的上下行方向错误");
    Require(data.existing_train_by_id.at("100")->line()->direction() == Direction::Down
        && data.existing_train_by_id.at("200")->line()->direction() == Direction::Up,
        "既有列车的上下行方向错误");

    // 保留一条下行线路的路径，只把第二个区间标为上行。
    const auto& line = *data.lines.at(0);
    auto sections = line.sections();
    const auto& original = *sections.at(1);
    const Section wrong_direction(original.index(), original.from_station(), original.to_station(),
        Direction::Up, original.existing_train_count(), original.maximum_train_count());
    sections.at(1) = &wrong_direction;
    bool rejected = false;
    try {
        const Line mixed(line.index(), line.stations(), std::move(sections), line.stop_positions(),
            line.allowed_platforms(), line.allowed_frequencies(), line.allowed_rolling_stocks());
    } catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()).find("运行方向必须一致") != std::string::npos;
    }
    Require(rejected, "混用上下行区间的线路未被正确拒绝");
}

void CheckStopSectionOdCoverage(const ProblemData& data) {
    for (const auto& line : data.lines) {
        const auto& stops = line->stop_sections();
        const auto& coverage = line->od_indices_by_stop_section();
        Require(coverage.size() == stops.size(), "OD 覆盖列表与停车区段不对齐");
        const auto& route = line->stations();
        const auto position = [&](int station_index) {
            return std::find_if(route.begin(), route.end(), [&](const Station* station) {
                return station->index() == station_index;
            });
        };
        for (std::size_t segment = 0; segment < stops.size(); ++segment) {
            const auto first = position(stops[segment].first->index());
            const auto last = position(stops[segment].second->index());
            std::vector<int> expected;
            for (const auto& od : data.ods) {
                const auto origin = od->origin_index();
                const auto destination = od->destination_index();
                if (line->HasDirectStopService(origin, destination)
                    && position(origin) <= first && last <= position(destination)) {
                    expected.push_back(od->index());
                }
            }
            Require(coverage[segment] == expected, "停车区段覆盖的 OD 错误");
        }
    }
    for (const auto& line : data.existing_lines) {
        const auto& coverage = line->od_indices_by_stop_section();
        Require(coverage.size() == line->stop_sections().size(), "既有线路覆盖列表未对齐");
        for (const auto& segment : coverage) {
            Require(segment.empty(), "既有线路进入新增 OD 的覆盖映射");
        }
    }
}

void CheckData(const ProblemData& data) {
    Require(data.stations.size() == 5 && data.platforms.size() == 30
        && data.sections.size() == 8 && data.lines.size() == 20
        && data.existing_trains.size() == 10 && data.existing_lines.size() == 10
        && data.candidates.size() == 80 && data.added_trains.size() == 120
        && data.ods.size() == 6, "对象数量错误");
    Require(data.parameters.existing_train_max_delay == 5, "后移参数未保留");
    Require(data.parameters.minimum_terminal_occupation == 15, "端点占轨参数错误");
    Require(data.rolling_stocks[0]->seat_capacity() == 550
        && data.rolling_stocks[1]->seat_capacity() == 1100, "车型容量被调换");
    CheckSectionDirections(data);
    CheckStopSectionOdCoverage(data);
    for (const auto& [id, station] : data.station_by_id)
        Require(data.stations.at(station->index()).get() == station, "车站业务 ID/内部索引混用");
    for (const auto& [id, platform] : data.platform_by_id)
        Require(data.platforms.at(platform->index()).get() == platform
            && data.stations.at(platform->station_index())->ContainsPlatform(platform), "轨道引用错误");
    for (const auto& [id, line] : data.line_by_id)
        Require(data.lines.at(line->index()).get() == line, "线路业务 ID/内部索引混用");

    const auto* train100 = data.existing_train_by_id.at("100");
    Require(!train100->published_arrival_times().front()
        && train100->published_departure_times().front() == 15
        && train100->published_arrival_times().back() == 107
        && !train100->published_departure_times().back(), "实际到发事件边界错误");
    Require(train100->published_start_track_occupation_time() == 0
        && train100->published_end_track_occupation_time() == 122
        && data.existing_train_by_id.at("204")->published_end_track_occupation_time() == 240,
        "始终点占轨边界派生错误");
    const auto rejects_boundaries = [&](std::optional<int> start, std::optional<int> end) {
        try {
            Train invalid(train100->index(), train100->kind(), train100->line(),
                train100->section_running_times(), train100->minimum_dwell_times(),
                train100->published_arrival_times(), train100->published_departure_times(),
                train100->fixed_platform_indices(), train100->candidate_plan_index(), start, end);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    Require(rejects_boundaries(std::nullopt, 122)
        && rejects_boundaries(0, std::nullopt)
        && rejects_boundaries(-1, 122)
        && rejects_boundaries(16, 122)
        && rejects_boundaries(0, 106), "始终点占轨边界校验失效");
    const auto* train101 = data.existing_train_by_id.at("101");
    Require(train101->published_arrival_times()[1] == 51
        && train101->published_departure_times()[1] == 51
        && train101->minimum_dwell_times()[1] == 0, "通过活动被计作停车");
    Require(train101->section_running_times() == std::vector<int>({15, 18, 19, 18}), "起停附加时分错误");

    std::map<int, int> occupied, traversals;
    std::set<int> train_indices, line_indices;
    for (const auto& line : data.lines) Require(line_indices.insert(line->index()).second, "线路编号重复");
    for (const auto& line : data.existing_lines) Require(line_indices.insert(line->index()).second, "既有线路编号重复");
    for (const auto& train : data.existing_trains) {
        Require(train_indices.insert(train->index()).second, "列车编号重复");
        const auto& line = *train->line();
        Require(line.allowed_frequencies().empty(), "既有线路混入候选服务");
        for (std::size_t i = 0; i < line.stations().size(); ++i) {
            const int start = i == 0 ? *train->published_start_track_occupation_time()
                                    : *train->published_arrival_times()[i];
            const int end = i + 1 == line.stations().size()
                ? *train->published_end_track_occupation_time()
                : *train->published_departure_times()[i];
            const bool terminal = i == 0 || i + 1 == line.stations().size();
            Require(start >= 0 && end <= data.parameters.planning_horizon, "既有访问时刻超出规划时域");
            if (terminal) Require(end - start == 15, "既有端点占轨时长错误");
            const bool stop = std::binary_search(line.stop_positions().begin(), line.stop_positions().end(), i);
            occupied[*train->fixed_platform_indices()[i]] += stop ? end - start : 2;
        }
        for (const auto* section : line.sections()) ++traversals[section->index()];
    }
    for (const auto& platform : data.platforms)
        Require(platform->existing_occupied_time() == occupied[platform->index()], "轨道占用统计错误");
    for (const auto& section : data.sections)
        Require(section->existing_train_count() == traversals[section->index()], "区间统计错误");

    std::size_t task_count = 0;
    bool found_all_stop = false, found_express = false;
    for (std::size_t i = 0; i < data.candidates.size(); ++i) {
        const auto& plan = *data.candidates[i];
        Require(plan.index() == static_cast<int>(i), "候选编号不连续");
        Require(plan.trains().size() == static_cast<std::size_t>(plan.frequency()), "频率展开错误");
        const auto* line = plan.line();
        const auto& occupations = plan.minimum_platform_occupation_times();
        Require(occupations.front() == 15 && occupations.back() == 15, "候选端点占轨系数错误");
        for (std::size_t j = 1; j + 1 < occupations.size(); ++j) {
            const bool stop = std::binary_search(line->stop_positions().begin(), line->stop_positions().end(), j);
            Require(occupations[j] == (stop ? 4 : 2), "候选中间站占轨系数错误");
        }
        if (line->stations().front()->name() == "A" && line->stations().back()->name() == "E") {
            if (line->stop_positions().size() == 5) {
                Require(plan.minimum_trip_time() == 92, "全停任务时间错误");
                found_all_stop = true;
            }
            if (line->stop_positions().size() == 2) {
                Require(plan.minimum_trip_time() == 65, "直达任务时间错误");
                found_express = true;
            }
        }
        for (const auto* train : plan.trains()) {
            Require(train_indices.insert(train->index()).second, "新增任务不唯一");
            Require(train == data.added_trains.at(task_count++).get(), "任务序号映射错误");
            Require(train->candidate_plan_index() == plan.index() && train->line() == line
                && train->kind() == TrainKind::Added, "候选归属错误");
            Require(!train->published_start_track_occupation_time()
                && !train->published_end_track_occupation_time(), "新增任务带有既有占轨边界");
            for (std::size_t j = 0; j < line->stations().size(); ++j)
                Require(!train->published_arrival_times()[j] && !train->published_departure_times()[j]
                    && !train->fixed_platform_indices()[j],
                    "新增任务被预先赋时或固定到发线");
        }
    }
    Require(found_all_stop && found_express && task_count == 120, "任务覆盖不完整");
    const std::map<std::pair<std::string, std::string>, std::pair<int, int>> expected{
        {{"A", "E"}, {65, 32}}, {{"A", "D"}, {50, 16}}, {{"B", "E"}, {52, 16}},
        {{"E", "A"}, {65, 32}}, {{"E", "C"}, {37, 16}}, {{"D", "A"}, {50, 16}}
    };
    for (const auto& od : data.ods) {
        const auto value = expected.at({od->origin_name(), od->destination_name()});
        Require(od->shortest_travel_time() == value.first
            && od->unmet_unit_penalty() == value.first * 10
            && od->candidate_plans().size() == static_cast<std::size_t>(value.second), "OD 派生值错误");
        const auto& services = data.od_services.at(od->index());
        Require(services.size() == od->candidate_plans().size(), "OD 旅行时间映射缺失");
        for (std::size_t i = 0; i < services.size(); ++i)
            Require(services[i].candidate == od->candidate_plans()[i]
                && services[i].travel_time >= value.first, "OD 服务引用错误");
    }
}

}  // namespace

/// @brief 执行算例断言；Release 下同样执行全部检查。
int wmain(int argc, wchar_t* argv[]) {
    try {
        Require(argc == 2, "需要数据目录参数");
        auto result = LoadProblemData(argv[1]);
        if (const auto* error = std::get_if<InputError>(&result))
            throw std::runtime_error(error->context + " " + error->message);
        auto data = std::move(std::get<ProblemData>(result));
        const auto* first = data.candidates.front().get();
        auto moved = std::move(data);
        Require(first == moved.candidates.front().get(), "移动后对象地址失效");
        CheckData(moved);
        std::cout << "PASS: counts, IDs, ownership, directions, event boundaries, runtimes, candidates, OD services and stop sections\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
