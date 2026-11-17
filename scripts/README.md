# 脚本适用范围

| 脚本 | 当前用途 |
|---|---|
| run_baseline.ps1 | 当前基线入口：构建正式程序及 C++ 双后端综合验收，保存快照、哈希、日志和报告 |
| validate_input_data.py | 原 Data 算例专用静态数据检查，不证明任意候选组合可插入 |
| validate_existing_timetable.py | 既有运行图辅助检查，不代替 C++ 第一层求解 |
| plot_existing_timetable.py | 既有运行图绘图工具，不参与优化 |
| test_input_data.py | 历史 Python 回归，依赖主程序已停用的 --report-input 接口 |
| test_master_problem.py | 历史 Python 回归，依赖主程序已停用的 --solve-master 接口 |

两个历史回归脚本保留供查阅，不用于当前主程序验收。应运行 `run_baseline.ps1` 或
`tests/timetable_sat_tests.vcxproj`。独立 C++ 测试程序仍可接收目录参数，不能据此认为主程序也支持。
辅助 Python 检查/绘图工具不在本轮综合基线的验收范围内。
