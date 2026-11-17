///////////////////////////////////////////////////////////////////////////
/// @file copt_smoke_test.cpp
/// @brief COPT C++ 接口部署验证程序
/// @details 构建并求解一个小型二元整数规划，校验求解状态、变量值和目标值。
///////////////////////////////////////////////////////////////////////////

#include "coptcpp_pch.h"

#include <cmath>
#include <iostream>

namespace {

constexpr double kTolerance = 1.0e-6;

///////////////////////////////////////////////////////////////////////////
/// @brief 判断两个浮点数是否在给定容差内相等
/// @param left 左侧数值
/// @param right 右侧数值
/// @return 两个数值之差不超过容差时返回 true
///////////////////////////////////////////////////////////////////////////
bool IsNearlyEqual(double left, double right) {
    return std::abs(left - right) <= kTolerance;
}

}  // namespace

///////////////////////////////////////////////////////////////////////////
/// @brief 构建并求解 COPT 冒烟测试模型
/// @return 验证成功返回 0，否则返回非零错误码
///////////////////////////////////////////////////////////////////////////
int main() {
    try {
        Envr environment;
        Model model = environment.CreateModel("copt_smoke_test");

        Var x = model.AddVar(0.0, 1.0, 0.0, COPT_BINARY, "x");
        Var y = model.AddVar(0.0, 1.0, 0.0, COPT_BINARY, "y");

        model.SetObjective(3.0 * x + 2.0 * y, COPT_MAXIMIZE);
        model.AddConstr(x + y <= 1.0, "selection_limit");
        model.Solve();

        const int solve_status = model.GetIntAttr(COPT_INTATTR_STATUS);
        if (solve_status != COPT_STATUS_OPTIMAL) {
            std::cerr << "COPT 测试失败：未获得最优解，状态码 = "
                      << solve_status << '\n';
            return 2;
        }

        const double x_value = x.Get(COPT_DBLINFO_VALUE);
        const double y_value = y.Get(COPT_DBLINFO_VALUE);
        const double objective_value = model.GetDblAttr(COPT_DBLATTR_OBJVAL);

        if (!IsNearlyEqual(x_value, 1.0) ||
            !IsNearlyEqual(y_value, 0.0) ||
            !IsNearlyEqual(objective_value, 3.0)) {
            std::cerr << "COPT 测试失败：求解结果与预期不一致。\n"
                      << "x = " << x_value << ", y = " << y_value
                      << ", objective = " << objective_value << '\n';
            return 3;
        }

        std::cout << "COPT 测试通过。\n"
                  << "x = " << x_value << ", y = " << y_value
                  << ", objective = " << objective_value << '\n';
        return 0;
    } catch (const CoptException& error) {
        std::cerr << "COPT 异常，错误码 = " << error.GetCode()
                  << "，信息：" << error.what() << '\n';
        return 4;
    } catch (const std::exception& error) {
        std::cerr << "标准异常：" << error.what() << '\n';
        return 5;
    } catch (...) {
        std::cerr << "未知异常。\n";
        return 6;
    }
}
