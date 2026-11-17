///////////////////////////////////////////////////////////////////////////
/// @file input_data.h
/// @brief CSV 数据加载、对象所有权和派生服务参数接口。
///////////////////////////////////////////////////////////////////////////
#ifndef INPUT_DATA_H
#define INPUT_DATA_H

#include "network.h"

#include <filesystem>
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

/// @brief 加载失败的统一诊断信息。
struct InputError {
    std::string category; ///< [内部计算] 错误类别
    std::string context;  ///< [内部计算] 文件、行号、字段
    std::string message;  ///< [内部计算] 中文原因
};

/// @brief C++20 下统一的成功值/错误结果；用 std::get_if 检查错误分支。
template <typename T>
using InputResult = std::variant<T, InputError>;

/// @brief 全局输入参数；运行图时间使用 time_step，求解控制时限使用实际秒数。
struct InputParameters {
    int planning_horizon;                 ///< parameters.csv:planning_horizon
    int time_step_minutes;                ///< parameters.csv:time_step_minutes
    double unmet_demand_penalty_factor;   ///< parameters.csv:unmet_demand_penalty_factor
    int track_headway;                    ///< parameters.csv:track_headway
    int arrival_headway;                  ///< parameters.csv:arrival_headway
    int departure_headway;                ///< parameters.csv:departure_headway
    int minimum_stop_occupation;          ///< parameters.csv:minimum_stop_platform_occupation_time
    int minimum_pass_occupation;          ///< parameters.csv:minimum_pass_platform_occupation_time
    int minimum_terminal_occupation;      ///< parameters.csv:minimum_terminal_platform_occupation_time
    int existing_train_max_delay;         ///< parameters.csv:existing_train_max_delay；仅实际事件受限后移
    int startup_additional_time;          ///< parameters.csv:startup_additional_time
    int stopping_additional_time;         ///< parameters.csv:stopping_additional_time
    std::vector<int> candidate_frequencies; ///< parameters.csv:candidate_frequencies
    int maximum_rounds;                    ///< parameters.csv:maximum_rounds，正整数。
    double total_time_limit_seconds;       ///< parameters.csv:total_time_limit_seconds，全流程墙钟预算。
    double master_time_limit_seconds;      ///< parameters.csv:master_time_limit_seconds，单轮主问题上限。
    bool master_solver_logging;            ///< parameters.csv:master_solver_logging，0=关闭、1=开启主问题日志。
    double timetable_time_limit_seconds;   ///< parameters.csv:timetable_time_limit_seconds，单轮第一层上限。
    bool timetable_solver_logging;         ///< parameters.csv:timetable_solver_logging，0/1。
    int timetable_solver;                 ///< parameters.csv:timetable_solver，必填：1=MILP，2=SAT。
    int conflict_cut_mode = 1;             ///< 可选：1=完整集合，2=assumptions 直接核；旧输入默认1。
    double core_verification_time_limit_seconds = 60; ///< 可选：子集重建与复核预算，另受总预算约束。
    std::string planning_output_root;       ///< parameters.csv:planning_output_root，UTF-8 路径。
    int time_step_seconds = 0;             ///< 可选，与 time_step_minutes 二选一；0表示旧分钟输入。
    int existing_timing_mode = 0;          ///< 0=统一运行/停站规则（旧版）；1=既有公布时刻派生逐车运行与停站下界。
    std::int64_t SecondsPerStep() const noexcept {
        return time_step_seconds > 0 ? time_step_seconds : std::int64_t{60} * time_step_minutes;
    }
};

/// @brief OD 在某候选上的旅行时间；与 OD::candidate_plans 一一对应。
struct OdService {
    const CandidatePlan* candidate; ///< [内部计算] 非拥有指针
    int travel_time;                ///< [内部计算] 上车实际出发至下车实际到达
};

/// @brief 拥有全部实体；指针随对象移动仍有效，依赖对象须在本数据集生命周期内使用。
/// @details 内部 index 连续编号，CSV 业务 ID 单独映射。既有专用线路不进入候选池。
struct ProblemData {
    InputParameters parameters{}; ///< parameters.csv
    std::vector<std::unique_ptr<Platform>> platforms; ///< platforms.csv；既有占用由 timetable.csv 累加
    std::vector<std::unique_ptr<Station>> stations; ///< stations.csv
    std::vector<std::unique_ptr<Section>> sections; ///< sections.csv；既有车次数由 timetable.csv 累加
    std::vector<std::unique_ptr<RollingStock>> rolling_stocks; ///< parameters.csv:long_*/short_*
    std::vector<std::unique_ptr<Line>> lines; ///< lines.csv 与 line_allowed_platforms.csv
    std::vector<std::unique_ptr<Line>> existing_lines; ///< [内部计算] 每列既有车的专用线路描述
    std::vector<std::unique_ptr<Train>> existing_trains; ///< timetable.csv
    std::vector<std::unique_ptr<Train>> added_trains; ///< [内部计算] 按候选、任务序号生成
    std::vector<std::unique_ptr<CandidatePlan>> candidates; ///< [内部计算] 按线路业务 ID、频率、车型顺序生成
    std::vector<std::unique_ptr<OD>> ods; ///< ods.csv
    std::map<int, const Station*> station_by_id; ///< stations.csv:station_id
    std::map<std::string, const Station*> station_by_name; ///< stations.csv:station_name
    std::map<int, const Platform*> platform_by_id; ///< platforms.csv:platform_id
    std::map<int, const Section*> section_by_id; ///< sections.csv:section_id
    std::map<int, const Line*> line_by_id; ///< lines.csv:line_id
    std::map<std::string, const Train*> existing_train_by_id; ///< timetable.csv:trainID
    std::map<int, const OD*> od_by_id; ///< ods.csv:od_id
    std::vector<std::string> rolling_stock_names; ///< parameters.csv:long/short 前缀；按车型 index 访问
    std::vector<int> pure_running_times; ///< sections.csv:pure_running_time；按区间 index 访问
    std::vector<std::vector<OdService>> od_services; ///< [内部计算] 按 OD index 访问
};

/// @brief 加载整个数据目录；不执行候选容量筛选或运营可行性求解。
/// @param directory 包含八个输入 CSV 的目录。
/// @return 完整数据集或含文件/行号/字段的错误；失败不暴露部分构建结果。
InputResult<ProblemData> LoadProblemData(const std::filesystem::path& directory);

/// @brief 输出对象数量、资源统计、候选任务映射和 OD 派生参数。
void PrintInputReport(const ProblemData& data, std::ostream& output);

#endif  // INPUT_DATA_H
