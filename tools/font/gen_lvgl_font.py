# -*- coding: utf-8 -*-
"""
gen_lvgl_font.py —— 生成 UI 用的 LVGL 位图字体（梦源黑体 GB2312 + LVGL 图标）

为什么需要它
------------
LVGL 内置的 `lv_font_source_han_sans_sc_16_cjk` 只覆盖 **约 1338 个常用汉字**，
GB2312 里其余汉字不在其中，界面上会渲染成"缺字方框"。所以必须自带一份覆盖
GB2312 全集的字体。

为什么不用 FreeType / 分区 TTF
------------------------------
本项目 UI 文本量小、字符集固定，编译期烘成位图字体最省事：
不需要文件系统垫片、不需要运行期解析、不需要额外的 flash 分区与烧录步骤，
渲染路径也与 LVGL 内置字体完全一致。

口径
----
* 汉字 / 全角标点 / ASCII：assets/fonts/DreamHanSansSC-W17-subset.ttf
  （GB2312 全集，由同目录的 gen_subset.py 生成）
* LVGL 图标：lvgl 组件自带的 FontAwesome5。码点**从 lv_symbol_def.h 的注释里
  现场抽取**，保证与当前 LVGL 版本一一对应 —— 漏一个码点就是一个图标变方框。
* 不压缩（--no-compress）：实测 RLE 对 4bpp 抗锯齿汉字几乎没有收益（559 KB vs
  579 KB），但压缩版必须打开 CONFIG_LV_USE_FONT_COMPRESSED，且每次渲染都要解压。
  明文布局还能让 LVGL 直接把 flash 里的位图展开到绘制缓冲，对动画帧率更友好。

产物
----
components/ui_font/ui_font_sc16.c   约 560 KB 常量数据（约 16 px / 4 bpp / 7600 字形）

用法（需已安装 lv_font_conv：npm i -g lv_font_conv；不依赖 Python 第三方库）
    python tools/font/gen_lvgl_font.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SUBSET_TTF = ROOT / "assets" / "fonts" / "DreamHanSansSC-W17-subset.ttf"
LVGL_DIR = ROOT / "managed_components" / "lvgl__lvgl"
FA_WOFF = LVGL_DIR / "scripts" / "built_in_font" / "FontAwesome5-Solid+Brands+Regular.woff"
SYMBOL_HDR = LVGL_DIR / "src" / "font" / "lv_symbol_def.h"
OUT_C = ROOT / "components" / "ui_font" / "ui_font_sc16.c"

SIZE = 16
BPP = 4
FONT_NAME = "ui_font_sc16"


def symbol_codepoints() -> list:
    """从 lv_symbol_def.h 抽取全部 LV_SYMBOL_* 的码点。

    定义形如
        #define LV_SYMBOL_AUDIO  "\\xEF\\x80\\x81" /*61441, 0xF001*/
    UTF-8 转义不好直接解析，而尾注释里同时给了十进制与十六进制，直接取它。
    """
    text = SYMBOL_HDR.read_text(encoding="utf-8", errors="ignore")
    found = re.findall(r"/\*\s*\d+,\s*(0x[0-9A-Fa-f]{4,5})\s*\*/", text)
    if not found:
        sys.exit(f"没能从 {SYMBOL_HDR} 解析出图标码点，LVGL 的注释格式可能变了")
    return sorted({int(v, 16) for v in found})


def lv_font_conv_argv() -> list:
    """拿到可以直接被 subprocess 执行的命令行。

    Windows 上 `lv_font_conv` 只是 npm 生成的 .cmd/.ps1 垫片，CreateProcess
    无法直接执行它们（会报 WinError 2），所以这里定位到真正的 JS 入口用 node 跑。
    """
    node = shutil.which("node")
    roots = []
    npm = shutil.which("npm")
    if npm:
        roots.append(Path(npm).resolve().parent)
    if os.environ.get("APPDATA"):
        roots.append(Path(os.environ["APPDATA"]) / "npm")
    if node:
        for r in roots:
            js = r / "node_modules" / "lv_font_conv" / "lv_font_conv.js"
            if js.exists():
                return [node, str(js)]
    direct = shutil.which("lv_font_conv")
    if direct and not direct.endswith((".cmd", ".ps1", ".bat")):
        return [direct]
    sys.exit("找不到可执行的 lv_font_conv，请先执行：npm i -g lv_font_conv")


def main() -> int:
    for p in (SUBSET_TTF, FA_WOFF, SYMBOL_HDR):
        if not p.exists():
            sys.exit(f"缺少输入文件：{p}")

    syms = [c for c in symbol_codepoints() if c >= 0xF000]
    # 只把私用区（0xF000+）交给 FontAwesome：LVGL 的列表里还有 0x2022(•) 这类
    # 来自 Montserrat 的普通标点，FontAwesome 里没有，交过去 lv_font_conv 会直接报错；
    # 这些标点在汉字字体（GB2312）里本来就有。
    print(f"图标码点 : {len(syms)} 个（来自 {SYMBOL_HDR.name}，私用区）")

    OUT_C.parent.mkdir(parents=True, exist_ok=True)

    cmd = lv_font_conv_argv() + [
        "--no-compress", "--no-prefilter",
        "--bpp", str(BPP),
        "--size", str(SIZE),
        # 汉字侧：subset TTF 已经是 GB2312 全集，这里给一个覆盖整个 BMP 的范围，
        # 字体里没有的码点会被自动跳过
        "--font", str(SUBSET_TTF), "-r", "0x20-0xFFFF",
        # 图标侧：只取 lv_symbol_def.h 里定义的那些码点
        "--font", str(FA_WOFF),
        "-r", ",".join(str(c) for c in syms),
        "--format", "lvgl",
        "--lv-font-name", FONT_NAME,
        "--lv-include", "lvgl.h",
        "--force-fast-kern-format",
        "-o", str(OUT_C),
    ]
    print("运行     : lv_font_conv ...")
    subprocess.run(cmd, check=True)

    text = OUT_C.read_text(encoding="utf-8", errors="ignore")
    data = len(re.findall(r"0x[0-9a-fA-F]{2}", text))
    print(f"输出     : {OUT_C.relative_to(ROOT)}  {data / 1024:.1f} KB 常量数据")
    print(f"           {OUT_C.stat().st_size / 1048576:.1f} MB 源码（编译期解析，之后走增量编译）")
    print("下一步   : 改完后 idf.py build；UI 字体在 ui_core/include/ui_metrics.h 指向它")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())