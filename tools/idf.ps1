# tools/idf.ps1 —— 本机 ESP-IDF 构建入口
#
# 环境来源就是用户本机 EIM 生成的那份 profile（与 IDE 里点"Build"用的是同一套）：
#   C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
# 它设置 IDF_PATH / IDF_TOOLS_PATH / IDF_PYTHON_ENV_PATH 并**打开 ccache**
# （IDF_CCACHE_ENABLE=1，增量构建快很多），这是 AGENTS.md 第 3 节手写 export.ps1
# 所没有的。
#
# 为什么还要包一层：AGENTS.md 的 `. export.ps1` 在本机会被 PowerShell 执行策略拦住
#   （它点源的临时激活脚本没有签名 → UnauthorizedAccess），而
#   `Set-ExecutionPolicy -Scope Process` 只对当前进程生效、没法持久化。
#   本脚本把"进程级 Bypass + profile + 代理 + bmgr 扩展"一次做完，用法与 idf.py 一致：
#
#     & .\tools\idf.ps1 build
#     & .\tools\idf.ps1 -B build_x build
#     & .\tools\idf.ps1 fullclean
#     & .\tools\idf.ps1 -p COM20 flash monitor
#
# ⚠ 本机沙箱下 `idf.py build` 有时会因为 asyncio 建管道失败而报
#   `PermissionError: [WinError 5]`；那条路可以直接退化成
#     & .\tools\idf.ps1 cmakebuild      # = cmake --build build
#   （ninja 会自动重跑 cmake 配置，改了 CMakeLists.txt / 新增源文件照样生效）

param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

$profilePath = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
if (Test-Path $profilePath) {
    . $profilePath | Out-Null
} else {
    . "D:\Espressif\EIM\v6.1\esp-idf\export.ps1" | Out-Null
    $env:IDF_PYTHON_ENV_PATH = "D:\Espressif\EIM\tools\python\v6.1\venv"
    $env:PATH = "D:\Espressif\EIM\tools\python\v6.1\venv\Scripts;" + $env:PATH
}

# 组件管理器拉依赖必须显式设置 https_proxy（否则报"组件找不到"）
$env:https_proxy = "http://127.0.0.1:7890"
$env:HTTPS_PROXY = "http://127.0.0.1:7890"

# 未安装 esp-bmgr-assist 时，让 idf.py 找到 board manager 扩展
$env:IDF_EXTRA_ACTIONS_PATH = (Resolve-Path "$PSScriptRoot\..\managed_components\espressif__esp_board_manager").Path

# ⚠⚠ 必须清掉 MSYSTEM（2026-11 踩）：从 git-bash 里起的 PowerShell 会继承 MSYSTEM=UCRT64，
#   而 idf.py 一看到它就判定"运行在 MSys/Mingw 下"，只打一行
#     MSys/Mingw is no longer supported. ... continue at your own risk.
#   然后**直接 exit 0、什么都不做** —— 表现就是 `idf.ps1 build` / `idf.ps1 flash`
#   只打印环境横幅、既没编译也没烧写（极易误判成"已经成功"）。
#
# ⚠ 只删 `Env:` 驱动项**不够**（实测删完 `$env:MSYSTEM` 还是 UCRT64），必须走
#   [Environment]::SetEnvironmentVariable(..., 'Process') 才真的清掉。
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
[Environment]::SetEnvironmentVariable('MSYSTEM', $null, 'Process')

if ($IdfArgs.Count -gt 0 -and $IdfArgs[0] -eq 'cmakebuild') {
    $rest = @()
    if ($IdfArgs.Count -gt 1) { $rest = $IdfArgs[1..($IdfArgs.Count - 1)] }
    & cmake --build build @rest
    exit $LASTEXITCODE
}

& idf.py @IdfArgs
exit $LASTEXITCODE
