///////////////////////////////////////////////////////////////////////////
/// @file network.h
/// @brief 增量线路计划的核心网络、线路、列车与 OD 数据类型
///////////////////////////////////////////////////////////////////////////

#ifndef NETWORK_H
#define NETWORK_H

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class Platform;
class Station;
class Section;
class RollingStock;
class Line;
class Train;
class CandidatePlan;

/// @brief 车站内的一条到发线（track）。
class Platform {
public:
    Platform(int index,
             std::string name,
             int station_index,
             int existing_occupied_time,
             int total_available_time)
        : index_(index),
          name_(std::move(name)),
          station_index_(station_index),
          existing_occupied_time_(existing_occupied_time),
          total_available_time_(total_available_time) {
        if (index_ < 0 || station_index_ < 0) {
            throw std::invalid_argument("到发线或所属车站索引不能为负数。");
        }
        if (name_.empty()) {
            throw std::invalid_argument("到发线名称不能为空。");
        }
        if (existing_occupied_time_ < 0 || total_available_time_ < 0) {
            throw std::invalid_argument("到发线时间不能为负数。");
        }
        if (existing_occupied_time_ > total_available_time_) {
            throw std::invalid_argument("到发线既有占用时间不能超过总可用时间。");
        }
    }

    int index() const noexcept { return index_; }
    const std::string& name() const noexcept { return name_; }
    int station_index() const noexcept { return station_index_; }
    int existing_occupied_time() const noexcept {
        return existing_occupied_time_;
    }
    int total_available_time() const noexcept { return total_available_time_; }
    int remaining_available_time() const noexcept {
        return total_available_time_ - existing_occupied_time_;
    }

private:
    int index_;
    std::string name_;
    int station_index_;
    int existing_occupied_time_;
    int total_available_time_;
};

/// @brief 车站实体及其所属到发线集合。
class Station {
public:
    Station(int index,
            std::string name,
            std::vector<const Platform*> platforms)
        : index_(index),
          name_(std::move(name)),
          platforms_(std::move(platforms)) {
        if (index_ < 0) {
            throw std::invalid_argument("车站索引不能为负数。");
        }
        if (name_.empty()) {
            throw std::invalid_argument("车站名称不能为空。");
        }
        ValidatePlatforms();
    }

    int index() const noexcept { return index_; }
    const std::string& name() const noexcept { return name_; }
    const std::vector<const Platform*>& platforms() const noexcept {
        return platforms_;
    }

    bool ContainsPlatform(const Platform* platform) const noexcept {
        return std::find(platforms_.begin(), platforms_.end(), platform)
               != platforms_.end();
    }

    bool ContainsPlatformIndex(int platform_index) const noexcept {
        return std::any_of(platforms_.begin(),
                           platforms_.end(),
                           [platform_index](const Platform* platform) {
                               return platform->index() == platform_index;
                           });
    }

private:
    void ValidatePlatforms() const {
        std::unordered_set<const Platform*> unique_platforms;
        std::unordered_set<int> unique_platform_indices;
        for (const Platform* platform : platforms_) {
            if (platform == nullptr) {
                throw std::invalid_argument("车站到发线集合不能包含空指针。");
            }
            if (!unique_platforms.insert(platform).second) {
                throw std::invalid_argument("车站到发线集合不能包含重复对象。");
            }
            if (!unique_platform_indices.insert(platform->index()).second) {
                throw std::invalid_argument("车站到发线索引不能重复。");
            }
            if (platform->station_index() != index_) {
                throw std::invalid_argument("到发线记录的所属车站与当前车站不一致。");
            }
        }
    }

    int index_;
    std::string name_;
    std::vector<const Platform*> platforms_;
};

/// @brief 有向区间所属的运行方向。
enum class Direction {
    Up,
    Down
};

/// @brief 两个相邻车站之间的有向区间。
class Section {
public:
    Section(int index,
            const Station* from_station,
            const Station* to_station,
            Direction direction,
            int existing_train_count,
            int maximum_train_count)
        : index_(index),
          from_station_(from_station),
          to_station_(to_station),
          direction_(direction),
          existing_train_count_(existing_train_count),
          maximum_train_count_(maximum_train_count) {
        if (index_ < 0) {
            throw std::invalid_argument("区间索引不能为负数。");
        }
        if (from_station_ == nullptr || to_station_ == nullptr) {
            throw std::invalid_argument("区间端点车站不能为空。");
        }
        if (from_station_ == to_station_) {
            throw std::invalid_argument("区间的两个端点车站不能相同。");
        }
        if (existing_train_count_ < 0 || maximum_train_count_ < 0) {
            throw std::invalid_argument("区间列车数不能为负数。");
        }
        if (existing_train_count_ > maximum_train_count_) {
            throw std::invalid_argument("区间既有列车数不能超过最大列车数。");
        }
    }

    int index() const noexcept { return index_; }
    const Station* from_station() const noexcept { return from_station_; }
    const Station* to_station() const noexcept { return to_station_; }
    /// @brief 返回输入数据中指定的运行方向。
    Direction direction() const noexcept { return direction_; }
    int existing_train_count() const noexcept { return existing_train_count_; }
    int maximum_train_count() const noexcept { return maximum_train_count_; }
    int remaining_train_capacity() const noexcept {
        return maximum_train_count_ - existing_train_count_;
    }

private:
    int index_;
    const Station* from_station_;
    const Station* to_station_;
    Direction direction_; ///< sections.csv:direction — 区间所属的上下行方向
    int existing_train_count_;
    int maximum_train_count_;
};

/// @brief 动车组类型及其可用数量。
class RollingStock {
public:
    RollingStock(int index,
                 int seat_capacity,
                 int available_units,
                 int minimum_turnaround_time)
        : index_(index),
          seat_capacity_(seat_capacity),
          available_units_(available_units),
          minimum_turnaround_time_(minimum_turnaround_time) {
        if (index_ < 0 || seat_capacity_ <= 0 || available_units_ < 0
            || minimum_turnaround_time_ < 0) {
            throw std::invalid_argument("动车组参数非法。");
        }
    }

    int index() const noexcept { return index_; }
    int seat_capacity() const noexcept { return seat_capacity_; }
    int available_units() const noexcept { return available_units_; }
    int minimum_turnaround_time() const noexcept {
        return minimum_turnaround_time_;
    }

private:
    int index_;
    int seat_capacity_;
    int available_units_;
    int minimum_turnaround_time_;
};

/// @brief 一条完整线路及其相邻停车站之间的区段集合。
class Line {
public:
    Line(int index,
         std::vector<const Station*> stations,
         std::vector<const Section*> sections,
         std::vector<std::size_t> stop_positions,
         std::vector<std::vector<const Platform*>> allowed_platforms,
         std::vector<int> allowed_frequencies,
         std::vector<const RollingStock*> allowed_rolling_stocks,
         std::vector<std::vector<int>> od_indices_by_stop_section = {})
        : index_(index),
          stations_(std::move(stations)),
          sections_(std::move(sections)),
          stop_positions_(std::move(stop_positions)),
          od_indices_by_stop_section_(std::move(od_indices_by_stop_section)),
          allowed_platforms_(std::move(allowed_platforms)),
          allowed_frequencies_(std::move(allowed_frequencies)),
          allowed_rolling_stocks_(std::move(allowed_rolling_stocks)) {
        Validate();
        BuildStopSections();
        if (od_indices_by_stop_section_.empty()) {
            od_indices_by_stop_section_.resize(stop_sections_.size());
        } else if (od_indices_by_stop_section_.size() != stop_sections_.size()) {
            throw std::invalid_argument("OD 覆盖列表必须与停车区段对齐。");
        }
    }

    int index() const noexcept { return index_; }
    const std::vector<const Station*>& stations() const noexcept {
        return stations_;
    }
    const std::vector<const Section*>& sections() const noexcept {
        return sections_;
    }
    /// @brief 返回线路各区间共同的运行方向。
    Direction direction() const noexcept { return sections_.front()->direction(); }
    const std::vector<std::size_t>& stop_positions() const noexcept {
        return stop_positions_;
    }
    /// @brief 返回相邻停车站之间的区段 \f$\mathcal Q_a^{\mathrm{stop}}\f$。
    const std::vector<std::pair<const Station*, const Station*>>&
    stop_sections() const noexcept {
        return stop_sections_;
    }
    /// @brief 按停车区段顺序返回可乘车经过该段的 OD 内部编号（座位容量系数）。
    const std::vector<std::vector<int>>& od_indices_by_stop_section() const noexcept {
        return od_indices_by_stop_section_;
    }
    const std::vector<std::vector<const Platform*>>& allowed_platforms()
        const noexcept {
        return allowed_platforms_;
    }
    const std::vector<int>& allowed_frequencies() const noexcept {
        return allowed_frequencies_;
    }
    const std::vector<const RollingStock*>& allowed_rolling_stocks()
        const noexcept {
        return allowed_rolling_stocks_;
    }

    bool HasDirectStopService(int origin_index,
                              int destination_index) const noexcept {
        std::optional<std::size_t> origin_position;
        std::optional<std::size_t> destination_position;
        for (const std::size_t position : stop_positions_) {
            if (stations_[position]->index() == origin_index) {
                origin_position = position;
            }
            if (stations_[position]->index() == destination_index) {
                destination_position = position;
            }
        }
        return origin_position.has_value() && destination_position.has_value()
               && *origin_position < *destination_position;
    }

private:
    void Validate() const {
        if (index_ < 0) {
            throw std::invalid_argument("线路索引不能为负数。");
        }
        if (stations_.size() < 2 || sections_.size() + 1 != stations_.size()) {
            throw std::invalid_argument("线路车站与区间数量不匹配。");
        }

        std::unordered_set<const Station*> unique_stations;
        for (const Station* station : stations_) {
            if (station == nullptr || !unique_stations.insert(station).second) {
                throw std::invalid_argument("线路车站不能为空或重复。");
            }
        }

        std::unordered_set<const Section*> unique_sections;
        for (std::size_t i = 0; i < sections_.size(); ++i) {
            const Section* section = sections_[i];
            if (section == nullptr || !unique_sections.insert(section).second) {
                throw std::invalid_argument("线路区间不能为空或重复。");
            }
            if (section->from_station() != stations_[i]
                || section->to_station() != stations_[i + 1]) {
                throw std::invalid_argument("线路区间端点与相邻车站不一致。");
            }
            if (section->direction() != direction()) {
                throw std::invalid_argument("线路各区间的运行方向必须一致，方向不一致的区间："
                    + section->from_station()->name() + "->" + section->to_station()->name());
            }
        }

        if (stop_positions_.size() < 2) {
            throw std::invalid_argument("线路至少需要两个停站位置。");
        }
        std::unordered_set<std::size_t> unique_stop_positions;
        for (const std::size_t position : stop_positions_) {
            if (position >= stations_.size()
                || !unique_stop_positions.insert(position).second) {
                throw std::invalid_argument("线路停站位置越界或重复。");
            }
        }
        if (!std::is_sorted(stop_positions_.begin(), stop_positions_.end())) {
            throw std::invalid_argument("线路停站位置必须按运行方向递增。");
        }

        if (allowed_platforms_.size() != stations_.size()) {
            throw std::invalid_argument("线路可用到发线集合必须与车站序列对齐。");
        }
        for (std::size_t i = 0; i < allowed_platforms_.size(); ++i) {
            std::unordered_set<const Platform*> unique_platforms;
            for (const Platform* platform : allowed_platforms_[i]) {
                if (platform == nullptr
                    || !stations_[i]->ContainsPlatform(platform)
                    || !unique_platforms.insert(platform).second) {
                    throw std::invalid_argument(
                        "线路可用到发线必须非空、属于对应车站且不重复。");
                }
            }
        }

        std::unordered_set<int> unique_frequencies;
        for (const int frequency : allowed_frequencies_) {
            if (frequency <= 0 || !unique_frequencies.insert(frequency).second) {
                throw std::invalid_argument("线路允许频率必须为正数且不重复。");
            }
        }

        std::unordered_set<const RollingStock*> unique_rolling_stocks;
        for (const RollingStock* rolling_stock : allowed_rolling_stocks_) {
            if (rolling_stock == nullptr
                || !unique_rolling_stocks.insert(rolling_stock).second) {
                throw std::invalid_argument("线路允许动车组不能为空或重复。");
            }
        }
    }

    void BuildStopSections() {
        if (stop_positions_.size() < 2) {
            return;
        }
        stop_sections_.reserve(stop_positions_.size() - 1);
        for (std::size_t i = 0; i + 1 < stop_positions_.size(); ++i) {
            stop_sections_.push_back(
                {stations_[stop_positions_[i]],
                 stations_[stop_positions_[i + 1]]});
        }
    }

    int index_;
    std::vector<const Station*> stations_;
    std::vector<const Section*> sections_;
    std::vector<std::size_t> stop_positions_;
    std::vector<std::pair<const Station*, const Station*>> stop_sections_;
    std::vector<std::vector<int>> od_indices_by_stop_section_;
    std::vector<std::vector<const Platform*>> allowed_platforms_;
    std::vector<int> allowed_frequencies_;
    std::vector<const RollingStock*> allowed_rolling_stocks_;
};

enum class TrainKind {
    Existing,
    Added
};

/// @brief 一列具体列车及其线路相关运行、停站和时刻数据。
class Train {
public:
    Train(int index,
          TrainKind kind,
          const Line* line,
          std::vector<int> section_running_times,
          std::vector<int> minimum_dwell_times,
          std::vector<std::optional<int>> published_arrival_times,
          std::vector<std::optional<int>> published_departure_times,
          std::vector<std::optional<int>> fixed_platform_indices,
          std::optional<int> candidate_plan_index,
          std::optional<int> published_start_track_occupation_time,
          std::optional<int> published_end_track_occupation_time)
        : index_(index),
          kind_(kind),
          line_(line),
          section_running_times_(std::move(section_running_times)),
          minimum_dwell_times_(std::move(minimum_dwell_times)),
          published_arrival_times_(std::move(published_arrival_times)),
          published_departure_times_(std::move(published_departure_times)),
          fixed_platform_indices_(std::move(fixed_platform_indices)),
          candidate_plan_index_(candidate_plan_index),
          published_start_track_occupation_time_(published_start_track_occupation_time),
          published_end_track_occupation_time_(published_end_track_occupation_time) {
        Validate();
    }

    int index() const noexcept { return index_; }
    TrainKind kind() const noexcept { return kind_; }
    const Line* line() const noexcept { return line_; }
    const std::vector<int>& section_running_times() const noexcept {
        return section_running_times_;
    }
    const std::vector<int>& minimum_dwell_times() const noexcept {
        return minimum_dwell_times_;
    }
    const std::vector<std::optional<int>>& published_arrival_times()
        const noexcept {
        return published_arrival_times_;
    }
    const std::vector<std::optional<int>>& published_departure_times()
        const noexcept {
        return published_departure_times_;
    }
    const std::vector<std::optional<int>>& fixed_platform_indices()
        const noexcept {
        return fixed_platform_indices_;
    }
    /// @brief 空值表示既有列车；有值表示唯一所属候选方案的索引。
    const std::optional<int>& candidate_plan_index() const noexcept {
        return candidate_plan_index_;
    }

    /// @brief 返回始发站基准开始占轨时间；新增任务为空。
    const std::optional<int>& published_start_track_occupation_time() const noexcept {
        return published_start_track_occupation_time_;
    }

    /// @brief 返回终到站基准释放轨道时间；新增任务为空。
    const std::optional<int>& published_end_track_occupation_time() const noexcept {
        return published_end_track_occupation_time_;
    }

private:
    void Validate() const {
        if (index_ < 0 || line_ == nullptr) {
            throw std::invalid_argument("列车索引或所属线路非法。");
        }
        const std::size_t station_count = line_->stations().size();
        if (section_running_times_.size() != line_->sections().size()
            || minimum_dwell_times_.size() != station_count
            || published_arrival_times_.size() != station_count
            || published_departure_times_.size() != station_count
            || fixed_platform_indices_.size() != station_count) {
            throw std::invalid_argument("列车运行数据必须与线路序列对齐。");
        }
        for (const int running_time : section_running_times_) {
            if (running_time < 0) {
                throw std::invalid_argument("列车区间运行时间不能为负数。");
            }
        }
        for (std::size_t i = 0; i < station_count; ++i) {
            if (kind_ == TrainKind::Existing) {
                if (!fixed_platform_indices_[i]
                    || published_arrival_times_[i].has_value() != (i > 0)
                    || published_departure_times_[i].has_value()
                           != (i + 1 < station_count)) {
                    throw std::invalid_argument("既有列车实际事件或固定轨道不完整。");
                }
            } else if (published_arrival_times_[i]
                       || published_departure_times_[i] || fixed_platform_indices_[i]) {
                throw std::invalid_argument("新增任务不能带有既有公布时刻或固定轨道。");
            }
            if (minimum_dwell_times_[i] < 0) {
                throw std::invalid_argument("列车最小停站时间不能为负数。");
            }
            if (published_arrival_times_[i].has_value()
                && *published_arrival_times_[i] < 0) {
                throw std::invalid_argument("公布到达时刻不能为负数。");
            }
            if (published_departure_times_[i].has_value()
                && *published_departure_times_[i] < 0) {
                throw std::invalid_argument("公布出发时刻不能为负数。");
            }
            if (published_arrival_times_[i].has_value()
                && published_departure_times_[i].has_value()
                && *published_arrival_times_[i]
                       > *published_departure_times_[i]) {
                throw std::invalid_argument("公布到达时刻不能晚于出发时刻。");
            }
            if (fixed_platform_indices_[i].has_value()
                && !line_->stations()[i]->ContainsPlatformIndex(
                    *fixed_platform_indices_[i])) {
                throw std::invalid_argument("列车固定到发线不属于对应车站。");
            }
        }
        if (kind_ == TrainKind::Existing) {
            if (!published_start_track_occupation_time_
                || !published_end_track_occupation_time_
                || *published_start_track_occupation_time_ < 0
                || *published_start_track_occupation_time_
                       > *published_departure_times_.front()
                || *published_end_track_occupation_time_
                       < *published_arrival_times_.back()) {
                throw std::invalid_argument("既有列车始发占轨或终到释放时间缺失、为负或顺序错误。");
            }
        } else if (published_start_track_occupation_time_
                   || published_end_track_occupation_time_) {
            throw std::invalid_argument("新增任务不能带有既有始终点占轨时间。");
        }
        if (candidate_plan_index_.has_value()
            && *candidate_plan_index_ < 0) {
            throw std::invalid_argument("候选方案索引不能为负数。");
        }
        if (kind_ == TrainKind::Existing && candidate_plan_index_.has_value()) {
            throw std::invalid_argument("既有列车不能属于新增候选方案。");
        }
        if (kind_ == TrainKind::Added && !candidate_plan_index_.has_value()) {
            throw std::invalid_argument("新增列车必须且只能属于一个候选方案。");
        }
    }

    int index_;
    TrainKind kind_;
    const Line* line_;
    std::vector<int> section_running_times_;
    std::vector<int> minimum_dwell_times_;
    std::vector<std::optional<int>> published_arrival_times_;
    std::vector<std::optional<int>> published_departure_times_;
    std::vector<std::optional<int>> fixed_platform_indices_;
    std::optional<int> candidate_plan_index_;
    std::optional<int> published_start_track_occupation_time_; ///< [内部计算] 实际始发减最小端点占轨时长
    std::optional<int> published_end_track_occupation_time_; ///< [内部计算] 实际终到加最小端点占轨时长
};

/// @brief 一条线路在给定频率、动车组与具体新增列车下的候选开行方案。
class CandidatePlan {
public:
    CandidatePlan(int index,
                  const Line* line,
                  int frequency,
                  const RollingStock* rolling_stock,
                  int minimum_trip_time,
                  std::vector<int> minimum_platform_occupation_times,
                  std::vector<const Train*> trains)
        : index_(index),
          line_(line),
          frequency_(frequency),
          rolling_stock_(rolling_stock),
          minimum_trip_time_(minimum_trip_time),
          minimum_platform_occupation_times_(
              std::move(minimum_platform_occupation_times)),
          trains_(std::move(trains)) {
        Validate();
    }

    int index() const noexcept { return index_; }
    const Line* line() const noexcept { return line_; }
    int frequency() const noexcept { return frequency_; }
    const RollingStock* rolling_stock() const noexcept {
        return rolling_stock_;
    }
    int minimum_trip_time() const noexcept { return minimum_trip_time_; }
    const std::vector<int>& minimum_platform_occupation_times()
        const noexcept {
        return minimum_platform_occupation_times_;
    }
    const std::vector<const Train*>& trains() const noexcept {
        return trains_;
    }

private:
    void Validate() const {
        if (index_ < 0 || line_ == nullptr || rolling_stock_ == nullptr
            || frequency_ <= 0 || minimum_trip_time_ < 0) {
            throw std::invalid_argument("候选方案基本参数非法。");
        }
        if (std::find(line_->allowed_frequencies().begin(),
                      line_->allowed_frequencies().end(),
                      frequency_)
            == line_->allowed_frequencies().end()) {
            throw std::invalid_argument("候选方案频率不在线路允许集合中。");
        }
        if (std::find(line_->allowed_rolling_stocks().begin(),
                      line_->allowed_rolling_stocks().end(),
                      rolling_stock_)
            == line_->allowed_rolling_stocks().end()) {
            throw std::invalid_argument("候选方案动车组不在线路允许集合中。");
        }
        if (minimum_platform_occupation_times_.size()
            != line_->stations().size()) {
            throw std::invalid_argument("最小到发线占用时间必须与线路车站对齐。");
        }
        for (const int occupation_time : minimum_platform_occupation_times_) {
            if (occupation_time < 0) {
                throw std::invalid_argument("最小到发线占用时间不能为负数。");
            }
        }
        if (trains_.size() != static_cast<std::size_t>(frequency_)) {
            throw std::invalid_argument("候选方案列车数必须等于开行频率。");
        }
        std::unordered_set<const Train*> unique_trains;
        for (const Train* train : trains_) {
            if (train == nullptr || !unique_trains.insert(train).second) {
                throw std::invalid_argument("候选方案列车不能为空或重复。");
            }
            if (train->kind() != TrainKind::Added || train->line() != line_
                || train->candidate_plan_index() != index_) {
                throw std::invalid_argument("候选方案列车的类型、线路或归属不一致。");
            }
        }
    }

    int index_;
    const Line* line_;
    int frequency_;
    const RollingStock* rolling_stock_;
    int minimum_trip_time_;
    std::vector<int> minimum_platform_occupation_times_;
    std::vector<const Train*> trains_;
};

/// @brief 一对新增客流 OD 及其候选直达开行方案集合。
class OD {
public:
    OD(int index,
       int origin_index,
       std::string origin_name,
       int destination_index,
       std::string destination_name,
       double demand,
       int shortest_travel_time,
       double unmet_unit_penalty,
       std::vector<const CandidatePlan*> candidate_plans)
        : index_(index),
          origin_index_(origin_index),
          origin_name_(std::move(origin_name)),
          destination_index_(destination_index),
          destination_name_(std::move(destination_name)),
          demand_(demand),
          shortest_travel_time_(shortest_travel_time),
          unmet_unit_penalty_(unmet_unit_penalty),
          candidate_plans_(std::move(candidate_plans)) {
        Validate();
    }

    int index() const noexcept { return index_; }
    int origin_index() const noexcept { return origin_index_; }
    const std::string& origin_name() const noexcept { return origin_name_; }
    int destination_index() const noexcept { return destination_index_; }
    const std::string& destination_name() const noexcept {
        return destination_name_;
    }
    double demand() const noexcept { return demand_; }
    int shortest_travel_time() const noexcept { return shortest_travel_time_; }
    double unmet_unit_penalty() const noexcept { return unmet_unit_penalty_; }
    /// @brief 返回 \f$\mathcal X_{od}\f$。
    const std::vector<const CandidatePlan*>& candidate_plans() const noexcept {
        return candidate_plans_;
    }

private:
    void Validate() const {
        if (index_ < 0 || origin_index_ < 0 || destination_index_ < 0
            || origin_index_ == destination_index_) {
            throw std::invalid_argument("OD 索引或端点非法。");
        }
        if (origin_name_.empty() || destination_name_.empty()) {
            throw std::invalid_argument("OD 端点名称不能为空。");
        }
        if (demand_ < 0.0 || shortest_travel_time_ < 0
            || unmet_unit_penalty_ < 0.0) {
            throw std::invalid_argument("OD 需求、时间或惩罚参数不能为负数。");
        }
        std::unordered_set<const CandidatePlan*> unique_candidate_plans;
        for (const CandidatePlan* plan : candidate_plans_) {
            if (plan == nullptr
                || !unique_candidate_plans.insert(plan).second) {
                throw std::invalid_argument("OD 候选方案不能为空或重复。");
            }
            if (!plan->line()->HasDirectStopService(origin_index_,
                                                    destination_index_)) {
                throw std::invalid_argument("OD 候选方案不能提供顺向直达停站服务。");
            }
        }
    }

    int index_;
    int origin_index_;
    std::string origin_name_;
    int destination_index_;
    std::string destination_name_;
    double demand_;
    int shortest_travel_time_;
    double unmet_unit_penalty_;
    std::vector<const CandidatePlan*> candidate_plans_;
};

#endif  // NETWORK_H
