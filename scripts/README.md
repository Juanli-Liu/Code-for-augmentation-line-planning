# 脚本用途与保留范围

主程序运行不依赖本目录。这里保留验收入口、原合成算例工具，以及具有来源追溯用途的数据准备脚本。当前区间次序模型由 C++ 实现，Python 辅助检查不能替代正式求解和独立见证校验。

## 推荐入口

| 脚本 | 用途与边界 |
|---|---|
| `run_baseline.ps1` | 构建生产工程及 C++ 综合验收，保存快照、哈希、日志和报告；需要 PowerShell 7、MSBuild、COPT 和 CaDiCaL |
| `validate_input_data.py` | 固定读取原 `Data` 合成算例，检查输入及部分聚合条件；不是任意数据目录的通用校验器 |
| `validate_existing_timetable.py` | 检查原 `Data` 的既有时刻表；不验证新增方案可插入，也不替代当前区间次序模型 |
| `plot_existing_timetable.py` | 绘制原 `Data` 的时空图与固定股道占用；需要 ReportLab 和脚本指定的本机字体 |

在仓库根目录运行：

```powershell
& .\scripts\run_baseline.ps1
python scripts/validate_input_data.py
python scripts/validate_existing_timetable.py
python scripts/plot_existing_timetable.py
```

后三项绑定原算例的路径、字段和假设，不能直接用于 `Case_study_data`。`validate_input_data.py` 复用另两个 Python 模块，故成组保留在原位置，避免破坏导入关系和现有文档链接。

验收脚本以每次输出的 `PASS.txt` 为通过凭据。发布仓库的测试与生产代码同步状态见[项目说明](../README.md)；本次目录整理不构成求解验收。

## 按需使用的数据工具

| 脚本 | 保留原因 |
|---|---|
| `extract_real_timetable.py` | 从原始工作簿提取时刻，保留来源和歧义信息；数据提取测试仍使用它 |
| `prepare_real_corridor_packages.py` | 将提取结果整理为走廊资料包，保留缺失项；待补齐资料包不是完整求解输入 |
| `inspect_private_sources.py` | 检查本地源资料结构，辅助定位输入和提取问题 |
| `extract_private_timetable.py` | 早期临时提取工具，供旧资料追溯；新工作优先使用 `extract_real_timetable.py` |
| `audit_private_resource_gaps.py` | 检查本地源资料的资源信息缺口 |
| `audit_real_resources.py` | 核对真实资源资料；并非当前案例股道的现场核定依据 |
| `screen_real_corridors.py` | 从提取结果筛选研究走廊 |
| `generate_scale_inputs.py` | 生成规模实验合成数据；不代表真实时刻表重放 |

这些脚本按具体任务使用，不是主程序运行的前置步骤，也不是 `Case_study_data` 的统一重建流水线。使用前查看脚本参数和路径，按需安装 `openpyxl`、`python-docx`、`pypdf` 等依赖；不要批量执行整个目录。

工作区可能还保留未提交的一次性数据修复、端点补齐和结果比较脚本。本次未将这些本地工具自动加入发布范围，也未修改它们或已有输出。

## 已退出当前工作流程的脚本

以下两个旧回归脚本已从 `scripts` 顶层移至 `legacy/`，保留历史内容，未删除文件：

| 归档路径 | 原因 |
|---|---|
| `legacy/test_input_data.py` | 调用现行主程序已停用的 `--report-input` 接口 |
| `legacy/test_master_problem.py` | 调用现行主程序已停用的 `--solve-master` 接口 |

它们仅适合研究旧接口或与匹配的历史程序配套使用，不应用来验收当前版本。归档后已调整仓库根目录定位和模块导入；当前工作请使用 C++ 验收入口。基线快照只收集 `scripts` 顶层脚本，归档目录不参与当前验收快照。
