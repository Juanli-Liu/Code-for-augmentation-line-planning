# 第一层 SAT 与 MILP 的 C++ 一致性验收

SAT 使用现有 `BuildTimetableData` 的输出，不重建候选、事件或冲突集合。主问题仍由 COPT 求解。第一层可选择 COPT MILP 或 CaDiCaL SAT；两者返回相同的业务状态及整数时刻表见证。完整集合模式保留原编码；新增 assumptions 直接提核与独立子集复核，未实现进一步缩核，详见 [直接提核说明](README_assumptions.md)。

## 正式程序选择后端

在当前输入目录的 `parameters.csv` 中修改已有的 `timetable_solver` 参数：

```csv
parameter,value,unit,Meaning
timetable_solver,2,backend,第一层时刻表求解器；1=MILP（COPT）；2=SAT（CaDiCaL）
```

值为 `1` 时使用 MILP，值为 `2` 时使用 SAT。每次运行读取该必填参数；缺失、非整数、超出 1/2 或错误单位都会在数据加载阶段报错。项目输入表当前设为 `1`，切换为 `2` 不需要重新编译。`Meaning` 是参数含义与用途说明，不参与数学模型计算。输入目录仍由 `main.cpp` 中已有的 `kInputDataDirectory` 决定。

C++ 调用方也可以通过 `PlanningOptions::timetable_backend` 选择。逐轮结果保留 `backend` 和原始状态码，报告不会把 CaDiCaL 状态误解释为 COPT 状态。

## 文件职责

| 文件 | 职责 |
|---|---|
| `timetable_sat.h/.cpp` | 顺序编码、条件时间差、轨道选择、CaDiCaL 求解与解码 |
| `timetable_solution.h/.cpp` | 共用结果与直接检查原约束的见证检查器 |
| `planning_iteration.h/.cpp` | 持久主问题迭代、MILP/SAT 求解分派及报告分派；原 timetable_solver 已合并至此 |
| `timetable_sat.props` | 共用编译依赖；主工程直接登记源码，测试工程复用源码清单 |
| `tests/timetable_sat_tests.cpp/.vcxproj` | 原生 C++ 算术、小例、组合和迭代验收 |

`timetable_milp.cpp` 的模型逻辑保持不变；`master_problem.cpp` 新增从参数表读取的日志控制，数学模型不变。`master_solver_logging` 控制主问题 COPT 日志，`timetable_solver_logging` 控制第一层日志，均为必填的 0/1 开关；项目参数表默认设为 0，程序结果报告仍保留。MILP 头文件中的结果名称通过类型别名保留，原函数仍可直接调用。MILP 和 SAT 均在各自求解入口内提取或解码可行见证，并调用同一个独立检查器；检查通过才返回可行结果，失败返回 `InputError`。共用分派层只选择后端，不重复检查。

## 编码边界与判定

- `q[e,k]` 表示事件时刻不晚于 `k`；负下标为假，`k>=H` 为真。业务时间变量分配在 `0..H-1`。
- `tau[after]-tau[before]>=gap` 通过 `q[after,k] -> q[before,k-gap]` 编码，范围包含 `k=H`。运行等式由正反两条下界构成；反向偏移和时间差计算使用 64 位整数。
- 轨道和次序条件以激活文字的否定加入子句；直接核模式另加入参与候选的否定文字，每次重建 SAT 实例。
- 变量统计包含编号 1 的真常量；子句统计包含固定该常量的单位子句，不包含被真常量化简掉的子句。
- CaDiCaL 的 `10/20/0` 分别映射为可行、已证不可行、未定。可行见证必须通过独立检查；异常返回 `InputError`。
- SAT 的单次预算包含编码、搜索与直接提核。编码未完成时不求解部分公式；搜索通过 `Terminator` 回调检查预算。回调检查不是操作系统硬实时抢占。解码、独立检查及析构可能带来少量额外时间。
- 原 MILP 时限语义仍是 COPT 求解阶段时限。报告分别记录 SAT 编码和搜索耗时，性能比较时应明确计时范围。
- 外层只有原问题已证不可行才进入加割；直接核须独立复核 UNSAT，否则回退完整集合，原问题未定不加割。

## 编译与运行

统一基线入口为项目根目录的 `scripts/run_baseline.ps1`，可自动构建、验收并保存源码与输入快照、
SHA-256 清单、完整日志和逐轮结果。固定基线及后续 assumptions 路线见 [项目说明](../README.md)。
以下命令用于单独运行综合测试；主程序不支持旧 `--solve-*` 参数。

在项目根目录、Visual Studio x64 开发环境中运行：

```powershell
msbuild "tests/timetable_sat_tests.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /p:VCToolsVersion=14.51.36231
& ".\tmp\timetable_sat_tests_bin\Release\timetable_sat_tests.exe" (Get-Location).Path
```

本次使用本机已安装的 MSVC 14.51.36231；在其他机器上应指定其已安装且兼容的工具链版本。将 `Release` 改为 `Debug` 可运行同一套断言；断言通过显式检查实现，不受 `NDEBUG` 影响。主解决方案也已包含验收工程。

所有测试逻辑均为 C++，直接编译、调用生产源文件和真实 COPT/CaDiCaL。旧 Python 脚本不参与本验收。每次测试在 `tmp/sat_acceptance_<时间戳>/` 创建独立目录，包含 `comparison.csv`、各次迭代报告和 CSV 配置测试副本，不覆盖原算例。

本机工具链兼容处理：CaDiCaL 静态库关闭 `/GL`，避免链接不同业务入口时触发 MSVC C1001；保留普通 Release 优化。新集成工程使用 `/Z7` 将调试信息写入对象文件，避免本机 PDB 服务的 C1902。未修改 CaDiCaL 算法源码。

## 验收项目与基准

1. 350 个固定时间差的算术真值案例，覆盖等式、下界、负时间差、零及超过时域的时长。
2. 20 个手工 MILP/SAT 对照案例：时域 0/H、通过、端点占轨、共同/独立轨道、固定轨道、到发间隔、禁越行，以及后移 0 时不可行、后移 1 后恢复可行。
3. 复杂算例分别设后移上限为 0 和 5，各枚举 64 个候选组合；两组均为 36 可行、28 已证不可行、0 未定。每个可行见证均检查原约束。
4. 两种后端分别运行复杂算例：21 轮、20 条完整集合割，最终 `{0,2,6,8}`，目标 `91700`；逐轮重算客流和目标，并用完整组合真值表检查割未排除可行组合。
5. 两种后端分别运行 `Test_data`：两轮，首轮 `{0}` 不可行并加割，次轮 `{2}` 可行，目标 `3400`。
6. 两种后端分别运行原 `Data`：一轮可行，目标 `144900`。
7. SAT 零预算返回未定且不加割；总预算与轮数限制；四列 CSV 通过正式入口分别选择 1=MILP 和 2=SAT；非法及缺失后端拒绝；人为破坏见证后检查器拒绝。

某一方法出现未定时，测试会明确失败并报告“对照未完成”，不会把两个未定算成一致性通过。上述基准属于小规模正确性验收，不代表大规模性能或具体动车组交路已经验证。
