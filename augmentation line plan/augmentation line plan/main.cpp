///////////////////////////////////////////////////////////////////////////
/// @file main.cpp
/// @brief 从代码指定的数据目录加载输入，执行完整加割迭代。
///////////////////////////////////////////////////////////////////////////
#include "input_data.h"
#include "master_problem.h"
#include "timetable_data.h"
#include "timetable_milp.h"
#include "planning_iteration.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <variant>

#include <Windows.h>

namespace {

// 输入数据目录的唯一设置位置。切换算例时只修改这里，然后重新编译运行。
// 当前读取 Test_data_complex；可改为 Test_data（基础验收）或 Data（原算例）。
// 使用绝对路径，不受 Visual Studio 工作目录或命令参数影响。
// 第一层求解器每次运行从该目录 parameters.csv 的 timetable_solver 读取：1=MILP，2=SAT。
// 切换求解器只改参数值，无需重新编译；Meaning 列用于说明参数用途。
// SAT下可增加 conflict_cut_mode=2 启用直接核；缺省1保留完整集合割。
constexpr wchar_t kInputDataDirectory[] =
    LR"(D:\C++Project\augmentation line plan code\augmentation line plan\Test_data_complex)";

class ScopedUtf8ConsoleOutput {
public:
    ScopedUtf8ConsoleOutput() {
        // 控制台输出句柄也可能只有写权限，GetConsoleMode 需要读权限。
        if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR
            || GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_CHAR) {
            const UINT code_page = GetConsoleOutputCP();
            if (code_page != 0 && code_page != CP_UTF8 && SetConsoleOutputCP(CP_UTF8)) {
                original_code_page_ = code_page;
            }
        }
    }

    ScopedUtf8ConsoleOutput(const ScopedUtf8ConsoleOutput&) = delete;
    ScopedUtf8ConsoleOutput& operator=(const ScopedUtf8ConsoleOutput&) = delete;

    ~ScopedUtf8ConsoleOutput() {
        if (original_code_page_ != 0) {
            // 先写完 UTF-8 输出，再恢复启动方共享的控制台代码页。
            std::cout.flush();
            std::cerr.flush();
            SetConsoleOutputCP(original_code_page_);
        }
    }

private:
    UINT original_code_page_ = 0;
};

/* 历史目录查找逻辑，供下方已注释的旧入口恢复时使用；现行入口不自动查找目录。
std::filesystem::path FindDataDirectory(const std::filesystem::path& executable) {
    // 同时覆盖 VS 工作目录、项目根目录及 x64/Debug、x64/Release 启动。
    for (auto base : {std::filesystem::current_path(), std::filesystem::absolute(executable).parent_path()}) {
        while (!base.empty()) {
            for (const auto& candidate : {base / "Data", base / "augmentation line plan" / "Data"})
                if (std::filesystem::is_regular_file(candidate / "parameters.csv")) return candidate;
            const auto parent = base.parent_path();
            if (parent == base) break;
            base = parent;
        }
    }
    throw std::runtime_error("找不到 Data 目录，请通过命令行指定。");
}
*/

}  // namespace

/// @brief 唯一运行入口：加载输入后执行完整加割迭代。
/// @details 输入路径统一由上方 kInputDataDirectory 指定，不读取命令行参数。
int wmain() {
    // 项目文件职责导航（.h 声明接口/数据结构，配套 .cpp 实现具体逻辑）：
    //   network.h                  铁路基础实体：车站、区间、轨道、线路、列车、候选及 OD。
    //   input_data.h/.cpp          读取并校验 CSV，构建实体、候选及 OD 到停车区段的映射。
    //   master_problem.h/.cpp      COPT 主问题：选择候选、分配客流、管理资源约束及追加冲突割。
    //   timetable_data.h/.cpp      将选中候选展开为具体列车，构建事件、活动、时间域和冲突关系。
    //   timetable_milp.h/.cpp      第一层 MILP：用 COPT 求时刻、轨道与次序的可行解。
    //   timetable_sat.h/.cpp       第一层 SAT：生成布尔子句，用 CaDiCaL 判定并解码具体时刻表。
    //   timetable_solution.h/.cpp  MILP/SAT 共用状态与见证，并按原约束独立检查可行结果。
    //   planning_iteration.h/.cpp  总协调：分派 MILP/SAT、管理预算、判定是否加割并保存逐轮报告。
    //                              原 timetable_solver 的求解/报告分派已合并到此文件。
    //   main.cpp                   程序入口：指定数据目录，调用加载与迭代，展示结果及返回退出码。
    //   tests/timetable_sat_tests.cpp  C++ 验收入口，不参与生产求解流程。
    //   timetable_sat.props        共用编译依赖与测试工程源码清单，不包含运行时求解逻辑。
    //
    // 本函数调用主线：LoadProblemData -> RunPlanningIteration -> 输出最后一轮及整体结果。
    // RunPlanningIteration 内部：主问题 -> 时刻表数据 -> 按 timetable_solver=1/2 分派
    // MILP/SAT -> 可行时结束；已证不可行才加完整集合割重求；未定不加割。
    const ScopedUtf8ConsoleOutput console_output;
    try {
        const std::filesystem::path directory(kInputDataDirectory);
        const auto directory_utf8 = directory.u8string(); // 中文路径按 UTF-8 输出，便于核对实际读取位置。
        std::cout << "输入数据目录=";
        std::cout.write(reinterpret_cast<const char*>(directory_utf8.data()),
                        static_cast<std::streamsize>(directory_utf8.size()));
        std::cout << '\n';
        auto loaded = LoadProblemData(directory);
        if (const auto* error = std::get_if<InputError>(&loaded)) {
            std::cerr << error->category << " [" << error->context << "] " << error->message << '\n';
            return 1;
        }
        const auto& data = std::get<ProblemData>(loaded); // loaded 在完整迭代结束前持有基础数据。
        const auto planning = RunPlanningIteration(data);
        // 控制台展示最后一轮结果；所有轮次的完整报告由协调模块保存。
        if (!planning.rounds.empty()) {
            const auto& last = planning.rounds.back();
            if (last.master) PrintMasterProblemReport(data, *last.master, std::cout);
            if (last.timetable && last.timetable_data)
                PrintTimetableReport(*last.timetable_data, *last.timetable, std::cout);
        }
        PrintPlanningReport(planning, std::cout);
        if (planning.status == PlanningStatus::Feasible) return 0;
        if (planning.status == PlanningStatus::ProvenInfeasible) return 3;
        if (planning.status == PlanningStatus::Error) return 1;
        return 4; // 未定或达到限制，不代表已证不可行。
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

/*
历史局部检查入口：完整保留旧参数解析和分阶段求解代码，当前不参与编译。
如需恢复：停用上方现行 wmain，取消本段注释，并恢复 master_problem.h/.cpp
中 SolveMasterProblem 的声明与定义，以及上方 FindDataDirectory。两个 wmain 不可同时启用。

/// @brief 默认执行完整迭代；--solve-timetable 保留单次第一层验证。
/// @details 显式报告参数用于单独查看某阶段数据，不改变默认的完整执行流程。
int wmain(int argc, wchar_t* argv[]) {
    const ScopedUtf8ConsoleOutput console_output;
    try {
        const std::wstring_view first_arg = argc > 1 ? argv[1] : L"";
        const bool report_input = first_arg == L"--report-input";
        const bool solve_master = first_arg == L"--solve-master";
        const bool report_timetable = first_arg == L"--report-timetable";
        const bool explicit_solve_timetable = first_arg == L"--solve-timetable";
        const bool has_mode_argument = report_input || solve_master || report_timetable || explicit_solve_timetable;
        const int directory_arg = has_mode_argument ? 2 : 1; // 有功能参数时，数据目录顺延一位。
        if (argc > directory_arg + 1 || (first_arg.starts_with(L"--") && !has_mode_argument)) {
            std::cerr << "用法：augmentation line plan.exe [Data目录]（默认执行加割迭代）\n"
                      << "      augmentation line plan.exe --report-input [Data目录]\n"
                      << "      augmentation line plan.exe --solve-master [Data目录]\n"
                      << "      augmentation line plan.exe --report-timetable [Data目录]\n"
                      << "      augmentation line plan.exe --solve-timetable [Data目录]\n";
            return 2;
        }
        const auto directory = argc > directory_arg ? std::filesystem::path(argv[directory_arg]) : FindDataDirectory(argv[0]);
        auto result = LoadProblemData(directory);
        if (const auto* error = std::get_if<InputError>(&result)) {
            std::cerr << error->category << " [" << error->context << "] " << error->message << '\n';
            return 1;
        }
        const auto& data = std::get<ProblemData>(result);
        if (report_input) {
            PrintInputReport(data, std::cout);
            return 0;
        }

        if (!has_mode_argument) {
            const auto planning = RunPlanningIteration(data);
            // 控制台保留最后取得的主问题解及可行时刻表；各轮完整数据另存报告。
            if (!planning.rounds.empty()) {
                const auto& last = planning.rounds.back();
                if (last.master) PrintMasterProblemReport(data, *last.master, std::cout);
                if (last.timetable && last.timetable_data)
                    PrintTimetableMilpReport(*last.timetable_data, *last.timetable, std::cout);
            }
            PrintPlanningReport(planning, std::cout);
            if (planning.status == PlanningStatus::Feasible) return 0;
            if (planning.status == PlanningStatus::ProvenInfeasible) return 3;
            if (planning.status == PlanningStatus::Error) return 1;
            return 4; // 未定、总时间或轮数限制：均没有不可行证明。
        }

        const auto master_result = SolveMasterProblem(data);
        if (const auto* error = std::get_if<InputError>(&master_result)) {
            std::cerr << error->category << " [" << error->context << "] " << error->message << '\n';
            return 1;
        }
        const auto& solved = std::get<MasterProblemResult>(master_result);
        PrintMasterProblemReport(data, solved, std::cout);
        // 取得可用解后，把主问题给出的方案直接交给时刻表数据构建。
        if (!solved.solution) return 1;
        const auto timetable = BuildTimetableData(data, *solved.solution);
        if (const auto* error = std::get_if<InputError>(&timetable)) {
            std::cerr << error->category << " [" << error->context << "] " << error->message << '\n';
            return 1;
        }
        const auto& timetable_data = std::get<TimetableData>(timetable);
        if (solve_master || report_timetable) {
            PrintTimetableDataReport(timetable_data, std::cout);
            return 0;
        }
        // 单次第一层验证；已证不可行或未定均只报告，不在这里生成割。
        const auto feasibility = SolveTimetableMilp(timetable_data,
            {data.parameters.timetable_time_limit_seconds, data.parameters.timetable_solver_logging});
        if (const auto* error = std::get_if<InputError>(&feasibility)) {
            std::cerr << error->category << " [" << error->context << "] " << error->message << '\n';
            return 1;
        }
        const auto& timetable_result = std::get<TimetableMilpResult>(feasibility);
        PrintTimetableMilpReport(timetable_data, timetable_result, std::cout);
        // 0=可行，3=已证不可行，4=未定；1 保留给加载/调用错误或主问题无可用解。
        if (timetable_result.status == TimetableStatus::ProvenInfeasible) return 3;
        if (timetable_result.status == TimetableStatus::Undetermined) return 4;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
*/
