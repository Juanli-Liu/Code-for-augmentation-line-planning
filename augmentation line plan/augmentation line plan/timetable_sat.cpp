///////////////////////////////////////////////////////////////////////////
/// @file timetable_sat.cpp
/// @brief 将整数事件时间差转为顺序编码 CNF；与 MILP 共用数据但独立建模。
///////////////////////////////////////////////////////////////////////////
#include "timetable_sat.h"
#include "cadical.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <initializer_list>
#include <map>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

/// <summary>
/// SolveTimetableSat 管流程，
/// `Encode` 管建模顺序，各类 Encode… 表达业务约束，
/// `AddMinimumDifference` 和 AddClause 完成底层转换，
/// `Decode` 将解还原为时刻表
/// </summary>
namespace {
using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

/// @brief 编码预算耗尽只表示未定，不属于输入错误或不可行证明。
struct EncodingTimeout {};

/// @brief CaDiCaL 在搜索过程中定期调用；编码阶段也检查同一个起点。
class TimeBudget final : public CaDiCaL::Terminator {
public:
    explicit TimeBudget(double limit) : limit_(limit) {}
    bool terminate() override { return SecondsSince(start_) >= limit_; }
    void CheckEncoding() {
        if (terminate()) throw EncodingTimeout{};
    }
private:
    Clock::time_point start_ = Clock::now(); ///< 整次 SAT 调用的预算起点。
    double limit_;                        ///< 墙钟秒，不是运行图时间步。
};

/// @brief 维护变量映射和子句生成；文字正负号分别表示变量及其否定。
class TimetableEncoder {
public:
    TimetableEncoder(const TimetableData& data, CaDiCaL::Solver& solver, TimeBudget& budget, bool assumptions)
        : data_(data), solver_(solver), budget_(budget), use_assumptions_(assumptions) {
        // 1 专用作真常量，-1 作假常量；真正业务变量从 2 开始。
        solver_.add(1);
        solver_.add(0);
    }

    void Encode() {
        ValidateInput();
        AllocateVariables();
        EncodeEvents();
        EncodeActivities();
        EncodeHeadwaysAndTracks();
        EncodeSectionOrders();
    }

    std::size_t variable_count() const { return static_cast<std::size_t>(last_variable_); }
    std::size_t clause_count() const { return clause_count_; }

    /// @brief 全部候选均必须开行；通过 assumptions 而非单位子句启用。
    void AssumeAllCandidates() {
        for (const auto& [candidate, mapping] : candidate_variables_) solver_.assume(mapping.literal);
    }

    std::vector<CandidateAssumption> CandidateMappings() const {
        std::vector<CandidateAssumption> mappings;
        for (const auto& [candidate, mapping] : candidate_variables_) mappings.push_back(mapping);
        return mappings;
    }

    /// @brief 仅在返回20后查询本次正候选 assumptions，不查询时间/轨道文字。
    std::vector<int> ExtractCandidateCore() {
        std::vector<int> core;
        for (const auto& [candidate, mapping] : candidate_variables_) {
            budget_.CheckEncoding(); // 预算耗尽时放弃提核，仍保留原始 UNSAT。
            if (solver_.failed(mapping.literal)) // 查询该激活文字是否属于本次不可满足核
                core.push_back(candidate);// 若属于，将对应候选编号加入核集合
        }
        return core;
    }

    /// @brief SAT 求解成功后，将布尔变量的真值还原为时刻、轨道和列车次序。
    TimetableSolution Decode() {
        TimetableSolution solution; // 保存还原后的时刻表结果，不保存 SAT 变量编号。
        // ① 还原事件时刻：q[e,k] 表示时刻 <= k，第一个为真的 k 就是事件时刻。
        for (std::size_t e = 0; e < data_.events.size(); ++e) {
            int event_time = data_.planning_horizon; // 所有 q[e,k] 为假时，事件恰在 H。
            bool reached_true = false;              // 是否已遇到真值；之后不允许再出现假值。
            for (int k = 0; k < data_.planning_horizon; ++k) {
                const bool value = ReadBoolean(time_variables_[e][k]); // 读取当前时间变量的真值。
                if (reached_true && !value) throw std::runtime_error("SAT 时间文字不单调。");
                if (value && !reached_true) event_time = k;
                reached_true = reached_true || value;
            }
            solution.event_times.push_back(event_time);
        }
        // ② 还原每次访问的轨道：choices 是该访问的“轨道编号 -> SAT 变量编号”映射。
        for (const auto& choices : track_variables_) {
            std::optional<int> selected; // 当前访问的原轨道编号，必须恰好一个。
            for (const auto& [track, variable] : choices) {
                // 选择变量为假则跳过；为真则记录轨道，并检查是否重复选择。
                if (!ReadBoolean(variable)) continue;
                if (selected) throw std::runtime_error("SAT 一次访问选中了多条轨道。");
                selected = track;
            }
            if (!selected) throw std::runtime_error("SAT 一次访问没有选中轨道。");
            solution.visit_platforms.push_back(*selected);
        }
        // ③ 还原冲突对 c 的到达、出发次序：真表示 first 在前，假表示 second 在前。
        for (std::size_t c = 0; c < arrival_orders_.size(); ++c)
            solution.orders.push_back({ReadBoolean(arrival_orders_[c]), ReadBoolean(departure_orders_[c])});
        // 返回后，由调用方独立检查整个时刻表是否满足约束。
        return solution;
    }

private:
    void ValidateInput() {
        if (data_.planning_horizon < 0 || data_.SecondsPerStep() <= 0 || data_.track_headway < 0)
            throw std::invalid_argument("时域、时间步或同轨间隔无效。");
        for (const auto& event : data_.events) {
            budget_.CheckEncoding();
            const auto& domain = event.time_domain; // 数据层已区分实际到发和端点占轨。
            if (domain.lower < 0 || domain.upper < domain.lower || domain.upper > data_.planning_horizon)
                throw std::invalid_argument("事件时间域无效。");
        }
        for (const auto& activity : data_.activities) {
            if (activity.duration < 0) throw std::invalid_argument("活动时长不得为负。");
            (void)data_.events.at(activity.from_event_index);
            (void)data_.events.at(activity.to_event_index);
        }
        for (const auto& visit : data_.visits) {
            (void)data_.events.at(visit.arrival_side_event_index);
            (void)data_.events.at(visit.departure_side_event_index);
            if (visit.fixed_platform_index && std::find(visit.allowed_platform_indices.begin(),
                visit.allowed_platform_indices.end(), *visit.fixed_platform_index) == visit.allowed_platform_indices.end())
                throw std::invalid_argument("固定轨道不在允许集合中。");
        }
        for (const auto& conflict : data_.station_conflicts)
            if (conflict.arrival_headway < 0 || conflict.departure_headway < 0)
                throw std::invalid_argument("到发间隔不得为负。");
    }

    int NewVariable() {
        budget_.CheckEncoding();
        if (last_variable_ == std::numeric_limits<int>::max() - 1)
            throw std::overflow_error("SAT 变量编号超过整数范围。");
        return ++last_variable_;
    }

    /// @brief 为时间顺序、到达次序、出发次序和轨道选择四类布尔变量分配唯一编号。
    /// @details 容器中保存的是 SAT 变量编号，不是时刻或真假值；NewVariable() 从 2 开始编号，
    /// 编号 1 已保留为真常量。本函数只建立变量映射，具体约束由后续 Encode... 函数添加。
    void AllocateVariables() {
        // 候选编号与SAT变量号分开保存，并保留候选所属的全部车次。
        if (use_assumptions_) {
            //给每个候选分配激活文字
            for (int candidate : data_.selected_candidate_indices) {
                if (candidate < 0 || candidate_variables_.contains(candidate))
                    throw std::invalid_argument("候选 assumptions 编号重复或为负。");
                candidate_variables_.emplace(candidate, CandidateAssumption{candidate, NewVariable(), {}});
            }
            for (const auto& [train_index, train] : data_.trains) {
                //把具体列车登记到所属候选下
                if (train.candidate_index)
                    candidate_variables_.at(*train.candidate_index).train_indices.push_back(train_index);
				//检查列车的访问映射是否一致
                for (auto visit : train.visit_indices)
                    if (data_.visits.at(visit).train_index != train_index)
                        throw std::invalid_argument("候选—列车—访问映射不一致。");
            }
            //保证每个选中候选都有具体列车
            for (const auto& [candidate, mapping] : candidate_variables_)
                if (mapping.train_indices.empty()) throw std::invalid_argument("选中候选没有所属列车。");
        }
        // ① 时间顺序变量 q[e,k]：为每个事件 e、每个 k=0,...,H-1 分配一个布尔变量。
        // time_variables_[e][k] 保存变量编号；k<0 和 k>=H 由 TimeLiteral 返回常量，无需分配。
        time_variables_.resize(data_.events.size());
        for (auto& times : time_variables_) {
            // times 是当前事件对应的一整行时间文字编号，按时间阈值 k 递增排列。
            for (int k = 0; k < data_.planning_horizon; ++k) times.push_back(NewVariable());
        }

        // ②③ 次序变量：每条同站同向冲突记录 c 分别分配一个到达次序和一个出发次序变量。
        // arrival_orders_[c] 为真表示 first_visit 的到达侧事件先于或同时于 second_visit，
        // departure_orders_[c] 为真表示 first_visit 的出发侧事件先于或同时于 second_visit；
        // 为假时顺序相反。是否必须严格间隔由后续安全间隔约束决定，零间隔时允许同刻。
        // 到达侧也可能是始发开始占轨，出发侧也可能是终到释放轨道，均沿用数据层的事件定义。
        for (std::size_t c = 0; c < data_.station_conflicts.size(); ++c) {
            arrival_orders_.push_back(NewVariable());
            departure_orders_.push_back(NewVariable());
        }

        // ④ 轨道选择变量 z[v,g]：为每次车站访问 v 的每条允许轨道 g 分配一个布尔变量。
        // 为真表示该次访问使用轨道 g；v 对应“某趟车在某站的一次访问”，不是车站编号。
        // track_variables_[v][g] 保存变量编号；不同访问即使使用同一轨道，也有各自的选择变量。
        // 此处只分配允许轨道；恰好选择一条及既有固定轨道约束由 EncodeHeadwaysAndTracks 添加。
        track_variables_.resize(data_.visits.size());
        for (std::size_t v = 0; v < data_.visits.size(); ++v) {
            auto& choices = track_variables_[v]; // 原轨道编号 -> 此访问的轨道选择文字。
            for (int track : data_.visits[v].allowed_platform_indices) {
                if (!choices.emplace(track, NewVariable()).second)
                    throw std::invalid_argument("允许轨道集合包含重复编号。");
            }
        }
        // 将以上四类业务变量一次性声明给 CaDiCaL，数量为 last_variable_-1（不含已有的真常量）。
        // 即便变量因常量化简没有出现在任何子句里，解码时也允许查询它。
        // 3.x 的显式声明还保护这些编号，避免内部扩展变量占用未出现在子句中的编号。
        if (last_variable_ > 1) solver_.declare_more_variables(last_variable_ - 1);
    }

    /// @brief q[e,k] 表示 tau[e]<=k；越界直接返回常量，不创建越界变量。
    int TimeLiteral(std::size_t event, std::int64_t k) const {
        const auto& times = time_variables_.at(event); // 先检查事件编号，即使 k 越界。
        if (k < 0) return -1;
        if (k >= data_.planning_horizon) return 1;
        return times.at(static_cast<std::size_t>(k));
    }

    /// @brief 化简真假常量；空子句原样提交，表示真实的逻辑矛盾。
    void AddClause(const std::vector<int>& literals) {
        budget_.CheckEncoding();
        if (std::find(literals.begin(), literals.end(), 1) != literals.end()) return;
        // 所有新增参与者均开行时约束才生效；既有背景没有候选前缀。
        for (int candidate : clause_candidates_) {
            auto& mapping = candidate_variables_.at(candidate);
            solver_.add(-mapping.literal);
            ++mapping.guarded_clause_count;
        }
        for (int literal : literals) {
            if (literal == 0) throw std::logic_error("业务文字不能使用子句终止符 0。");
            if (literal != -1) solver_.add(literal);
        }
        solver_.add(0);
        ++clause_count_;
    }

    /// @brief 每段约束显式重设参与者；同候选多车只加入一个候选条件。
    void UseParticipants(std::initializer_list<std::size_t> visits) {
        clause_candidates_.clear();
        if (!use_assumptions_) return; // 完整集合模式保持原有编码。
        for (auto visit : visits) {
            const auto& record = data_.visits.at(visit);
            const auto& train = data_.trains.at(record.train_index);
            if (std::find(train.visit_indices.begin(), train.visit_indices.end(), visit) == train.visit_indices.end())
                throw std::invalid_argument("访问未登记在所属列车中。");
            if (train.candidate_index) {
                (void)candidate_variables_.at(*train.candidate_index);
                clause_candidates_.push_back(*train.candidate_index);
            }
        }
        std::sort(clause_candidates_.begin(), clause_candidates_.end());
        clause_candidates_.erase(std::unique(clause_candidates_.begin(), clause_candidates_.end()), clause_candidates_.end());
    }

    /// @brief 为每个事件添加时间顺序文字的单调性约束，以及允许时间域的上下界约束。
    void EncodeEvents() {
        // e 是 data_.events 中的事件下标，对应 time_variables_ 的一行。
        for (std::size_t e = 0; e < data_.events.size(); ++e) {
            UseParticipants({data_.events[e].visit_index});
            // ① 时间单调性：q[e,k] -> q[e,k+1]，即“若事件不晚于 k，也一定不晚于 k+1”。
            // 蕴含转换为子句：!q[e,k] OR q[e,k+1]；负号表示否定该文字。
            // k 是时间阈值；这些子句禁止时间文字出现“先真后假”，但允许一直为假（时刻为 H）。
            for (int k = 0; k < data_.planning_horizon; ++k)
                AddClause({-TimeLiteral(e, k), TimeLiteral(e, static_cast<std::int64_t>(k) + 1)});

            // ② 时间域：domain 是当前事件允许的闭区间 [L,U]，由时刻表数据层预先构建。
            // 既有实际到发使用公布时刻及允许后移窗口；端点占轨事件使用其独立时间域。
            const auto& domain = data_.events[e].time_domain;
            // 下界单位子句 !q[e,L-1]：事件不能不晚于 L-1，因此整数时刻必须 >=L。
            AddClause({-TimeLiteral(e, static_cast<std::int64_t>(domain.lower) - 1)});
            // 上界单位子句 q[e,U]：事件必须不晚于 U，因此时刻必须 <=U。
            AddClause({TimeLiteral(e, domain.upper)});
            // 例如 [L,U]=[2,4]：强制 q[e,1] 为假、q[e,4] 为真，事件只能发生在 2、3、4。
            // 边界由 TimeLiteral 统一处理：q[e,-1]=假，q[e,H]=真。
            // 因而 L=0、U=H 及单调性循环的最后一个子句可由 AddClause 自动化简。
        }
    }

    /// @brief 激活条件全部为真时要求 tau[after]-tau[before]>=gap。
    /// @details 对 k=0..H 编码 q[after,k] -> q[before,k-gap]，必须包含 k=H。
    /// gap 可以为负；等式的反向下界需要 -duration，偏移计算使用 64 位。
    void AddMinimumDifference(std::size_t before, std::size_t after, std::int64_t gap,
        const std::vector<int>& activation = {}) {
        std::vector<int> clause; // 条件文字的否定前缀，在整个时间循环内保持不变。
        for (int literal : activation) clause.push_back(-literal);
        const auto prefix_size = clause.size(); // 每个时间位置仅替换最后两个时间文字。
        for (std::int64_t k = 0; k <= data_.planning_horizon; ++k) {
            clause.resize(prefix_size);
            clause.push_back(-TimeLiteral(after, k));
            clause.push_back(TimeLiteral(before, k - gap));
            AddClause(clause);
        }
    }

    void EncodeActivities() {
        for (const auto& activity : data_.activities) {
            UseParticipants({data_.events.at(activity.from_event_index).visit_index,
                             data_.events.at(activity.to_event_index).visit_index});
            //规定时长下限
            AddMinimumDifference(activity.from_event_index, activity.to_event_index, activity.duration);
            //针对固定时长，反向调用补上上限
            if (activity.has_fixed_duration())
                AddMinimumDifference(activity.to_event_index, activity.from_event_index,
                    -static_cast<std::int64_t>(activity.duration));
        }
    }

    /// @brief 编码每次车站访问的轨道选择，以及冲突列车对的到发间隔和同轨占用间隔。
    /// @details AddMinimumDifference(before, after, gap, activation) 仅在 activation
    /// 中所有文字为真时，要求 t[after]-t[before]>=gap；所有间隔均以模型时间步为单位。
    void EncodeHeadwaysAndTracks() {
        // ① 每次车站访问恰好选择一条允许轨道；v 是 data_.visits 中的访问下标。
        for (std::size_t v = 0; v < data_.visits.size(); ++v) {
            UseParticipants({v}); // 轨道唯一分配和固定轨道均随该候选启停。
            std::vector<int> choices; // 本次访问所有可选轨道的正文字。
            // track 是轨道编号，variable 是“本次访问选择该轨道”的布尔变量编号。
            for (const auto& [track, variable] : track_variables_[v]) choices.push_back(variable);
            // 所有选择文字取 OR：至少一个为真；空子句表示没有轨道可选，模型不可行。
            AddClause(choices); // 至少一条；允许集合为空时提交空子句。
            // i、j 枚举两项不同的轨道选择；!choices[i] OR !choices[j] 禁止同时选择。
            // 两两互斥保证“至多一条”，与上一条子句合起来就是“恰好一条”。
            for (std::size_t i = 0; i < choices.size(); ++i)
                for (std::size_t j = i + 1; j < choices.size(); ++j) AddClause({-choices[i], -choices[j]});
            // fixed 是可选的固定轨道编号；存在时，用单位子句强制选择该轨道。
            if (const auto fixed = data_.visits[v].fixed_platform_index)
                AddClause({track_variables_[v].at(*fixed)});
        }

        // ② 逐对处理同站同向的冲突访问；c 同时索引该冲突对的到达、出发次序变量。
        for (std::size_t c = 0; c < data_.station_conflicts.size(); ++c) {
            const auto& conflict = data_.station_conflicts[c]; // 仅使用数据层生成的同站同向对。
            UseParticipants({conflict.first_visit_index, conflict.second_visit_index});
            // first、second 是冲突对中的两次车站访问，名称不代表已经确定的时间先后。
            const auto& first = data_.visits.at(conflict.first_visit_index);
            const auto& second = data_.visits.at(conflict.second_visit_index);
            const int arrival_order = arrival_orders_[c];     // 到达次序变量编号：真表示 first 到达侧在前。
            const int departure_order = departure_orders_[c]; // 出发次序变量编号：真表示 first 出发侧在前。
            // 上述 int 存储变量编号，不是求解后的真值；负号表示相反次序的激活文字。
            // 到达侧沿用实际到达或始发开始占轨事件；出发侧沿用实际出发或终到释放事件。
            // 事件及有效间隔由数据层确定；间隔为 0 时，“在前”允许两事件同时发生。

            // ③ 到达侧安全间隔：arrival_order 为真时 second 在 first 之后，
            // 为假时 first 在 second 之后；两种次序均要求至少 arrival_headway 的间隔。
            AddMinimumDifference(first.arrival_side_event_index, second.arrival_side_event_index,
                conflict.arrival_headway, {arrival_order});
            AddMinimumDifference(second.arrival_side_event_index, first.arrival_side_event_index,
                conflict.arrival_headway, {-arrival_order});

            // ④ 出发侧安全间隔：同样按 departure_order 的真假，分别约束两个方向。
            // 到达、出发次序分别编码；区间禁越行关系由 EncodeSectionOrders() 另行连接。
            AddMinimumDifference(first.departure_side_event_index, second.departure_side_event_index,
                conflict.departure_headway, {departure_order});
            AddMinimumDifference(second.departure_side_event_index, first.departure_side_event_index,
                conflict.departure_headway, {-departure_order});

            // ⑤ 同轨占用间隔：track 枚举双方都允许使用的轨道，不表示双方已经选中了它。
            // 只有两车实际选择同一条轨道时，才要求前车释放后，后车才能开始占用。
            // 上面的到发安全间隔不以同轨为前提；共同轨道集合为空时仅跳过本段。
            for (int track : conflict.common_platform_indices) {
                // 两个变量分别表示 first、second 是否选择当前 track。
                const int first_track = track_variables_.at(conflict.first_visit_index).at(track);
                const int second_track = track_variables_.at(conflict.second_visit_index).at(track);
                // eq:sat-encode-track-compatibility：沿用到达次序，不另设占轨次序。
                // 两车同轨且 first 到达侧在前：second 开始占轨 - first 释放轨道 >= track_headway。
                // 花括号中三个激活文字必须全部为真，此时间差约束才生效。
                AddMinimumDifference(first.departure_side_event_index, second.arrival_side_event_index,
                    data_.track_headway, {first_track, second_track, arrival_order});
                // 两车同轨且 second 到达侧在前：first 开始占轨 - second 释放轨道 >= track_headway。
                AddMinimumDifference(second.departure_side_event_index, first.arrival_side_event_index,
                    data_.track_headway, {first_track, second_track, -arrival_order});
            }
        }
    }

    void EncodeSectionOrders() {
        for (const auto& section : data_.section_orders) {
            const auto& entry_conflict = data_.station_conflicts.at(section.entry_departure_conflict_index);
            const auto& exit_conflict = data_.station_conflicts.at(section.exit_arrival_conflict_index);
            UseParticipants({entry_conflict.first_visit_index, entry_conflict.second_visit_index,
                             exit_conflict.first_visit_index, exit_conflict.second_visit_index});
            const int entry = departure_orders_.at(section.entry_departure_conflict_index);
            const int exit = arrival_orders_.at(section.exit_arrival_conflict_index);
            AddClause({-entry, exit}); // 同一列车对：入口先后次序与出口相同。
            AddClause({entry, -exit});
        }
    }

    bool ReadBoolean(int variable) {
        const int value = solver_.val(variable); // CaDiCaL 返回带符号的变量号。
        if (value != variable && value != -variable) throw std::runtime_error("SAT 解中的变量未赋值。");
        return value > 0;
    }

    const TimetableData& data_;
    CaDiCaL::Solver& solver_;
    TimeBudget& budget_;
    bool use_assumptions_;
    std::map<int, CandidateAssumption> candidate_variables_;
    std::vector<int> clause_candidates_; ///< 当前业务约束的候选参与者，每段编码前重设。
    int last_variable_ = 1;       ///< 真常量占用编号 1，业务变量从 2 开始。
    std::size_t clause_count_ = 1; ///< 包括初始化时固定真常量的单位子句。
    std::vector<std::vector<int>> time_variables_; ///< events 下标 -> q[e,0..H-1]。
    std::vector<int> arrival_orders_;   ///< station_conflicts 下标 -> 到达次序文字。
    std::vector<int> departure_orders_; ///< station_conflicts 下标 -> 出发次序文字。
    std::vector<std::map<int, int>> track_variables_; ///< visits 下标 -> 原轨道号 -> 文字。
};
} // namespace

InputResult<TimetableSatResult> SolveTimetableSat(
    const TimetableData& timetable, const TimetableSatOptions& options) {
    try {
        // ① 检查求解选项：时间上限必须为非负有限秒数；非法选项交由末尾 catch 返回错误。
        if (!std::isfinite(options.time_limit_seconds) || options.time_limit_seconds < 0)
            throw std::invalid_argument("SAT 时间上限必须为非负有限秒数。");

        // ② 初始化结果为 Undetermined：取得确定结论前保持“未定”，且没有时刻表见证。
        // 后续因预算耗尽提前返回时，直接保留此状态，不能将超时当作不可行。
        TimetableSatResult result{TimetableStatus::Undetermined, 0, 0, std::nullopt};
        result.backend = TimetableBackend::Sat;
        result.solver_version = CaDiCaL::Solver::version();

        // ③ 创建计时器、CaDiCaL 求解器和编码器：编码与搜索共用同一时间预算。
        // 设置日志、随机种子和终止回调，再让编码器引用输入数据、求解器及计时器。
        TimeBudget budget(options.time_limit_seconds); // 比 solver 先构造，回调对象最后析构。
        CaDiCaL::Solver solver;
        solver.set("quiet", options.solver_logging ? 0 : 1);
        solver.set("seed", 0); // 固定随机种子，便于复现实验。
        solver.connect_terminator(&budget);
        TimetableEncoder encoder(timetable, solver, budget, options.candidate_assumptions);

        // ④ encoder.Encode()：检查建模数据、分配布尔变量，并将各类约束编码为 SAT 子句。
        // 本段结束后记录编码耗时及规模；编码中途超时则提前返回，不求解不完整的公式。
        const auto encoding_start = Clock::now(); // 单独记录编码耗时，预算仍用整个调用起点。
        try {
            budget.CheckEncoding();
            encoder.Encode();//建模，加入候选激活条件
        } catch (const EncodingTimeout&) {
            result.encoding_seconds = SecondsSince(encoding_start);
            result.variable_count = encoder.variable_count();
            result.clause_count = encoder.clause_count();
            return result; // 编码未完成绝不能对部分公式求解并返回可行。
        }
        result.encoding_seconds = SecondsSince(encoding_start);
        result.variable_count = encoder.variable_count();
        result.clause_count = encoder.clause_count();

        result.candidate_assumptions = encoder.CandidateMappings();
        encoder.AssumeAllCandidates();//要求本轮所有候选启用

        // ⑤ solver.solve()：仅在编码完成且仍有预算时启动 CaDiCaL，保存原始返回码和搜索耗时。
        // 即便编码刚好完成，只要预算已耗尽，也直接返回 Undetermined。
        if (budget.terminate()) return result;
        const auto solving_start = Clock::now(); // 只包含 CaDiCaL solve 调用。
        result.solver_status = solver.solve();//求解一次
        result.solving_seconds = SecondsSince(solving_start);

        // ⑥ 根据返回码处理结果：10=可满足，20=已证不可满足，0=未完成判定；其他值报错。
        if (result.solver_status == 10) {
            // ⑦ 仅在可满足分支执行 Decode()：将布尔解还原为事件时刻、轨道分配及列车次序。
            // 再按原始约束独立检查见证；检查失败返回错误，通过后才正式标记 Feasible。
            result.solution = encoder.Decode();
            if (const auto error = ValidateTimetableSolution(timetable, *result.solution)) return *error;
            result.status = TimetableStatus::Feasible;
        } else if (result.solver_status == 20) {
            result.status = TimetableStatus::ProvenInfeasible;
            // 直接查询一次核心，不删除候选、不重复搜索；异常不撤销完整集合的证明。
            if (options.candidate_assumptions) {
                const auto core_start = Clock::now();
                try {
                    // 只有结果为 UNSAT 且启用了 assumptions 模式，才执行
                    result.candidate_core = encoder.ExtractCandidateCore();
                } catch (const EncodingTimeout&) {
                    result.core_error = "提核预算耗尽";
                } catch (const std::exception& error) {
                    result.core_error = error.what();
                }
                result.core_extraction_seconds = SecondsSince(core_start);
            }
        } else if (result.solver_status != 0) {
            throw std::runtime_error("CaDiCaL 返回了未知状态码。");
        }
        // 返回码为 0 时没有进入上述状态赋值分支，因此保留步骤②的 Undetermined。
        return result;
    } catch (const std::exception& error) {
        // 异常出口：选项、编码或解码错误均作为 InputError 返回，不冒充不可行证明。
        return InputError{"SAT 时刻表错误", "SolveTimetableSat", error.what()};
    }
}

void PrintTimetableSatReport(const TimetableData& timetable,
    const TimetableSatResult& result, std::ostream& output) {
    const char* status = result.status == TimetableStatus::Feasible ? "可行"
        : result.status == TimetableStatus::ProvenInfeasible ? "已证不可行" : "未定";
    output << "第一层 SAT 状态=" << status << "，CaDiCaL 状态码=" << result.solver_status
           << "，版本=" << result.solver_version << "，seed=0\n"
           << "变量数=" << result.variable_count << "，子句数=" << result.clause_count
           << "，编码秒=" << result.encoding_seconds << "，求解秒=" << result.solving_seconds << '\n';
    for (const auto& mapping : result.candidate_assumptions) {
        output << "assumption 候选=" << mapping.candidate_index << " 文字=" << mapping.literal << " 列车=";
        for (int train : mapping.train_indices) output << ' ' << train;
        output << " 条件子句数=" << mapping.guarded_clause_count << '\n';
    }
    if (result.candidate_core) {
        output << "直接候选核={";
        for (int candidate : *result.candidate_core) output << ' ' << candidate;
        output << " }\n";
    }
    output << "提核秒=" << result.core_extraction_seconds << " 提核错误=" << result.core_error << '\n';
    if (!result.solution) return;
    const auto& solution = *result.solution; // 见证与本轮输入索引一一对应。
    for (const auto& [train_index, train] : timetable.trains) {
        output << "列车 " << train_index;
        if (train.candidate_index) output << " 候选=" << *train.candidate_index;
        else output << " 既有";
        output << '\n';
        for (std::size_t v : train.visit_indices) {
            const auto& visit = timetable.visits.at(v);
            const auto arrival = visit.arrival_side_event_index;   // 实际到达或开始占轨。
            const auto departure = visit.departure_side_event_index; // 实际出发或释放轨道。
            output << "  访问 " << v << " 车站=" << visit.station_index << " 轨道=" << solution.visit_platforms.at(v)
                   << (timetable.events.at(arrival).kind == Event::StartOccupation ? " 开始占轨=" : " 实际到达=")
                   << static_cast<std::int64_t>(solution.event_times.at(arrival)) * timetable.SecondsPerStep()
                   << (timetable.events.at(departure).kind == Event::ReleaseTrack ? " 释放轨道=" : " 实际出发=")
                   << static_cast<std::int64_t>(solution.event_times.at(departure)) * timetable.SecondsPerStep()
                   << "（秒）\n";
        }
    }
}
