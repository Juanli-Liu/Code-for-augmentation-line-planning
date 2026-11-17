# 增量列车开行方案与第一层时刻表验证

当前实现：COPT 主问题 + 第一层 COPT MILP / CaDiCaL SAT；支持完整集合割与 SAT assumptions 直接核割。
第一层可行表示存在时刻表及轨道分配，不表示具体动车组交路已经可行。

## 仓库内容与本机文件

本仓库提交生产源码、Visual Studio 工程、三套合成示例输入、测试与辅助脚本，
以及 CaDiCaL 的 Windows 构建所需源码和 MIT 许可证。依赖安装见
[third_party/README.md](third_party/README.md)。COPT SDK 与许可证须自行配置。

`.gitignore` 按允许清单筛选提交内容：编译产物、IDE 缓存、运行日志及报告、
`tmp`、`results`、`baselines`、`experiments`、第三方安装包和私有真实数据均保留在本机，
不纳入版本库。新增文件类型时需要显式更新允许清单。
下文提及的固定基线快照和真实数据处理输出是本地记录，不随仓库发布；
可使用已提交的测试脚本重新生成验收结果。

## 正式入口

在项目根目录执行命令。主程序 `main.cpp` 的 `kInputDataDirectory` 当前指定
`augmentation line plan/Test_data_complex` 的绝对路径。切换输入目录需要改该常量并重新编译。
主程序不读取命令行参数；旧 `--report-input`、`--solve-master`、`--report-timetable`、
`--solve-timetable` 及命令行数据目录均已停用。独立 C++ 测试程序的目录参数仍有效。

当前输入目录的 `parameters.csv` 控制求解：

| 参数 | 含义 |
|---|---|
| timetable_solver | 1=COPT MILP，2=CaDiCaL SAT；修改后重新运行即可 |
| master_solver_logging / timetable_solver_logging | 分别控制主问题和第一层日志，0/1 |
| maximum_rounds / total_time_limit_seconds | 最大轮数 / 全流程墙钟预算 |
| master_time_limit_seconds | 每轮主问题求解预算 |
| timetable_time_limit_seconds | MILP 求解预算；SAT 编码及搜索预算 |
| planning_output_root | 结果目录；相对路径基于进程工作目录 |
| conflict_cut_mode（可选） | 1=完整集合割（默认），2=assumptions直接提核；模式2要求SAT后端 |
| core_verification_time_limit_seconds（可选） | 核独立重建与复核预算，默认60秒，另受全流程剩余预算限制 |

`Meaning` 列用于解释，不参与模型计算。模型数据和完整参数定义见
[Data 输入说明](augmentation%20line%20plan/Data/README.md)。

在 Visual Studio x64 开发环境中构建并运行正式程序：

```powershell
msbuild "augmentation line plan/augmentation line plan/augmentation line plan.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /p:VCToolsVersion=14.51.36231
& ".\augmentation line plan\augmentation line plan\x64\Release\augmentation line plan.exe"
```

可执行文件位置以 MSBuild 输出为准。本机使用 MSVC 14.51.36231、COPT 8.0.7、CaDiCaL 3.0.1。
其他机器需要兼容工具链及可用 COPT 许可，并调整源码中的输入绝对路径和参数表中的输出绝对路径。

## 固定与复验基线

在 PowerShell 7 中推荐统一使用以下脚本；脚本构建正式程序和 C++ 综合验收程序，随后直接调用生产模块验收。
测试中的配置入口是与 main 相同的 `RunPlanningIteration(data)`，并非旧命令行入口。

```powershell
& .\scripts\run_baseline.ps1
```

脚本自动查找 MSBuild，也可传入 `-MSBuildPath`、`-VCToolsVersion` 和 `-OutputDirectory`。
输出目录必须不存在，默认新建 `tmp/baseline_<唯一编号>`；失败不生成 `PASS.txt`。
目录包含源码/工程/测试/输入快照 `snapshot`、SHA-256 清单 `manifest.json`、
环境记录、构建日志、验收日志、`reports/comparison.csv` 和逐轮报告。
第三方库不重复复制；版本写入环境记录。构建仍使用当前工作区，不在快照目录内构建。
脚本验证运行前后快照所对应的工作区文件哈希未变，不清理历史文件。

2026-11-14 固定基线保存在 `baselines/full_set_20261114_final`，以其 `PASS.txt` 为实际验收通过凭据。
后续实验使用新的输出目录；保留此目录作为 assumptions / 核缩减改动前的对照。
当前脚本还运行 assumptions 验收，输出 `cut_comparison.csv` 和 `cut_rounds.csv`，不会覆盖固定基线。
快照是源文件备份，不是独立安装包；恢复或比较时须结合本机第三方依赖和路径设置。

| 验收对象 | 完整集合割基准（两种后端） |
|---|---|
| 时间编码 | 350 个算术案例，以及手工 MILP/SAT 对照和见证拒绝检查 |
| 复杂算例，后移上限 0 / 5 | 各 64 个子集，36 可行、28 已证不可行、0 未定 |
| Test_data_complex 完整迭代 | 21 轮、20 条割，最终 {0,2,6,8}，目标 91700 |
| Test_data 完整迭代 | 2 轮，{0} 不可行后改选 {2}，目标 3400 |
| Data 完整迭代 | 1 轮可行，目标 144900 |
| 终止与配置 | 未定不加割、预算/轮数限制、CSV 后端选择及非法参数拒绝 |

详细验收范围见 [tests/README_sat.md](tests/README_sat.md)。时刻表可行见证不唯一，
不要求两种后端输出相同的事件时刻或轨道；每份见证必须独立满足原约束。
轮数和选择是当前输入、工具链与求解器下的回归基准，不是任意环境的数学不变量。

## 代码与辅助工具

生产主线：`input_data` → `master_problem` → `timetable_data` →
`timetable_milp` 或 `timetable_sat` → `timetable_solution` 独立检查。
`planning_iteration` 负责后端分派、持久主问题、直接核复核、回退加割和逐轮报告，`main` 负责启动及输出。

`tests/timetable_sat_tests.cpp` 是当前综合验收入口。独立 `input_data_tests`、
`planning_iteration_tests` 及算例的 `verify_iteration` / `verify_complex` 保留作专项检查。
旧 Python 应用回归依赖已停用的命令行接口，不纳入当前基线；详见 [scripts/README.md](scripts/README.md)。

## 直接核模式及后续实验

当前已实现候选 `assume()` / `failed()` 直接提核：全部选中候选在完整求解中必须开行。
新增相关子句由所有参与候选共同激活，既有背景无条件生效。UNSAT 后的核用原构建器独立重建，
关闭 assumptions 再求解；仅复核 UNSAT 后使用该核加割。提核失败或复核 SAT/UNKNOWN/异常时回退已证
不可行的完整集合；原问题 UNKNOWN 不加割。空核单独复核既有背景，不生成空集合割。

三份工作区算例的 `parameters.csv` 已显式列出割模式和复核预算，当前均为 `timetable_solver=1`、
`conflict_cut_mode=1`、复核预算60秒。正式程序每次启动读取所用算例参数表；改这两项为2即可启用
SAT直接核，无需命令行模式参数、重新编译或运行验收脚本。固定基线快照中的原始CSV保留不变。
启用方式、映射说明、预算与验收见
[assumptions 直接提核说明](tests/README_assumptions.md)。新实验数据由 C++ 测试写入独立目录。

后续顺序：assumptions 核进一步缩减 → 证明追踪直接提核 → 证明核以相同规则和预算进一步缩减。
本次未实现删除式缩核、证明追踪或跨轮SAT实例复用；核不保证极小，也不保证严格小于完整集合。

第二层运行图—动车组交路联合验证留待后续，不属于本轮工作范围。

## 真实既有时刻的输入适配

`parameters.csv` 支持 `time_step_seconds,1,second_per_time_step,...`，与旧
`time_step_minutes` 二选一。省略秒参数时仍读取旧分钟参数。所有标为 `time_step`
的时刻、运行时间、停站、间隔、折返与规划时域必须按选定步长提供；程序不会自动将旧表乘60。
墙钟求解预算仍以秒计，不参与换算。时刻表报告统一显示秒，模型内部仍使用整数时间步。

可选参数 `existing_timing_mode,1,mode,...` 使既有列车的逐区间运行时间取自其公布到发之差，
中间停站下界取自该车实际停站时长。候选列车仍沿用区间基础时分、起停附加与统一停站下界。
省略此参数或取0时保留原统一规则校验。真实时刻固定测试还须显式设定
`existing_train_max_delay=0`；模式1本身不会覆盖用户配置的后移上限。

这两项配置不补齐缺失时刻、股道、线路拓扑或截取边界规则。原输入格式的始终点仍具有
始发/终到占轨语义，不能直接将真实走廊截取点当作始终点。秒级全天时域也会增大当前SAT编码，
后续性能实验须记录编码时间和规模，不能将预算耗尽当成不可行。

本地私有数据处理工具为 `scripts/extract_real_timetable.py`、
`scripts/prepare_real_corridor_packages.py` 和 `scripts/audit_private_resource_gaps.py`。
输出仅保存在被忽略的 `experiments` 目录；该目录中的 `ready_for_solver=false` 数据包是待补齐资料，
不能作为完整运行图或已通过求解校验的输入。`tests/test_real_timetable_extraction.py` 校验逐行来源，
C++综合验收另用合成秒级样例验证两个后端，真实记录不会混入公开测试夹具。
