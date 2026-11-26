///////////////////////////////////////////////////////////////////////////
/// @file timetable_data.h
/// @brief 第一层时刻表的事件、活动、轨道和列车次序数据。
///////////////////////////////////////////////////////////////////////////
#ifndef TIMETABLE_DATA_H
#define TIMETABLE_DATA_H

#include "input_data.h"

#include <cstddef>
#include <iosfwd>
#include <map>
#include <optional>
#include <vector>

struct MasterSolution;

/// @brief 区分真实到发事件和始终点的占轨边界。
enum class Event { StartOccupation, Arrival, Departure, ReleaseTrack };

/// @brief 区间运行与通过为固定时长，其余活动使用时长下界。
enum class Activity { Run, Stop, Pass, OriginOccupation, DestinationOccupation };

/// @brief 事件的闭区间整数时间域，单位为 time_step。
struct TimeDomain {
    int lower;
    int upper;
};

/// @brief 一个车站访问中的事件，以在 events 中的下标编号。
struct TimetableEvent {
    Event kind;
    std::size_t visit_index;           ///< [内部编号] 所属车站访问
    TimeDomain time_domain;            ///< [内部计算] 待求时刻的上下界
    std::optional<int> baseline_time;  ///< [内部计算] 既有实际时刻或基准占轨边界
};

/// @brief 从前事件指向后事件的运行或站内活动。
struct TimetableActivity {
    Activity kind;
    std::size_t from_event_index;  ///< [内部编号] 活动开始事件
    std::size_t to_event_index;    ///< [内部编号] 活动结束事件
    int duration;                 ///< [内部计算] 固定时长或时长下界，单位为 time_step

    /// @brief 判断时长是否为等式约束。
    bool has_fixed_duration() const noexcept { return kind == Activity::Run || kind == Activity::Pass; }
};

/// @brief 一趟既有或新增列车在一座车站的访问记录。
/// @details 两类列车都记录占轨起止事件和可用轨道。既有车的可用轨道只有固定轨道，
/// 新增车的可用轨道来自线路在本站的允许轨道集合。
struct TimetableVisit {
    int train_index;                           ///< 原 Train::index()，也是 TimetableData::trains 的键。
    int station_index;                         ///< 原 Station::index()。
    std::size_t arrival_side_event_index;       ///< events 下标，指向实际到达或始发开始占轨。
    std::size_t departure_side_event_index;     ///< events 下标，指向实际出发或终到释放轨道。
    std::optional<int> fixed_platform_index;    ///< 既有车的固定 Platform::index()；新增车为空。
    std::vector<int> allowed_platform_indices;  ///< 本次访问可使用的 Platform::index()，按升序排列。
};

/// @brief 一趟参加验证的列车，按运行顺序引用车站访问。
struct TimetableTrain {
    Direction direction;                        ///< [内部计算] 来自 Line::direction()
    std::optional<int> candidate_index;          ///< [内部编号] 新增车所属候选；既有车为空
    std::vector<std::size_t> visit_indices;      ///< TimetableData::visits 的下标，按车站访问顺序排列。
};

/// @brief 共用有向区间的两车，按原列车编号排序。
struct TimetableSectionOrder {
    int section_index; ///< 原有向区间内部编号。
    std::size_t first_entry_visit_index; ///< 第一车入口 visits 下标。
    std::size_t first_exit_visit_index; ///< 第一车出口 visits 下标。
    std::size_t second_entry_visit_index; ///< 第二车入口 visits 下标。
    std::size_t second_exit_visit_index; ///< 第二车出口 visits 下标。
    int arrival_headway; ///< 出口到达间隔，单位 time_step。
    int departure_headway; ///< 入口出发间隔，单位 time_step。
};

/// @brief 共同可用轨道的两次访问及其关联区间次序。
struct TimetableTrackConflict {
    std::size_t first_visit_index; ///< 第一车 visits 下标。
    std::size_t second_visit_index; ///< 第二车 visits 下标。
    std::size_t section_order_index; ///< section_orders 下标。
    std::vector<int> common_platform_indices; ///< 共同允许的轨道内部编号。
};

/// @brief 一组选中候选与全部既有车对应的时刻表建模输入。
/// @details trains 用原列车内部编号查找，编号可不连续。其余记录按所在 vector 的下标编号。
/// 从列车通过 visit_indices 查访问，再通过访问的两个事件下标查时间域。
/// 活动引用事件，区间次序和轨道冲突引用访问。
/// 时间字段均为整数 time_step，SecondsPerStep() 用于换算。
struct TimetableData {
    int planning_horizon;                         ///< parameters.csv:planning_horizon，时间域上界 H。
    int time_step_minutes;                       ///< parameters.csv:time_step_minutes，每步对应的分钟数。
    int track_headway;                           ///< parameters.csv:track_headway，同轨两次占用之间的间隔。
    std::vector<int> selected_candidate_indices;  ///< 已校验、升序排列的候选内部编号。
    std::map<int, TimetableTrain> trains;          ///< 以原 Train::index() 为键的参与验证列车。
    std::vector<TimetableVisit> visits;            ///< 每趟车每到访一站有一条记录。
    std::vector<TimetableEvent> events;            ///< 每条访问有两个事件，各自保存时间域。
    std::vector<TimetableActivity> activities;     ///< 区间运行和站内占轨对事件时差的要求。
    std::vector<TimetableTrackConflict> track_conflicts; ///< 同站共轨冲突。
    std::vector<TimetableSectionOrder> section_orders;        ///< 每个共用区间列车对的入口与出口次序关联。
    int time_step_seconds = 0;                    ///< 0时兼容旧分钟输入；正数为每步秒数。
    std::int64_t SecondsPerStep() const noexcept {
        return time_step_seconds > 0 ? time_step_seconds : std::int64_t{60} * time_step_minutes;
    }
};

/// @brief 接收主问题提供的可用解，构建对应的时刻表数据。
/// @param data 加载完成的问题数据。
/// @param master_solution 主问题的可用解；读取其中的 selected_candidate_indices 展开车次。
/// @return 时刻表数据，或重复、越界候选编号等错误诊断。
InputResult<TimetableData> BuildTimetableData(
    const ProblemData& data, const MasterSolution& master_solution);

/// @brief 输出构建结果，包含内部编号、时刻、活动和次序关联。
/// @param timetable 构建完成的时刻表数据。
/// @param output 报告输出流。
void PrintTimetableDataReport(const TimetableData& timetable, std::ostream& output);

#endif  // TIMETABLE_DATA_H
