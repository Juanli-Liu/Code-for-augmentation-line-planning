// 直接调用正式 C++ 协调器与 COPT，验证本算例的完整两轮过程。
#include "../augmentation line plan/planning_iteration.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message); // Release 下同样执行断言。
}

void VerifyWitness(const TimetableData& data, const TimetableSolution& solution) {
    Require(solution.event_times.size() == data.events.size(), "事件数量不匹配");
    Require(solution.visit_platforms.size() == data.visits.size(), "轨道数量不匹配");
    for (std::size_t e = 0; e < data.events.size(); ++e) {
        const auto& domain = data.events[e].time_domain;
        const int time = solution.event_times[e];
        Require(time >= domain.lower && time <= domain.upper, "事件时刻超出时间域");
    }
    for (const auto& activity : data.activities) {
        const int duration = solution.event_times.at(activity.to_event_index)
                           - solution.event_times.at(activity.from_event_index);
        Require(activity.has_fixed_duration() ? duration == activity.duration : duration >= activity.duration,
                "运行、停站、通过或端点占轨约束违反");
    }
    for (std::size_t v = 0; v < data.visits.size(); ++v) {
        const auto& visit = data.visits[v];
        const int platform = solution.visit_platforms[v];
        Require(std::find(visit.allowed_platform_indices.begin(), visit.allowed_platform_indices.end(), platform)
                    != visit.allowed_platform_indices.end(), "分配了不允许的轨道");
        Require(!visit.fixed_platform_index || platform == *visit.fixed_platform_index, "既有轨道改变");
    }
    // 本例恰好一上行、一下行，允许轨道也分离；没有需额外检查的同向冲突。
    Require(data.trains.size() == 2 && data.station_conflicts.empty() && data.section_orders.empty(),
            "本例列车/冲突集合与设计不符");
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        Require(argc == 2, "请传入 Test_data 路径");
        auto loaded = LoadProblemData(argv[1]);
        if (const auto* error = std::get_if<InputError>(&loaded)) throw std::runtime_error(error->message);
        const auto& data = std::get<ProblemData>(loaded);
        Require(data.candidates.size() == 4 && data.existing_trains.size() == 1, "候选/既有车数量错误");

        const auto result = RunPlanningIteration(data); // 使用 CSV 配置，不伪造状态、不注入测试割。
        PrintPlanningReport(result, std::cout);
        Require(result.status == PlanningStatus::Feasible && !result.error, "完整迭代未成功");
        Require(result.rounds.size() == 2, "预期恰好两轮");
        const auto& first = result.rounds[0];
        const auto& second = result.rounds[1];
        Require(first.master && first.master->solution && second.master && second.master->solution, "主问题解缺失");
        Require(first.master->solution->selected_candidate_indices == std::vector<int>{0}, "首轮未唯一选中候选0");
        Require(second.master->solution->selected_candidate_indices == std::vector<int>{2}, "第二轮未唯一换选候选2");
        Require(std::abs(first.master->solution->objective_value - 2500) < 1e-6, "首轮目标错误");
        Require(std::abs(second.master->solution->objective_value - 3400) < 1e-6, "第二轮目标错误");
        Require(first.timetable && first.timetable->status == TimetableStatus::ProvenInfeasible
                    && !first.timetable->solution, "首轮没有取得明确不可行判定");
        Require(first.added_conflict == first.master->solution->selected_candidate_indices, "未使用完整选中集合生成割");
        Require(second.added_conflict.empty(), "第二轮可行却生成了割");
        Require(second.timetable_data && second.timetable && second.timetable->status == TimetableStatus::Feasible
                    && second.timetable->solution, "第二轮缺少可行见证");
        for (const auto* round : {&first, &second}) {
            const auto& allocation = round->master->solution->od_allocations.at(0);
            Require(std::abs(allocation.unmet_demand) < 1e-6 && allocation.services.size() == 1
                        && std::abs(allocation.services[0].passengers - 100) < 1e-6, "OD 客流分配错误");
        }
        VerifyWitness(*second.timetable_data, *second.timetable->solution);
        for (const auto* name : {"configuration.txt", "round_1.txt", "round_2.txt", "summary.txt"})
            Require(std::filesystem::file_size(result.output_directory / name) > 0, "结果文件缺失");
        std::cout << "PASS: candidate 0 -> proven infeasible -> cut x_0 <= 0 -> candidate 2 -> feasible\n"
                  << "PASS: objectives 2500/3400, OD=100, complete cut, saved rounds, independently checked witness\n";
        // 验证“最后允许的一轮恰好加割”时不会误报整个问题不可行，也不会继续求解。
        PlanningOptions limited_options(data.parameters);
        limited_options.maximum_rounds = 1;
        const auto limited = RunPlanningIteration(data, limited_options);
        Require(limited.status == PlanningStatus::RoundLimit && limited.rounds.size() == 1,
                "达到轮数上限后仍继续求解或返回了错误状态");
        Require(limited.rounds[0].added_conflict == std::vector<int>{0}
                    && limited.rounds[0].timetable->status == TimetableStatus::ProvenInfeasible,
                "轮数用尽前的判定和割没有保留");
        Require(std::filesystem::file_size(limited.output_directory / "round_1.txt") > 0
                    && std::filesystem::file_size(limited.output_directory / "summary.txt") > 0,
                "轮数限制退出时结果没有保存");
        std::cout << "PASS: round limit preserves the proven-infeasible round and its cut\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
