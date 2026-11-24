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
#     & .\tools\idf.ps1 -p COMx flash monitor
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

# 组件管理器拉依赖如需代理，请提前在环境中设置 https_proxy / HTTPS_PROXY

# 未安装 esp-bmgr-assist 时，让 idf.py 找到 board manager 扩展
$env:IDF_EXTRA_ACTIONS_PATH = (Resolve-Path "$PSScriptRoot\..\managed_components\espressif__esp_board_manager").Path

# ⚠⚠ 必须清掉 MSYSTEM（2026-11 踩）：从 git-bash / MSYS 环境里起的 PowerShell 会继承
#   MSYSTEM=UCRT64，而 idf.py 的入口只有一句
#       if 'MSYSTEM' in os.environ: print_warning('MSys/Mingw is no longer supported...')
#   —— 打完警告**既不 main() 也不报错，直接 exit 0、什么都不做**。表现就是
#   `idf.ps1 build` / `idf.ps1 flash` 只打印环境横幅、既没编译也没烧写
#   （极易误判成"已经成功"）。
#
# ⚠⚠ 2026-11-16 实测：**PowerShell 侧怎么删都删不干净**。`Remove-Item Env:MSYSTEM`、
#   `[Environment]::SetEnvironmentVariable('MSYSTEM', $null, 'Process')`、
#   `$env:MSYSTEM = [NullString]::Value` 三条路都会在 Windows 的环境块里留下一个
#   **空值项**：PowerShell 里 `$env:MSYSTEM` 读出来是空串，而 Python 的
#   `'MSYSTEM' in os.environ` 依然是 True（`repr(os.environ['MSYSTEM']) == "''"`）
#   —— idf.py 于是照样走 MSys 分支、照样 exit 0。
#   真正的删法只能在**子进程里**做（见文件末尾的 python 前导）。

if ($IdfArgs.Count -gt 0 -and $IdfArgs[0] -eq 'cmakebuild') {
    $rest = @()
    if ($IdfArgs.Count -gt 1) { $rest = $IdfArgs[1..($IdfArgs.Count - 1)] }
    & cmake --build build @rest
    exit $LASTEXITCODE
}

# idf.py 走一段 python 前导：先把 MSYSTEM 从 os.environ 里 pop 掉（PowerShell 侧删不掉，
# 见上面那段注释），再按原样 runpy 执行 idf.py —— 参数、工作目录、退出码都不变。
# ⚠ PowerShell 7.4 的 Env: 提供程序在脚本作用域里**第一次读**可能还是空缓存
#   （profile 用 Set-Item 刚写进去的值要第二次读才可见）。本机实测：直接
#   $idfPy = Join-Path $env:IDF_PATH ... 得到空串 → 子进程 KeyError。
#   先空读一次把缓存刷新，再取。
$null = $env:IDF_PATH
$null = $env:IDF_PYTHON_ENV_PATH
$idfPy = Join-Path $env:IDF_PATH 'tools\idf.py'
$env:_IDF_LAUNCH_TARGET = $idfPy
$prelude = "import os,runpy,sys;" +
           "os.environ.pop('MSYSTEM',None);" +
           "p=os.environ['_IDF_LAUNCH_TARGET'];" +
           "sys.argv[0]=p;" +
           "sys.path.insert(0,os.path.dirname(p));" +
           "runpy.run_path(p,run_name='__main__')"
$py = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
if (-not (Test-Path $py)) { $py = 'python' }
& $py -c $prelude @IdfArgs
exit $LASTEXITCODE
