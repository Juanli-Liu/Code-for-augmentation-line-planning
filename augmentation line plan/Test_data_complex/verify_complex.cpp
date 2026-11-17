// 使用正式 C++ 模型和真实 COPT；枚举有效候选组合，独立核对见证及每条割。
#include "../augmentation line plan/planning_iteration.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Near(double a, double b, const char* message) { Require(std::abs(a-b) < 1e-5, message); }

struct DelayStatistics {
    int actual_events = 0;   // 仅既有实际到达/出发；不含始发占轨、终到释放。
    int delayed_events = 0;  // 后移量严格大于0的实际事件数。
    int maximum_delay = 0;
};

DelayStatistics CheckExistingDelays(const TimetableData& data, const TimetableSolution& solution,
                                   int delay_limit, std::ostream* report = nullptr) {
    DelayStatistics stats;
    if (report) *report << "train_index,station_index,event,baseline,actual,delay,limit\n";
    for (std::size_t e = 0; e < data.events.size(); ++e) {
        const auto& event = data.events[e];
        const auto& visit = data.visits.at(event.visit_index);
        if (data.trains.at(visit.train_index).candidate_index
            || (event.kind != Event::Arrival && event.kind != Event::Departure)) continue;
        Require(event.baseline_time.has_value(), "既有实际事件缺少公布时刻");
        const int delay = solution.event_times.at(e) - *event.baseline_time;
        Require(delay >= 0 && delay <= delay_limit, "既有实际事件提前或后移超过上限");
        Require(event.time_domain.lower == *event.baseline_time
                && event.time_domain.upper == std::min(data.planning_horizon, *event.baseline_time + delay_limit),
                "实际事件时间域未正确使用后移参数");
        ++stats.actual_events;
        if (delay > 0) ++stats.delayed_events;
        stats.maximum_delay = std::max(stats.maximum_delay, delay);
        if (report) *report << visit.train_index << ',' << visit.station_index << ','
                           << (event.kind == Event::Arrival ? "arrival" : "departure") << ','
                           << *event.baseline_time << ',' << solution.event_times[e] << ','
                           << delay << ',' << delay_limit << '\n';
    }
    return stats;
}

// 独立检查求解结果，不通过再次求解代替对事件、活动和冲突的核对。
void CheckWitness(const TimetableData& d, const TimetableSolution& s) {
    Require(s.event_times.size()==d.events.size() && s.visit_platforms.size()==d.visits.size()
            && s.orders.size()==d.station_conflicts.size(), "见证维度错误");
    for (std::size_t e=0;e<d.events.size();++e)
        Require(s.event_times[e]>=d.events[e].time_domain.lower && s.event_times[e]<=d.events[e].time_domain.upper,
                "事件超出时间域");
    for (const auto& a:d.activities) {
        const int duration=s.event_times.at(a.to_event_index)-s.event_times.at(a.from_event_index);
        Require(a.has_fixed_duration()?duration==a.duration:duration>=a.duration, "活动时长违反");
    }
    for (std::size_t v=0;v<d.visits.size();++v) {
        const auto& visit=d.visits[v];
        Require(std::find(visit.allowed_platform_indices.begin(),visit.allowed_platform_indices.end(),s.visit_platforms[v])
                    !=visit.allowed_platform_indices.end(), "轨道不在允许集合");
        Require(!visit.fixed_platform_index || *visit.fixed_platform_index==s.visit_platforms[v], "既有轨道改变");
    }
    for (std::size_t c=0;c<d.station_conflicts.size();++c) {
        const auto& pair=d.station_conflicts[c];
        const auto& first=d.visits.at(pair.first_visit_index);
        const auto& second=d.visits.at(pair.second_visit_index);
        const int first_arr=s.event_times.at(first.arrival_side_event_index);
        const int second_arr=s.event_times.at(second.arrival_side_event_index);
        const int first_dep=s.event_times.at(first.departure_side_event_index);
        const int second_dep=s.event_times.at(second.departure_side_event_index);
        Require((s.orders[c].first_arrives_first?second_arr-first_arr:first_arr-second_arr)>=pair.arrival_headway,
                "到达间隔违反");
        Require((s.orders[c].first_departs_first?second_dep-first_dep:first_dep-second_dep)>=pair.departure_headway,
                "出发间隔违反");
        if(s.visit_platforms[pair.first_visit_index]==s.visit_platforms[pair.second_visit_index])
            Require((s.orders[c].first_arrives_first?second_arr-first_dep:first_arr-second_dep)>=d.track_headway,
                    "同轨占用冲突");
    }
    for(const auto& order:d.section_orders)
        Require(s.orders.at(order.entry_departure_conflict_index).first_departs_first
                    ==s.orders.at(order.exit_arrival_conflict_index).first_arrives_first, "区间越行");
}

void CheckPassengers(const ProblemData& d,const MasterSolution& s) {
    std::vector<std::vector<double>> loads(d.candidates.size());
    for(const auto& a:d.candidates) loads[a->index()].resize(a->line()->od_indices_by_stop_section().size());
    double cost=0;
    Require(s.od_allocations.size()==d.ods.size(), "OD 结果数量错误");
    for(const auto& allocation:s.od_allocations) {
        const int od=allocation.od_index;
        double total=allocation.unmet_demand;
        Require(total>=-1e-6,"负未满足需求");
        cost+=total*d.ods.at(od)->unmet_unit_penalty();
        for(const auto& flow:allocation.services) {
            Require(flow.passengers>=0 && std::find(s.selected_candidate_indices.begin(),s.selected_candidate_indices.end(),
                    flow.candidate_index)!=s.selected_candidate_indices.end(),"未选中候选承运客流");
            total+=flow.passengers;
            const auto& services=d.od_services.at(od);
            const auto service=std::find_if(services.begin(),services.end(),[&](const auto& item) {
                return item.candidate->index()==flow.candidate_index;
            });
            Require(service!=services.end(),"OD 不可由该候选服务");
            cost+=flow.passengers*service->travel_time;
            const auto& coverage=service->candidate->line()->od_indices_by_stop_section();
            for(std::size_t leg=0;leg<coverage.size();++leg)
                if(std::find(coverage[leg].begin(),coverage[leg].end(),od)!=coverage[leg].end())
                    loads[flow.candidate_index][leg]+=flow.passengers;
        }
        Near(total,d.ods.at(od)->demand(),"OD 需求不守恒");
    }
    for(const auto& a:d.candidates)
        for(double load:loads[a->index()])
            Require(load<=a->frequency()*a->rolling_stock()->seat_capacity()+1e-6,"分段座位超载");
    Near(cost,s.objective_value,"目标值重算不一致");
}
} // namespace

int wmain(int argc,wchar_t* argv[]) {
    try {
        Require(argc==2,"需要 Test_data_complex 路径");
        auto loaded=LoadProblemData(argv[1]);
        if(const auto* error=std::get_if<InputError>(&loaded)) throw std::runtime_error(error->message);
        const auto& d=std::get<ProblemData>(loaded);
        Require(d.ods.size()==10 && d.candidates.size()==12 && d.existing_trains.size()==2,"算例规模不符");
        Require(d.parameters.existing_train_max_delay==5,"当前验收要求数据表中的后移上限为5");
        // 对照组重新加载同一输入，仅在测试对象内恢复旧值0；不修改任何 CSV 或生产参数。
        auto zero_loaded=LoadProblemData(argv[1]);
        if(const auto* error=std::get_if<InputError>(&zero_loaded)) throw std::runtime_error(error->message);
        auto& zero_data=std::get<ProblemData>(zero_loaded);
        zero_data.parameters.existing_train_max_delay=0;
        // 每条线路频率1，long 是偶数编号；short 备用数量0，主问题不能选择。
        const std::vector<int> active{0,2,4,6,8,10};
        std::vector<bool> feasible(64);
        int feasible_count=0;
        int recovered_count=0; // 同一组合从0上限不可行变为5上限可行的次数。
        int shifted_witnesses=0;
        std::ofstream verdicts(std::filesystem::path(argv[1])/"combination_verdicts_delay5.csv");
        verdicts.exceptions(std::ios::failbit|std::ios::badbit);
        verdicts<<"mask,candidates,status_delay0,status_delay5,recovered,delayed_actual_events,max_actual_delay,seconds_delay5\n";
        for(unsigned mask=0;mask<64;++mask) {
            MasterSolution selection{};
            for(unsigned bit=0;bit<active.size();++bit)
                if(mask&(1u<<bit)) selection.selected_candidate_indices.push_back(active[bit]);
            auto built=BuildTimetableData(d,selection);
            if(const auto* error=std::get_if<InputError>(&built)) throw std::runtime_error(error->message);
            const auto& timetable=std::get<TimetableData>(built);
            auto zero_built=BuildTimetableData(zero_data,selection);
            if(const auto* error=std::get_if<InputError>(&zero_built)) throw std::runtime_error(error->message);
            const auto& zero_timetable=std::get<TimetableData>(zero_built);
            auto zero_solved=SolveTimetableMilp(zero_timetable,{d.parameters.timetable_time_limit_seconds,false});
            if(const auto* error=std::get_if<InputError>(&zero_solved)) throw std::runtime_error(error->message);
            const auto& zero_result=std::get<TimetableMilpResult>(zero_solved);
            Require(zero_result.status!=TimetableStatus::Undetermined,"零后移对照组出现未定");
            if(zero_result.solution) {
                CheckWitness(zero_timetable,*zero_result.solution);
                CheckExistingDelays(zero_timetable,*zero_result.solution,0);
            }
            auto solved=SolveTimetableMilp(timetable,{d.parameters.timetable_time_limit_seconds,false});
            if(const auto* error=std::get_if<InputError>(&solved)) throw std::runtime_error(error->message);
            const auto& result=std::get<TimetableMilpResult>(solved);
            Require(result.status!=TimetableStatus::Undetermined,"枚举出现未定，不能得出完整结论");
            feasible[mask]=result.status==TimetableStatus::Feasible;
            const bool zero_feasible=zero_result.status==TimetableStatus::Feasible;
            Require(!zero_feasible || feasible[mask],"放宽后移限制却失去原有可行性");
            const bool recovered=!zero_feasible && feasible[mask];
            if(recovered) ++recovered_count;
            DelayStatistics delay_stats;
            if(feasible[mask]) {
                Require(result.solution.has_value(),"缺少可行见证");
                CheckWitness(timetable,*result.solution);
                delay_stats=CheckExistingDelays(timetable,*result.solution,5);
                if(delay_stats.delayed_events>0) ++shifted_witnesses;
                ++feasible_count;
            }
            verdicts<<mask<<",";
            for(int index:selection.selected_candidate_indices) verdicts<<index<<';';
            verdicts<<','<<(zero_feasible?"feasible":"proven_infeasible")
                    <<','<<(feasible[mask]?"feasible":"proven_infeasible")<<','<<recovered
                    <<','<<delay_stats.delayed_events<<','<<delay_stats.maximum_delay
                    <<','<<result.solving_seconds<<'\n';
        }
        verdicts.close();
        Require(feasible[0],"既有运行图本身不可行");
        for(unsigned bit=0;bit<active.size();++bit) Require(feasible[std::size_t{1}<<bit],"存在单独就不可行的有效候选");
        Require(!feasible[63],"全部候选组合未触发冲突");
        Require(!feasible[6] && !feasible[48],"同向两种中停候选的组合应冲突");
        Require(feasible_count==36,"组合可行数量与预期不符");
        std::cout<<"PASS exhaustive actual MILP: 36 feasible / 28 proven infeasible / 0 undetermined\n";
        std::cout<<"PASS baseline and all 6 singleton candidates feasible; every feasible witness checked\n";
        std::cout<<"DELAY COMPARISON recovered_combinations="<<recovered_count
                 <<" feasible_witnesses_with_positive_delay="<<shifted_witnesses<<"\n";

        const auto run=RunPlanningIteration(d); // 正式持久模型迭代，未预先加入任何割。
        PrintPlanningReport(run,std::cout);
        Require(run.status==PlanningStatus::Feasible && run.rounds.size()>2,"未完成多轮加割");
        std::set<unsigned> visited;
        std::vector<unsigned> cuts; // 每个 mask 是完整候选支持集，用于核对后续轮次和穷举见证。
        double previous=-1;
        for(const auto& round:run.rounds) {
            Require(round.master && round.master->solution && round.timetable,"轮次结果缺失");
            const auto& master=*round.master->solution;
            CheckPassengers(d,master);
            Require(master.objective_value>=previous-1e-6,"有效割加入后最优目标反而降低");
            previous=master.objective_value;
            unsigned mask=0;
            for(int index:master.selected_candidate_indices) {
                const auto found=std::find(active.begin(),active.end(),index);
                Require(found!=active.end(),"选中了禁用编组");
                mask|=1u<<static_cast<unsigned>(found-active.begin());
            }
            Require(visited.insert(mask).second,"候选组合重复，割未生效");
            for(unsigned cut:cuts) Require((mask&cut)!=cut,"后续解违反已有割");
            if(round.timetable->status==TimetableStatus::ProvenInfeasible) {
                Require(!feasible[mask] && round.added_conflict==master.selected_candidate_indices,
                        "割不是来自已证不可行的完整集合");
                for(unsigned superset=0;superset<64;++superset)
                    if((superset&mask)==mask) Require(!feasible[superset],"割错误排除了可行组合");
                cuts.push_back(mask);
            } else {
                Require(round.timetable->status==TimetableStatus::Feasible && feasible[mask]
                        && round.added_conflict.empty() && round.timetable->solution,"可行轮次状态错误");
                CheckWitness(*round.timetable_data,*round.timetable->solution);
            }
        }
        Near(run.rounds.front().master->solution->objective_value,21950,"初始目标错误");
        Near(run.rounds.back().master->solution->objective_value,91700,"最终目标错误");
        Require(run.rounds.back().master->solution->selected_candidate_indices==std::vector<int>({0,2,6,8}),
                "最终候选集合错误");
        const auto& final_round=run.rounds.back();
        std::ofstream delay_report(std::filesystem::path(argv[1])/"existing_event_delays_delay5.csv");
        delay_report.exceptions(std::ios::failbit|std::ios::badbit);
        const auto final_delays=CheckExistingDelays(*final_round.timetable_data,*final_round.timetable->solution,
                                                   d.parameters.existing_train_max_delay,&delay_report);
        delay_report.close();
        Require(final_delays.actual_events==16,"本例既有实际到发事件数错误");
        std::cout<<"FINAL DELAYS actual_events="<<final_delays.actual_events
                 <<" delayed_events="<<final_delays.delayed_events<<" max_delay="<<final_delays.maximum_delay<<"\n";
        std::cout<<"PASS all "<<cuts.size()<<" full-support cuts sound against all 64 combinations\n"
                 <<"PASS objectives, OD conservation, segment seats, no repeated choices, final timetable\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
