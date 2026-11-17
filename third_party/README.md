# 构建依赖

## CaDiCaL 3.0.1

仓库保留 `cadical-3.0.1/src`、本地 `msvc/cadical.vcxproj` 和
`msvc-compat` Windows 适配头文件，以及上游 README、版本和 MIT 许可证。
这是供本项目 Visual Studio 工程使用的源码子集，不包含上游完整的 Unix 构建工具和测试集。
编译生成的静态库、对象文件和调试文件不提交。

## COPT 8.0.7

COPT SDK 和许可证不随本仓库分发。请自行安装并配置有效许可。
现有工程默认从 `third_party/copt80` 读取依赖，所需布局为：

```text
copt80/
  include/             # 包括 coptcpp_inc
  lib/copt_cpp.lib
  bin/copt_cpp.dll
```

也可以通过 MSBuild `/p:CoptRoot=<安装目录>` 指定 SDK 根目录。
所有 COPT 安装文件仍由 `.gitignore` 排除。

工程当前使用 MSVC v145、Windows SDK 和 C++20；具体构建命令见根目录 README。
