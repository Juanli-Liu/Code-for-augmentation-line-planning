#Requires -Version 7.0
param(
    [string]$MSBuildPath = '',
    [string]$VCToolsVersion = '14.51.36231',
    [string]$OutputDirectory = ''
)

# 构建并保存完整集合割及assumptions直接核验收；只创建新目录，不覆盖历史。
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $MSBuildPath) {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) { $MSBuildPath = $command.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $MSBuildPath = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        }
    }
}
if (-not $MSBuildPath -or -not (Test-Path -LiteralPath $MSBuildPath)) {
    throw '找不到 MSBuild；请通过 -MSBuildPath 指定已安装的 MSBuild.exe。'
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ('tmp\baseline_' + [guid]::NewGuid().ToString('N'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw '输出目录必须不存在，以保留历史验收。' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$snapshot = Join-Path $OutputDirectory 'snapshot'

# 保存生产源码、工程配置、测试、文档和三个算例原始输入；不复制编译中间件。
$files = @(
    Get-Item -LiteralPath (Join-Path $projectRoot 'README.md'), (Join-Path $projectRoot 'timetable_sat.props')
    Get-ChildItem -LiteralPath (Join-Path $projectRoot 'scripts') -File | Where-Object Extension -in '.py', '.ps1', '.md'
    Get-ChildItem -LiteralPath (Join-Path $projectRoot 'tests') -File | Where-Object Extension -in '.cpp', '.vcxproj', '.md'
    Get-ChildItem -LiteralPath (Join-Path $projectRoot 'augmentation line plan\augmentation line plan') -File | Where-Object Extension -in '.cpp', '.h', '.vcxproj'
    foreach ($name in 'Data', 'Test_data', 'Test_data_complex') {
        Get-ChildItem -LiteralPath (Join-Path $projectRoot "augmentation line plan\$name") -File | Where-Object Extension -in '.csv', '.md', '.cpp', '.vcxproj'
    }
)
$manifest = foreach ($file in $files) {
    $relative = $file.FullName.Substring($projectRoot.Length + 1)
    $target = Join-Path $snapshot $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $target
    [pscustomobject]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash }
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding UTF8
$dependencies = foreach ($relative in 'third_party\copt80\bin\copt_cpp.dll', 'third_party\cadical-3.0.1\VERSION', 'third_party\cadical-3.0.1\msvc\cadical.vcxproj') {
    $dependency = Join-Path $projectRoot $relative
    if (-not (Test-Path -LiteralPath $dependency)) { throw "缺少基线依赖：$relative" }
    [pscustomobject]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath $dependency -Algorithm SHA256).Hash }
}
$dependencies | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'dependencies.json') -Encoding UTF8
[ordered]@{
    recorded_at = [DateTimeOffset]::Now.ToString('o')
    workspace = $projectRoot
    msbuild = $MSBuildPath
    vc_tools_version = $VCToolsVersion
    configuration = 'Release|x64'
    copt = '8.0.7'
    cadical = '3.0.1'
    cut_strategy = 'full_selected_set_and_assumptions_direct'
    scope = 'first_layer_only'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'environment.json') -Encoding UTF8

Push-Location $projectRoot
try {
    $projects = @('augmentation line plan\augmentation line plan\augmentation line plan.vcxproj', 'tests\timetable_sat_tests.vcxproj')
    for ($i = 0; $i -lt $projects.Count; $i++) {
        & $MSBuildPath $projects[$i] /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 "/p:VCToolsVersion=$VCToolsVersion" /v:minimal /nologo *> (Join-Path $OutputDirectory "build_$i.log")
        if ($LASTEXITCODE -ne 0) { throw "构建失败；见 build_$i.log。" }
    }
    $log = Join-Path $OutputDirectory 'acceptance.log'
    & '.\tmp\timetable_sat_tests_bin\Release\timetable_sat_tests.exe' $projectRoot 2>&1 | Out-File -LiteralPath $log -Encoding UTF8
    if ($LASTEXITCODE -ne 0) { throw '验收失败；见 acceptance.log。' }
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    if (-not ($lines -contains 'ALL C++ SAT ACCEPTANCE TESTS PASSED')) { throw '缺少完整通过标记。' }
    $reportLine = $lines | Where-Object { $_.StartsWith('REPORT_DIRECTORY=') } | Select-Object -Last 1
    if (-not $reportLine) { throw '缺少验收报告目录。' }
    $reportDirectory = $reportLine.Substring('REPORT_DIRECTORY='.Length)
    # 每次只复制一个明确文件，保留全部逐轮报告和比较表。
    foreach ($file in Get-ChildItem -LiteralPath $reportDirectory -File -Recurse) {
        $target = Join-Path (Join-Path $OutputDirectory 'reports') $file.FullName.Substring($reportDirectory.Length + 1)
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
    foreach ($entry in $manifest) {
        if ((Get-FileHash -LiteralPath (Join-Path $projectRoot $entry.path) -Algorithm SHA256).Hash -ne $entry.sha256) {
            throw "运行期间源文件或输入发生改变：$($entry.path)；本次不能固定为基线。"
        }
    }
    $lines | Where-Object { $_ -match '^(PASS |ALL C\+\+)' } | Set-Content -LiteralPath (Join-Path $OutputDirectory 'PASS.txt') -Encoding UTF8
    Write-Output "基线验收通过：$OutputDirectory"
} finally {
    Pop-Location
}
