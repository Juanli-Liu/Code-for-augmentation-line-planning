///////////////////////////////////////////////////////////////////////////
/// @file master_problem.h
/// @brief 增量开行方案主问题的求解与结果报告接口。
///////////////////////////////////////////////////////////////////////////
#ifndef MASTER_PROBLEM_H
#define MASTER_PROBLEM_H

#include "input_data.h"

#include <iosfwd>
#include <memory>
#include <optional>
#include <vector>

/// @brief 一个 OD 分配给单个候选方案的客流。
struct PassengerAllocation {
    int candidate_index; ///< [内部编号] 候选方案索引
    double passengers;   ///< [内部计算] 承运人数
};

/// @brief 一个 OD 的需求分配结果。
struct OdAllocation {
    int od_index;                              ///< [内部编号] OD 索引
    double unmet_demand;                       ///< [内部计算] 未满足人数
    std::vector<PassengerAllocation> services; ///< [内部计算] 有客流分配的候选方案
};

/// @brief 主问题取得的方案及客流解。
struct MasterSolution {
    double objective_value;                     ///< [内部计算] COPT 目标值
    double passenger_travel_cost;               ///< [内部计算] 总车内时间，单位为人·time_step
    double unmet_demand_cost;                   ///< [内部计算] 未满足需求时间惩罚，单位为人·time_step
    std::vector<int> selected_candidate_indices; ///< [内部编号] 被选中的候选方案索引
    std::vector<OdAllocation> od_allocations;    ///< [内部计算] 按 OD 内部编号排列
};

/// @brief 一次求解的状态和当前可用解。
struct MasterProblemResult {
    int solver_status;                       ///< [内部计算] COPT 原始状态码
    std::optional<MasterSolution> solution;    ///< [内部计算] 存在可用解时填充
};

/// @brief C 是已证不可行的候选内部编号集合，对应 sum(x_a,a in C)<=|C|-1。
using CandidateConflict = std::vector<int>;

/// @brief 跨轮次保留 COPT 环境、模型和变量；ProblemData 必须比本对象活得更久且保持不变。
/// @details 工厂只建立一次静态模型。AddConflictCut 追加约束，Solve 在原模型上重求。
class MasterProblemSession {
public:
    static InputResult<std::unique_ptr<MasterProblemSession>> Create(const ProblemData& data);
    ~MasterProblemSession();
    MasterProblemSession(const MasterProblemSession&) = delete;
    MasterProblemSession& operator=(const MasterProblemSession&) = delete;

    /// @param time_limit_seconds 本次重求的时限，必须是非负有限秒数。
    InputResult<MasterProblemResult> Solve(double time_limit_seconds);
    /// @return true=成功追加；false=已有同一集合；非法集合或求解器错误返回 InputError。
    /// @details 调用方负责提供不可行证明；不允许空集合，编号排序去重后入模。
    InputResult<bool> AddConflictCut(CandidateConflict conflict);

private:
    struct Impl;
    explicit MasterProblemSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_; ///< 隐藏 COPT 类型，使头文件不依赖求解器头文件。
};

/* 历史局部检查入口，暂时停用；需要时与 .cpp 中的定义一并取消注释。
/// @brief 构建并求解包含全部静态约束的开行方案主问题。
/// @param data 已由 LoadProblemData 构建的问题数据。
/// @return 求解状态及可用解，或模型构建和求解器调用的诊断信息。
InputResult<MasterProblemResult> SolveMasterProblem(const ProblemData& data);
*/

/// @brief 输出求解状态、候选选择、OD 客流分配和目标值分解。
/// @param data 与求解结果对应的问题数据，用于将索引转为业务 ID。
/// @param result 主问题求解结果。
/// @param output 报告输出流。
void PrintMasterProblemReport(const ProblemData& data,
                              const MasterProblemResult& result,
                              std::ostream& output);

#endif  // MASTER_PROBLEM_H
