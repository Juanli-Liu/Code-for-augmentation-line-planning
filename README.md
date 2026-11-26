# 基于区间次序的增量列车开行方案优化

本项目使用 COPT 求解新增列车开行方案主问题，并通过 COPT MILP 或 CaDiCaL SAT 验证第一层时刻表可行性。当前版本按**共用有向区间的列车对**建立次序变量，联合决定事件时刻和到发线分配。

主流程为：读取 CSV → 主问题选择候选方案并分配客流 → 第一层验证 → 已证不可行时加冲突割并重新求解。第一层可行只表示存在满足当前约束的时刻表和股道分配，尚不证明具体动车组交路可行。

## 当前模型

- 每个共用有向区间的列车对使用一个次序变量；区间入口出发和出口到达遵循同一次序，分别满足出发、到达间隔，防止区间内越行。
- 同站列车选择同一股道时，按关联区间次序约束前车释放到后车占用的间隔。优先关联共同入站区间，没有共同入站区间时关联共同出站区间。
- 当前数据构建器要求：反向列车的允许股道集合不相交；没有共同相邻区间的列车，其允许股道集合也不相交。违反这些前提会报错，不能将本模型直接视为任意枢纽交叉进路模型。
- 每次车站访问有到达侧、出发侧两个事件。区间运行使用固定时长；中间通过到发同刻；中间停站和始终点占轨采用时长下界。
- MILP 和 SAT 共用事件、区间次序和股道冲突数据；可行见证均由 `timetable_solution` 独立检查。
- `conflict_cut_mode=1` 使用完整选中候选集合割；模式 `2` 使用 SAT assumptions 提核，并独立重建复核。复核失败或未完成时回退完整集合割；原问题未定不加割。

## 目录与依赖

| 路径 | 用途 |
|---|---|
| `augmentation line plan/augmentation line plan/` | C++ 生产源码与 Visual Studio 工程 |
| `augmentation line plan/Data/` | 原合成算例及输入格式说明 |
| `augmentation line plan/Test_data/` | 基础合成验收算例 |
| `augmentation line plan/Test_data_complex/` | 当前主程序默认读取的复杂合成算例 |
| `augmentation line plan/Case_study_data/` | 案例研究的网络、候选方案、需求与既有时刻表数据 |
| `tests/` | C++ 验收及数据提取测试；不参与生产入口运行 |
| `scripts/` | 基线构建、原算例检查和数据准备工具，详见[脚本说明](scripts/README.md) |
| `third_party/` | CaDiCaL 源码、Windows 构建适配和[依赖说明](third_party/README.md) |
| `timetable_sat.props` | 主工程和测试工程共用的 SAT 构建依赖，不可直接删除 |
| `C++Style_CN.md` | 可选开发参考：[代码规范](C++Style_CN.md)，不影响运行 |

`.gitignore` 使用允许清单。编译产物、IDE 缓存、运行结果、`tmp`、`experiments` 和 COPT 安装文件不上传；`Case_study_data/code` 下的处理过程和历史核验材料也不属于当前发布输入。

## 编译与运行

需要 Windows、Visual Studio C++ 工具链（工程设置为 MSVC v145、C++20）、Windows SDK，以及自行安装并配置许可的 COPT。仓库提供 CaDiCaL 3.0.1 的 Windows 构建源码子集；COPT SDK 和许可证不随仓库发布。

默认 COPT 布局为 `third_party/copt80/include`、`lib/copt_cpp.lib`、`bin/copt_cpp.dll`；也可通过 MSBuild 的 `/p:CoptRoot=<安装目录>` 指定位置。本机使用 COPT 8.0.7 和 MSVC 14.51.36231，其他机器应选择兼容的已安装版本。

1. 修改 `main.cpp` 中的 `kInputDataDirectory`，指向本机输入目录。当前源码写入的是 `Test_data_complex` 的本机绝对路径，克隆到其他机器后必须调整。
2. 核对该目录的 `parameters.csv`，特别是求解器、规划时域、时间单位、预算和 `planning_output_root`。输出路径也可能是原机器的绝对路径。
3. 在仓库根目录的 Visual Studio x64 开发环境中执行：

```powershell
msbuild "augmentation line plan/augmentation line plan/augmentation line plan.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /p:VCToolsVersion=14.51.36231
& ".\augmentation line plan\augmentation line plan\x64\Release\augmentation line plan.exe"
```

主程序入口为无参数 `wmain()`，不读取命令行数据目录，也不支持旧 `--report-input`、`--solve-master`、`--report-timetable`、`--solve-timetable` 参数。切换输入目录需要重新编译；切换参数表中的求解器无需重新编译。

退出码：`0` 为可行，`3` 为已证不可行，`1` 为错误，`4` 为未定或达到限制。每次运行在输出根目录下创建独立运行目录，保存逐轮报告和 `summary.txt`；超时不能解释为不可行。

## 输入与案例研究数据

求解输入包括以下八份 CSV：

| 文件 | 内容 |
|---|---|
| `stations.csv` | 车站 |
| `sections.csv` | 有向区间、容量与基础运行时分 |
| `platforms.csv` | 股道及可用时间 |
| `lines.csv` | 候选线路与停车方案 |
| `line_allowed_platforms.csv` | 候选线路逐站允许股道 |
| `ods.csv` | OD 需求 |
| `timetable.csv` | 既有列车路径、时刻及已提供的股道 |
| `parameters.csv` | 时域、间隔、动车组资源、求解器与预算 |

输入语义详见 [Data 输入说明](augmentation%20line%20plan/Data/README.md)，实际解析和校验以 `input_data.cpp` 为准。

**当前发布的 `Case_study_data` 只有前七份 CSV，尚未提供 `parameters.csv`，也不是默认运行输入。** 使用该案例前，需要按研究方案补齐参数，并继续核对区间时分、股道方向分组、既有时刻和端点占轨语义；上传成功不表示已通过完整加载或求解验收。现有部分股道属于研究分配，不能等同于现场逐列实绩。

走廊截取点不能自动视为列车真正始发或终到站：当前模型仍按输入线路端点建立始终点占轨事件。正式使用真实时刻表前需明确这一边界假设。

## 主要参数

| 参数 | 含义 |
|---|---|
| `timetable_solver` | `1`：COPT MILP；`2`：CaDiCaL SAT，主问题始终使用 COPT |
| `conflict_cut_mode` | `1`：完整集合割；`2`：assumptions 直接核，要求 SAT |
| `core_verification_time_limit_seconds` | 核独立复核预算，同时受总剩余预算限制 |
| `planning_horizon` | 以模型时间步计的时域上界 |
| `time_step_minutes` / `time_step_seconds` | 分钟或秒时间步，二选一；所有时刻与时长须使用一致单位 |
| `existing_timing_mode` | `0`：统一规则；`1`：既有逐车运行时长和停站下界由公布时刻派生 |
| `existing_train_max_delay` | 既有实际事件允许后移上限；固定既有时刻时设为 `0` |
| `arrival_headway` / `departure_headway` | 共用区间出口到达、入口出发的间隔 |
| `track_headway` | 同轨前车释放到后车占用的间隔 |
| `maximum_rounds` / `total_time_limit_seconds` | 迭代次数和全流程墙钟预算 |
| `master_time_limit_seconds` / `timetable_time_limit_seconds` | 单轮主问题、第一层预算 |
| `master_solver_logging` / `timetable_solver_logging` | 对应求解器日志的 `0/1` 开关 |
| `planning_output_root` | 输出根目录；相对路径基于进程工作目录 |

程序不会自动把分钟数据乘以 60。报告显示秒，模型内部使用整数时间步；求解预算始终使用墙钟秒。SAT 预算包括编码与搜索，MILP 的第一层时限作用于求解阶段，性能比较应统一计时口径。

## 验证与复现

统一验收入口为 PowerShell 7 脚本：

```powershell
& .\scripts\run_baseline.ps1
```

它构建生产工程和 C++ 综合验收工程，保存快照、SHA-256 清单、构建日志、验收日志和报告。可指定 `-MSBuildPath`、`-VCToolsVersion`、`-OutputDirectory`；输出目录必须不存在。通过以该次运行的 `PASS.txt` 为准，脚本存在不代表当前版本已经通过。

区间次序改造涉及测试接口同步。此前仅发布生产源码和案例数据，`tests` 的相应本地修改未随该次提交上传；在发布端测试同步并重新运行前，不能宣称 GitHub 上的旧测试已验证当前模型。测试范围说明见 [SAT 验收说明](tests/README_sat.md) 和 [assumptions 说明](tests/README_assumptions.md)。旧文档中的站级次序结果、轮数和目标值只作历史参考，不作为区间次序版本的实测结论。

比较 MILP/SAT 或新旧模型时，应固定输入、候选集合、工具链、求解器版本及预算，记录状态、见证校验、编码时间和求解时间。出现未定或语义变化时，不应只比较运行速度。

## 代码导航

`input_data` 读取并校验输入；`master_problem` 选择候选、分配客流并管理冲突割；`timetable_data` 构建事件、活动、区间次序和股道冲突；`timetable_milp` / `timetable_sat` 求解第一层；`timetable_solution` 独立核验见证；`planning_iteration` 协调迭代、预算和报告；`main` 设置输入目录并启动流程。
