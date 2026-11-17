///////////////////////////////////////////////////////////////////////////
/// @file master_problem.cpp
/// @brief COPT 主问题的变量、目标、静态约束和结果提取。
/// @details 按论文 model:mp 构建静态 MILP。约束名称中的数字使用 ProblemData
/// 的内部索引，报告中的 line_id 和 OD 编号使用 CSV 业务 ID。
///////////////////////////////////////////////////////////////////////////
#include "master_problem.h"

#include "coptcpp_pch.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr double kBinarySelected = 0.5;  // 读取 COPT 浮点解时判定二元变量是否选中。

/// @brief 保存解中需要读取的三组 COPT 变量句柄。
/// @details selected[a] 对应 x_a，unmet[od] 对应 u_od，
/// flow_by_candidate[a][od] 对应 phi_od^a。客流按候选组织，便于为同一候选逐段汇总载客量。
struct MasterVariables {
    std::vector<Var> selected;
    std::vector<std::map<int, Var>> flow_by_candidate;
    std::vector<Var> unmet;
};

/// @brief 建立候选选择、客流与未满足量，并按 eq:demand-balance 建立 OD 需求守恒。
/// @details 目标 eq:objective 的系数在 AddVar 的第三个参数中设置。
MasterVariables AddVariablesAndDemandBalance(Model& model, const ProblemData& data) {
    MasterVariables variables;
    variables.selected.reserve(data.candidates.size());
    variables.flow_by_candidate.resize(data.candidates.size());
    for (const auto& candidate : data.candidates) {
        const auto name = "x_" + std::to_string(candidate->index());
        variables.selected.push_back(model.AddVar(0.0, 1.0, 0.0, COPT_BINARY, name.c_str()));
    }

    // 对每个 OD，先把未满足量 u_od 放入守恒式，再加入所有可直达服务的客流。
    // 各项非负且总和等于该 OD 的需求，因此每个变量都可以用需求量作上界。
    variables.unmet.reserve(data.ods.size());
    for (const auto& od : data.ods) {
        const int od_index = od->index();
        const auto od_suffix = std::to_string(od_index);
        const auto unmet_name = "u_" + od_suffix;
        auto unmet = model.AddVar(0.0, od->demand(), od->unmet_unit_penalty(),
                                  COPT_CONTINUOUS, unmet_name.c_str());
        Expr demand_balance(unmet);
        variables.unmet.push_back(unmet);
        // od_services 只包含能顺向直达该 OD 的候选。
        for (const auto& service : data.od_services.at(od_index)) {
            const int candidate_index = service.candidate->index();
            const auto name = "phi_" + od_suffix + "_" + std::to_string(candidate_index);
            auto flow = model.AddVar(0.0, od->demand(), service.travel_time,
                                     COPT_CONTINUOUS, name.c_str());  // phi_od^a，分给候选 a 的人数。
            demand_balance += flow;
            variables.flow_by_candidate.at(candidate_index).emplace(od_index, flow);
        }
        model.AddConstr(demand_balance == od->demand(), ("demand_" + od_suffix).c_str());
    }
    model.SetObjSense(COPT_MINIMIZE);
    return variables;
}

/// @brief 按线路限制候选方案数，按有向物理区间限制新增车次。
void AddLineAndSectionConstraints(Model& model, const ProblemData& data,
                                  const MasterVariables& variables) {
    std::vector<Expr> line_choices(data.lines.size());  // 各线路的已选方案数，对应 eq:alternative。
    std::vector<Expr> section_counts(data.sections.size());  // 各有向物理区间的新增车次数 sum(f_a * x_a)。
    for (const auto& candidate : data.candidates) {
        const auto& selected = variables.selected.at(candidate->index());
        const auto& line = *candidate->line();
        line_choices.at(line.index()) += selected;
        // 候选经过的每个物理区间都计入其车次，包括通过站前后的区间。
        for (const auto* section : line.sections()) {
            section_counts.at(section->index()).AddTerm(selected, candidate->frequency());
        }
    }
    for (const auto& line : data.lines) {
        const auto name = "line_choice_" + std::to_string(line->index());
        model.AddConstr(line_choices.at(line->index()) <= 1.0, name.c_str());
    }
    // eq:section-limit 的右端使用区间总容量减去既有车次数。
    for (const auto& section : data.sections) {
        const auto name = "section_count_" + std::to_string(section->index());
        model.AddConstr(section_counts.at(section->index()) <= section->remaining_train_capacity(), name.c_str());
    }
}

/// @brief 按候选和相邻停车站区段建立座位容量约束 eq:seat-capacity。
void AddSeatConstraints(Model& model, const ProblemData& data,
                        const MasterVariables& variables) {
    for (const auto& candidate : data.candidates) {
        const int index = candidate->index();  // 候选内部索引，用于查找 x_a 和 phi_od^a。
        const auto& coverage = candidate->line()->od_indices_by_stop_section();  // 各停车区段覆盖的 OD 索引。
        const auto& flows = variables.flow_by_candidate.at(index);  // 当前候选的 phi_od^a，以 OD 索引为键。
        const double seats = static_cast<double>(candidate->frequency())
                             * candidate->rolling_stock()->seat_capacity();  // 选中时每段可用座位数 f_a * Q_r。
        for (std::size_t section = 0; section < coverage.size(); ++section) {  // 线路内的停车区段序号。
            Expr load;  // 当前区段的载客量表达式，每段从零开始。
            // 例如停车序列 A、C、E 中，A→E 的同一个变量会计入 A–C 和 C–E。
            for (const int od_index : coverage[section]) {
                load += flows.at(od_index);
            }
            // 写成区段客流之和 - f_a * Q_r * x_a <= 0。
            // x_a 为 0 时可用座位为零，结合客流非负性可知这些客流也必须为零。
            load.AddTerm(variables.selected.at(index), -seats);
            const auto name = "seat_" + std::to_string(index) + "_" + std::to_string(section);
            model.AddConstr(load <= 0.0, name.c_str());
        }
    }
}

/// @brief 检查每个经由站的聚合轨道分配和每条轨道的总占用时间。
void AddPlatformConstraints(Model& model, const ProblemData& data,
                            const MasterVariables& variables) {
    std::vector<Expr> occupation(data.platforms.size());  // 各轨道的总新增占用，单位为 time_step。
    for (const auto& candidate : data.candidates) {
        const int index = candidate->index();
        const auto& choices = candidate->line()->allowed_platforms();  // 按经由站位置索引的可用轨道列表。
        const auto& times = candidate->minimum_platform_occupation_times();  // 每站单趟最小占轨，单位为 time_step。
        for (std::size_t position = 0; position < choices.size(); ++position) {
            Expr station_count;  // 当前候选在当前站分给各轨道的车次数之和。
            const auto suffix = std::to_string(index) + "_" + std::to_string(position);
            for (const auto* platform : choices[position]) {
                const auto name = "zeta_" + suffix + "_" + std::to_string(platform->index());
                const auto count = model.AddVar(0.0, candidate->frequency(), 0.0,
                                                COPT_INTEGER, name.c_str());  // 分给该轨道的整数车次数 zeta_a,s,g。
                station_count += count;
                // 给该轨道累加占用时间，系数 times[position] 把车次数换成占用时间。
                occupation.at(platform->index()).AddTerm(count, times[position]);
            }
            // eq:platform-assignment 写成各轨道分配车次数之和 - f_a * x_a = 0。
            // 例如频率为 2 时，选中方案就在这个站共分配 2 趟，未选中时共分配 0 趟。
            station_count.AddTerm(variables.selected.at(index), -static_cast<double>(candidate->frequency()));
            model.AddConstr(station_count == 0.0, ("platform_assignment_" + suffix).c_str());
        }
    }
    // 完成所有候选的占用累加后，eq:platform-time 为每条轨道限制一次总新增占用。
    // remaining_available_time() 返回轨道总资源时间减去既有占用。
    for (const auto& platform : data.platforms) {
        const auto name = "platform_time_" + std::to_string(platform->index());
        model.AddConstr(occupation.at(platform->index()) <= platform->remaining_available_time(), name.c_str());
    }
}

/// @brief 按车型检查任务最短在途时间与必要折返时间能否容纳在备用车组的总可用时间内。
void AddRollingStockConstraints(Model& model, const ProblemData& data,
                                const MasterVariables& variables) {
    std::vector<Expr> task_counts(data.rolling_stocks.size());  // 各车型的新增任务数 sum(f_a * x_a)。
    std::vector<Expr> trip_workload(data.rolling_stocks.size());  // 各车型的最短在途工作量，单位为 time_step。
    for (const auto& candidate : data.candidates) {
        const int stock_index = candidate->rolling_stock()->index();
        const double frequency = candidate->frequency();
        const auto& selected = variables.selected.at(candidate->index());
        task_counts.at(stock_index).AddTerm(selected, frequency);
        // 一趟的 T_a 从实际始发计到实际终到，含中途停站。候选的贡献是 f_a * T_a * x_a。
        trip_workload.at(stock_index).AddTerm(selected, frequency * candidate->minimum_trip_time());
    }
    for (const auto& stock : data.rolling_stocks) {
        const int index = stock->index();
        const auto suffix = std::to_string(index);
        const double available_units = stock->available_units();  // 当前车型的备用组数。
        const auto connections = model.AddVar(0.0, COPT_INFINITY, 0.0, COPT_CONTINUOUS,
                                              ("chi_" + suffix).c_str());  // 用于计入必要接续次数的辅助变量 chi_r。
        // eq:minimum-connections
        model.AddConstr(connections >= task_counts.at(index) - available_units,
                        ("connections_" + suffix).c_str());
        // eq:rolling-stock-workload
        model.AddConstr(trip_workload.at(index) + stock->minimum_turnaround_time() * connections
                            <= available_units * data.parameters.planning_horizon,
                        ("workload_" + suffix).c_str());
    }
}

/// @brief 将 COPT 解转成候选索引和按 OD 汇总的客流分配。
MasterSolution ExtractSolution(Model& model, const ProblemData& data,
                               MasterVariables& variables) {
    MasterSolution solution{};
    solution.objective_value = model.GetDblAttr(COPT_DBLATTR_OBJVAL);
    for (const auto& candidate : data.candidates) {
        const int index = candidate->index();
        if (variables.selected.at(index).Get(COPT_DBLINFO_VALUE) > kBinarySelected) {
            solution.selected_candidate_indices.push_back(index);
        }
    }
    // 用建模时的系数分别计算目标的两项，objective_value 保留求解器报告的总目标。
    solution.od_allocations.reserve(data.ods.size());
    for (const auto& od : data.ods) {
        const int od_index = od->index();
        OdAllocation allocation{od_index, variables.unmet.at(od_index).Get(COPT_DBLINFO_VALUE), {}};
        solution.unmet_demand_cost += allocation.unmet_demand * od->unmet_unit_penalty();
        for (const auto& service : data.od_services.at(od_index)) {
            const int index = service.candidate->index();
            const double flow = variables.flow_by_candidate.at(index).at(od_index).Get(COPT_DBLINFO_VALUE);
            solution.passenger_travel_cost += flow * service.travel_time;
            if (flow > 0.0) {
                allocation.services.push_back({index, flow});
            }
        }
        solution.od_allocations.push_back(std::move(allocation));
    }
    return solution;
}

const char* StatusDescription(const MasterProblemResult& result) {
    switch (result.solver_status) {
        case COPT_STATUS_OPTIMAL: return "最优";
        case COPT_STATUS_INFEASIBLE: return "已证不可行";
        case COPT_STATUS_UNBOUNDED: return "无界";
        case COPT_STATUS_INF_OR_UNB: return "不可行或无界";
        case COPT_STATUS_TIMEOUT: return result.solution ? "达到时限，已有可行解" : "达到时限，未取得可行解";
        case COPT_STATUS_NUMERICAL: return "数值异常";
        case COPT_STATUS_IMPRECISE: return "求解精度不足";
        default: return result.solution ? "有可行解，未证最优" : "未取得可行解";
    }
}

}  // namespace


//MasterProblemSession 是长期保留的“主问题对象”；
// Impl装着它真正使用的 COPT 模型和变量。外层迭代一直使用这一个对象，只往模型里加约束
struct MasterProblemSession::Impl {
    // 声明顺序保证析构时先释放变量/模型，最后释放环境；data 是非拥有引用。
    const ProblemData& data;
    Envr environment;
    Model model;
    MasterVariables variables;
    std::set<CandidateConflict> conflicts; // 已成功追加的规范化集合，用于防止重复加割。

    explicit Impl(const ProblemData& problem)
        : data(problem), model(environment.CreateModel("augmentation_master_problem")),
          variables(AddVariablesAndDemandBalance(model, data)) {
        AddLineAndSectionConstraints(model, data, variables);
        AddSeatConstraints(model, data, variables);
        AddPlatformConstraints(model, data, variables);
        AddRollingStockConstraints(model, data, variables);
    }
};

MasterProblemSession::MasterProblemSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MasterProblemSession::~MasterProblemSession() = default;

InputResult<std::unique_ptr<MasterProblemSession>> MasterProblemSession::Create(const ProblemData& data) {
    try {
        return std::unique_ptr<MasterProblemSession>(new MasterProblemSession(std::make_unique<Impl>(data)));
    } catch (const std::exception& error) {
        return InputError{"主问题构建错误", "MasterProblemSession::Create", error.what()};
    }
}

InputResult<bool> MasterProblemSession::AddConflictCut(CandidateConflict conflict) {
    try {
        std::sort(conflict.begin(), conflict.end());
        conflict.erase(std::unique(conflict.begin(), conflict.end()), conflict.end());
        if (conflict.empty()) throw std::invalid_argument("不允许添加空候选冲突割。");
        for (int index : conflict)
            if (index < 0 || static_cast<std::size_t>(index) >= impl_->variables.selected.size())
                throw std::invalid_argument("冲突割候选编号越界：" + std::to_string(index));
        if (impl_->conflicts.contains(conflict)) return false;
        Expr selected_count; // 本冲突集合内选中方案数 sum_{a in C} x_a。
        for (int index : conflict) selected_count += impl_->variables.selected.at(index);
        const auto name = "conflict_" + std::to_string(impl_->conflicts.size());
        impl_->model.AddConstr(selected_count <= static_cast<double>(conflict.size() - 1), name.c_str());
        impl_->conflicts.insert(std::move(conflict));
        return true;
    } catch (const std::exception& error) {
        return InputError{"冲突割错误", "MasterProblemSession::AddConflictCut", error.what()};
    }
}

InputResult<MasterProblemResult> MasterProblemSession::Solve(double time_limit_seconds) {
    try {
        if (!std::isfinite(time_limit_seconds) || time_limit_seconds < 0)
            throw std::invalid_argument("主问题时限必须为非负有限秒数。");
        auto& model = impl_->model; // 原模型的引用；不复制、不重建、不 Reset。
        // 每轮求解均按 CSV 控制主问题日志；不影响第一层日志和程序自身的结果报告。
        model.SetIntParam(COPT_INTPARAM_LOGGING, impl_->data.parameters.master_solver_logging ? 1 : 0);
        model.SetDblParam(COPT_DBLPARAM_TIMELIMIT, time_limit_seconds);
        model.Solve();
        // 先保留终止状态，再单独检查是否取得解。达到时限时也可能有可读的可行解。
        MasterProblemResult result{model.GetIntAttr(COPT_INTATTR_STATUS), std::nullopt};
        if (model.GetIntAttr(COPT_INTATTR_HASSOL) != 0) {
            result.solution = ExtractSolution(model, impl_->data, impl_->variables);
        }
        return result;
    } catch (const CoptException& error) {
        return InputError{"求解器错误", "SolveMasterProblem",
                          "COPT 错误码 " + std::to_string(error.GetCode()) + "，" + error.what()};
    } catch (const std::exception& error) {
        return InputError{"主问题错误", "SolveMasterProblem", error.what()};
    }
}

/* 历史局部检查入口，暂时停用；需要时与 .h 中的声明一并取消注释。
InputResult<MasterProblemResult> SolveMasterProblem(const ProblemData& data) {
    auto session = MasterProblemSession::Create(data);
    if (const auto* error = std::get_if<InputError>(&session)) return *error;
    return std::get<std::unique_ptr<MasterProblemSession>>(session)->Solve(data.parameters.master_time_limit_seconds);
}
*/

void PrintMasterProblemReport(const ProblemData& data,
                              const MasterProblemResult& result,
                              std::ostream& output) {
    output << "主问题状态=" << StatusDescription(result) << "，COPT 状态码=" << result.solver_status << '\n';
    if (!result.solution) return;

    const auto& solution = *result.solution;
    const auto previous_precision = output.precision(12);
    output << "目标值=" << solution.objective_value << "，旅客总车内时间=" << solution.passenger_travel_cost
           << "，未满足需求惩罚=" << solution.unmet_demand_cost << "（人·time_step）\n";

    std::vector<int> line_ids(data.lines.size());  // 按线路内部索引查找 CSV 线路业务 ID。
    for (const auto& [id, line] : data.line_by_id) {
        line_ids.at(line->index()) = id;
    }
    output << "选中候选数=" << solution.selected_candidate_indices.size() << '\n';
    for (const int index : solution.selected_candidate_indices) {
        const auto& candidate = *data.candidates.at(index);
        output << "选中候选 " << index << " line_id=" << line_ids.at(candidate.line()->index())
               << " frequency=" << candidate.frequency()
               << " formation=" << data.rolling_stock_names.at(candidate.rolling_stock()->index()) << '\n';
    }
    for (const auto& [id, od] : data.od_by_id) {
        const auto& allocation = solution.od_allocations.at(od->index());
        output << "OD " << id << ' ' << od->origin_name() << "->" << od->destination_name()
               << " 需求=" << od->demand() << " 未满足=" << allocation.unmet_demand;
        for (const auto& service : allocation.services) {
            output << " 候选" << service.candidate_index << '=' << service.passengers;
        }
        output << '\n';
    }
    output.precision(previous_precision);
}
