<#
 * prepare_sources.ps1 —— 把"不是标准 PNG"的原始素材转成 assets/icons/*.png
 *
 * 为什么需要这一步：gen_icon.py 是**纯标准库**的 PNG 解码器（AGENTS.md 第 9 节
 * 禁止全局 pip，tools/font/venv 里只有 brotli/fonttools，没有 PIL）。而
 * MomoTalk 这套素材里有两种它解不了的格式：
 *
 *   assets/default.png   后缀是 .png，**内容其实是 JPEG**（404x456）—— 与
 *                        AGENTS §14.8 记的那条"按魔数认格式，别信扩展名"同一个坑
 *   assets/MomoIcon.png  后缀是 .png，**内容其实是 ICO**（64x64 32bpp）
 *   MomoTalk.html 里那个 ✕ 是**内联 SVG**，本来就不是位图
 *
 * 这里用 Windows 自带的 System.Drawing（.NET）把它们统一转成带 alpha 的 PNG，
 * 之后交给 gen_icon.py 烘成 LVGL 的 C 数组。转换只改像素格式，不改内容。
 *
 * 用法（在仓库根目录执行）：
 *   powershell -ExecutionPolicy Bypass -File tools/icons/prepare_sources.ps1
 *
 * 产物（改素材后重新跑一次即可）：
 *   assets/icons/momotalk_avatar.png   192x192 正方形（原图居中裁方 + 高质量缩放）
 *   assets/icons/momotalk_icon.png      64x64  （ICO 里那张 32bpp 位图）
 *   assets/icons/momotalk_close.png     96x96  （SVG 的 4 倍渲染，白线、圆头、透明底）
#>
param(
    [string]$Root = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Root) {
    $Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
Add-Type -AssemblyName System.Drawing

$outDir = Join-Path $Root 'assets\icons'
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

function Save-Png32([System.Drawing.Bitmap]$bmp, [string]$path) {
    # 统一存成 32bppArgb 的 PNG（保留 alpha），gen_icon.py 认 colortype 2/6
    $dst = New-Object System.Drawing.Bitmap($bmp.Width, $bmp.Height,
                                            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $g.DrawImage($bmp, 0, 0, $bmp.Width, $bmp.Height)
    $g.Dispose()
    $dst.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host ("  -> {0}  {1}x{2}" -f (Split-Path $path -Leaf), $dst.Width, $dst.Height)
    $dst.Dispose()
    $bmp.Dispose()
}

# ---------------------------------------------------------------- 1) 头像
# assets/default.png 实为 JPEG（404x456）：先居中裁成正方形（避免被拉扁），
# 再用高质量插值缩到 192x192，之后由 gen_icon.py 缩到显示尺寸。
$srcPath = Join-Path $Root 'assets\default.png'
$src = [System.Drawing.Image]::FromFile($srcPath)
Write-Host ("头像源 : default.png  {0}x{1}（魔数其实是 JPEG）" -f $src.Width, $src.Height)
$side = [Math]::Min($src.Width, $src.Height)
$tmp = New-Object System.Drawing.Bitmap(192, 192, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($tmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$srcRect = New-Object System.Drawing.Rectangle([int](($src.Width - $side) / 2), [int](($src.Height - $side) / 2), $side, $side)
$dstRect = New-Object System.Drawing.Rectangle(0, 0, 192, 192)
$g.DrawImage($src, $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose()
$src.Dispose()
Save-Png32 $tmp (Join-Path $outDir 'momotalk_avatar.png')

# ---------------------------------------------------------------- 2) 标题栏左侧图标
# assets/MomoIcon.png 实为 ICO（64x64 32bpp）：Icon.ToBitmap() 取出位图。
$icoPath = Join-Path $Root 'assets\MomoIcon.png'
$ico = New-Object System.Drawing.Icon($icoPath)
Write-Host ("图标源 : MomoIcon.png  {0}x{1}（魔数其实是 ICO）" -f $ico.Width, $ico.Height)
$bmp = $ico.ToBitmap()
$ico.Dispose()
Save-Png32 $bmp (Join-Path $outDir 'momotalk_icon.png')

# ---------------------------------------------------------------- 3) 右上角 ✕
# 照 MomoTalk.html 里那段内联 SVG 画：
#   viewBox 0 0 24 24, stroke=currentColor(白), stroke-width 2.5,
#   stroke-linecap round, 路径 M6 18L18 6 M6 6l12 12
# 放大 4 倍（96x96）再交给 gen_icon.py 缩小，缩放时自带抗锯齿，比直接画 32px 干净。
$N = 96
$s = $N / 24.0
$bmp = New-Object System.Drawing.Bitmap($N, $N, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear([System.Drawing.Color]::Transparent)
$pen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, [float](2.5 * $s))
$pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawLine($pen, [float](6 * $s), [float](18 * $s), [float](18 * $s), [float](6 * $s))
$g.DrawLine($pen, [float](6 * $s), [float](6 * $s), [float](18 * $s), [float](18 * $s))
$g.Dispose()
$pen.Dispose()
Write-Host "关闭源 : MomoTalk.html 内联 SVG（✔ 已按 4 倍重绘）"
Save-Png32 $bmp (Join-Path $outDir 'momotalk_close.png')

Write-Host "完成。下一步："
Write-Host "  tools\font\venv\Scripts\python.exe tools\icons\gen_icon.py --manifest assets\icons\icons.txt"
