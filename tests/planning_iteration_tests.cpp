///////////////////////////////////////////////////////////////////////////
/// @file planning_iteration_tests.cpp
/// @brief 使用真实 C++ 模型及当前 Data 验证迭代基础行为，不模拟求解判定。
///////////////////////////////////////////////////////////////////////////
#include "../augmentation line plan/augmentation line plan/planning_iteration.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message); // Release 下同样执行。
}

void CheckReports(const PlanningResult& result) {
    Require(std::filesystem::is_regular_file(result.output_directory / "summary.txt"), "缺少汇总报告");
    for (const auto& round : result.rounds) {
        const auto path = result.output_directory / ("round_" + std::to_string(round.number) + ".txt");
        Require(std::filesystem::file_size(path) > 0, "缺少逐轮报告");
        Require(round.added_conflict.empty(), "当前验证路径不应添加冲突割");
    }
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        Require(argc == 2, "需要当前 Data 目录");
        auto loaded = LoadProblemData(argv[1]);
        if (const auto* error = std::get_if<InputError>(&loaded)) throw std::runtime_error(error->message);
        const auto& data = std::get<ProblemData>(loaded);

        // 同一 session 连续求解：验证对象生命周期与持久变量句柄，不重建静态模型。
        auto created = MasterProblemSession::Create(data);
        Require(std::holds_alternative<std::unique_ptr<MasterProblemSession>>(created), "持久主问题构建失败");
        auto& session = *std::get<std::unique_ptr<MasterProblemSession>>(created);
        for (int solve = 0; solve < 2; ++solve) {
            const auto solved = session.Solve(60);
            Require(std::holds_alternative<MasterProblemResult>(solved), "持久主问题求解失败");
            const auto& result = std::get<MasterProblemResult>(solved);
            Require(result.solution && std::abs(result.solution->objective_value - 144900) < 1e-6,
                    "当前算例主问题目标错误");
        }
        // 非法割必须在修改 COPT 模型之前拒绝；这里不人为声称某个有效集合不可行。
        Require(std::holds_alternative<InputError>(session.AddConflictCut({})), "空割未拒绝");
        Require(std::holds_alternative<InputError>(session.AddConflictCut({-1})), "越界割未拒绝");
        Require(std::holds_alternative<InputError>(session.Solve(-1)), "负求解时限未拒绝");

        PlanningOptions options(data.parameters);
        options.output_root = "tmp/planning_iteration_tests";
        options.timetable.solver_logging = false;
        options.maximum_rounds = 1; // 首轮可行应优先返回成功，而非误报轮数上限。
        const auto feasible = RunPlanningIteration(data, options);
        Require(feasible.status == PlanningStatus::Feasible && feasible.rounds.size() == 1,
                "当前算例未在首轮取得可行解");
        const auto& round = feasible.rounds.front();
        Require(round.master && round.master->solution && round.timetable && round.timetable->solution,
                "未完整保留主问题解与第一层见证");
        Require(round.timetable_data->selected_candidate_indices
                == round.master->solution->selected_candidate_indices, "候选传递不一致");
        Require(round.timetable->solution->event_times.size() == round.timetable_data->events.size(),
                "事件见证与数据不对应");
        CheckReports(feasible);
        std::cout << "PASS current_data_first_round_feasible_and_reports\n";

        // 实际 COPT 零时限求解，不使用伪造状态或替代模型。
        options.timetable.time_limit_seconds = 0;
        const auto undetermined = RunPlanningIteration(data, options);
        Require(undetermined.status == PlanningStatus::Undetermined && undetermined.rounds.size() == 1,
                "第一层零时限应保留未定状态");
        Require(undetermined.rounds.front().timetable
                && undetermined.rounds.front().timetable->status == TimetableStatus::Undetermined,
                "第一层原始业务状态缺失");
        CheckReports(undetermined);
        std::cout << "PASS real_timetable_timeout_without_cut\n";

        options.total_time_limit_seconds = 0;
        const auto timed = RunPlanningIteration(data, options);
        Require(timed.status == PlanningStatus::TimeLimit && timed.rounds.empty(), "零总预算未及时结束");
        CheckReports(timed);
        options.maximum_rounds = 0;
        const auto invalid = RunPlanningIteration(data, options);
        Require(invalid.status == PlanningStatus::Error && invalid.error, "非法轮数未报告错误");
        std::cout << "PASS persistent_solves_invalid_inputs_and_total_budget\n";

        // 用 C++ 生成独立 CSV 副本并经正式加载器读取，验证改表确实控制正式入口。
        const auto fixture = feasible.output_directory / "csv_parameters";
        std::filesystem::create_directory(fixture);
        for (const auto& entry : std::filesystem::directory_iterator(argv[1]))
            if (entry.path().extension() == ".csv")
                std::filesystem::copy_file(entry.path(), fixture / entry.path().filename());
        const auto write_parameters = [&](const std::string& rounds, bool omit_rounds = false,
                                          const std::string& master_logging = "0", bool omit_master_logging = false) {
            std::ifstream source(std::filesystem::path(argv[1]) / "parameters.csv");
            std::ofstream target(fixture / "parameters.csv");
            source.exceptions(std::ios::badbit);
            target.exceptions(std::ios::failbit | std::ios::badbit);
            std::string line; // 保留无关参数，只改本测试需要核对的配置项。
            while (std::getline(source, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                // 修改数值时保留第四列说明，保证测试副本与实际四列参数表一致。
                const auto unit_end = line.find(',', line.find(',', line.find(',') + 1) + 1);
                const auto meaning = unit_end == std::string::npos ? std::string{} : line.substr(unit_end);
                if (line.starts_with("maximum_rounds,")) {
                    if (omit_rounds) continue;
                    line = "maximum_rounds," + rounds + ",round";
                } else if (line.starts_with("total_time_limit_seconds,")) line = "total_time_limit_seconds,0,second";
                else if (line.starts_with("master_time_limit_seconds,")) line = "master_time_limit_seconds,1.25,second";
                else if (line.starts_with("master_solver_logging,")) {
                    if (omit_master_logging) continue;
                    line = "master_solver_logging," + master_logging + ",boolean";
                }
                else if (line.starts_with("timetable_time_limit_seconds,")) line = "timetable_time_limit_seconds,0.5,second";
                else if (line.starts_with("timetable_solver_logging,")) line = "timetable_solver_logging,0,boolean";
                else if (line.starts_with("planning_output_root,")) line = "planning_output_root,tmp/csv_controlled_runs,path";
                if (line.find(',', line.find(',', line.find(',') + 1) + 1) == std::string::npos)
                    line += meaning;
                target << line << '\n';
            }
        };
        write_parameters("7");
        auto modified = LoadProblemData(fixture);
        Require(std::holds_alternative<ProblemData>(modified), "修改后的 CSV 加载失败");
        const auto& modified_data = std::get<ProblemData>(modified);
        const PlanningOptions csv_options(modified_data.parameters);
        Require(csv_options.maximum_rounds == 7 && csv_options.master_time_limit_seconds == 1.25
                && csv_options.timetable.time_limit_seconds == 0.5 && !csv_options.timetable.solver_logging
                && csv_options.output_root == std::filesystem::path("tmp/csv_controlled_runs"),
                "CSV 参数未完整传入迭代设置");
        const auto csv_run = RunPlanningIteration(modified_data); // 不传覆盖设置，与 main 的调用完全相同。
        Require(csv_run.status == PlanningStatus::TimeLimit && csv_run.rounds.empty(),
                "数据表中的零总预算未控制正式入口");
        CheckReports(csv_run);
        // 两种日志设置均经 CSV 加载，并在同一主问题对象上连续求解两次。
        // 标记仅包围 Solve，便于核对求解日志，避免混入 COPT 环境初始化信息。
        for (const std::string logging : {"0", "1"}) {
            write_parameters("7", false, logging);
            auto logging_loaded = LoadProblemData(fixture);
            Require(std::holds_alternative<ProblemData>(logging_loaded), "主问题日志参数加载失败");
            const auto& logging_data = std::get<ProblemData>(logging_loaded);
            Require(logging_data.parameters.master_solver_logging == (logging == "1"), "主问题日志参数值错误");
            auto logging_created = MasterProblemSession::Create(logging_data);
            Require(std::holds_alternative<std::unique_ptr<MasterProblemSession>>(logging_created), "日志测试主问题构建失败");
            auto& logging_session = *std::get<std::unique_ptr<MasterProblemSession>>(logging_created);
            std::cout << "BEGIN_MASTER_LOGGING_" << logging << std::endl;
            for (int solve = 0; solve < 2; ++solve) {
                const auto logging_result = logging_session.Solve(60);
                Require(std::holds_alternative<MasterProblemResult>(logging_result), "日志测试求解失败");
                const auto& master = std::get<MasterProblemResult>(logging_result);
                Require(master.solution && std::abs(master.solution->objective_value - 144900) < 1e-6,
                        "切换日志开关后主问题目标错误");
            }
            std::cout << "END_MASTER_LOGGING_" << logging << std::endl;
        }
        for (const std::string invalid_logging : {"-1", "2", "0.5", "true"}) {
            write_parameters("7", false, invalid_logging);
            Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "非法主问题日志参数未拒绝");
        }
        write_parameters("7", false, "0", true);
        Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "缺失主问题日志参数未拒绝");
        std::cout << "PASS master_logging_csv_both_modes_repeated_solves_and_validation\n";
        write_parameters("0");
        Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "CSV 非法轮数未拒绝");
        write_parameters("7", true);
        Require(std::holds_alternative<InputError>(LoadProblemData(fixture)), "CSV 缺失参数未拒绝");
        std::cout << "PASS csv_configuration_controls_actual_entry_and_rejects_invalid_or_missing_values\n";
        std::cout << "Deferred: proven-infeasible/cut/reselection fixture, as requested.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
