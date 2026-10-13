# -*- coding: utf-8 -*-
"""
gen_lvgl_font.py —— 生成 UI 用的 LVGL 位图字体（中文主字体 + 日文汉字补充）

为什么需要它
------------
LVGL 内置的 `lv_font_source_han_sans_sc_16_cjk` 只覆盖 **约 1338 个常用汉字**，
GB2312 里其余汉字不在其中，界面上会渲染成"缺字方框"。所以必须自带一份覆盖
GB2312 全集的字体。

日文汉字为什么还要单独一份
--------------------------
GB2312 是**简体中文**字符集，日文汉字里有大量字形/码位都不在其中的字
（典型：鎖 U+9396、転 U+8EE2 —— 中文对应的是 锁 / 转，码位不同），
于是"鎖那 - 夏休みの転校生"这类日文曲名会缺字成方框。
LVGL 9 的 `lv_font_t.fallback` 支持"本字体找不到字形就去备用字体找"（递归），
所以这里生成两份**只含增量**的日文补充字体，挂在主字体后面：

    ui_font_sc16    (GB2312 + ASCII + 图标, 4 bpp)    ← 中文/UI，一点没动
      └─ ui_font_jp1_16 (JIS 第 1 水準 + 日文记号,  4 bpp)  ← 常用日文汉字
           └─ ui_font_jp2_16 (JIS 第 2 水準,        2 bpp)  ← 生僻日文汉字

为什么 jp2 用 2 bpp：app 分区只剩 ~335 KB，日文补充合计要压进这个额度。
4 bpp 的 jp2 会超（见 gen_subset.py 的说明）；生僻字降到 2 bpp 肉眼几乎无感，
而中文主字体与常用日文汉字仍是 4 bpp。

为什么不压缩（--no-compress）
-----------------------------
实测 RLE 对 4 bpp 抗锯齿汉字收益很小，但压缩版必须打开
CONFIG_LV_USE_FONT_COMPRESSED，且每次渲染都要解压。明文布局还能让 LVGL 直接把
flash 里的位图展开到绘制缓冲，对动画帧率更友好。
**另外：LVGL 的"未压缩"路径只实现了 1/2/4/8 bpp（见 lv_font_fmt_txt.c 的
LV_FONT_FMT_TXT_PLAIN 分支），3 bpp 只有压缩路径才有 —— 所以这里绝不能给
--bpp 3，否则字形会整片空白。**

产物
----
components/ui_font/ui_font_sc16.c    中文主字体（约 950 KB 常量数据）
components/ui_font/ui_font_jp1_16.c  日文常用补充
components/ui_font/ui_font_jp2_16.c  日文生僻补充

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
FONTS_DIR = ROOT / "assets" / "fonts"
SUBSET_TTF = FONTS_DIR / "DreamHanSansSC-W17-subset.ttf"
JP1_TTF = FONTS_DIR / "DreamHanSansSC-W17-jp1.ttf"
JP2_TTF = FONTS_DIR / "DreamHanSansSC-W17-jp2.ttf"
DICT_TTF = FONTS_DIR / "DreamHanSansSC-W17-dict.ttf"   # 生僻字 + extra_chars（2bpp）
LVGL_DIR = ROOT / "managed_components" / "lvgl__lvgl"
FA_WOFF = LVGL_DIR / "scripts" / "built_in_font" / "FontAwesome5-Solid+Brands+Regular.woff"
SYMBOL_HDR = LVGL_DIR / "src" / "font" / "lv_symbol_def.h"
OUT_DIR = ROOT / "components" / "ui_font"

SIZE = 16
# 每个产物：(子集 TTF, 输出 .c, LVGL 字体名, bpp, 是否并图标)
SPECS = [
    (SUBSET_TTF, "ui_font_sc16.c",   "ui_font_sc16",   4, True),
    (JP1_TTF,    "ui_font_jp1_16.c", "ui_font_jp1_16", 4, False),
    (JP2_TTF,    "ui_font_jp2_16.c", "ui_font_jp2_16", 2, False),
    # ★ 生僻字/补充表：2bpp（同 jp2 的理由——省一半体积；LVGL 未压缩路径支持 2bpp）
    (DICT_TTF,   "ui_font_dict16.c", "ui_font_dict16", 2, False),
]


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


def build_one(conv: list, ttf: Path, out_name: str, font_name: str, bpp: int,
              with_icons: bool, syms: list) -> None:
    out_c = OUT_DIR / out_name
    cmd = conv + [
        "--no-compress", "--no-prefilter",
        "--bpp", str(bpp),
        "--size", str(SIZE),
        # 汉字侧：子集 TTF 里已经只含需要的字符，这里给一个覆盖整个 BMP 的范围，
        # 字体里没有的码点会被自动跳过
        "--font", str(ttf), "-r", "0x20-0xFFFF",
    ]
    if with_icons:
        # 图标侧：只取 lv_symbol_def.h 里定义的那些码点（私用区，0xF000+）
        cmd += ["--font", str(FA_WOFF), "-r", ",".join(str(c) for c in syms)]
    cmd += [
        "--format", "lvgl",
        "--lv-font-name", font_name,
        "--lv-include", "lvgl.h",
        "--force-fast-kern-format",
        "-o", str(out_c),
    ]
    subprocess.run(cmd, check=True)

    text = out_c.read_text(encoding="utf-8", errors="ignore")
    glyphs = len(re.findall(r"/\* U\+", text))
    print(f"  {out_name:22s} {bpp} bpp  {glyphs:5d} 字形  "
          f"源码 {out_c.stat().st_size / 1048576:.1f} MB")
    return glyphs


def main() -> int:
    for p in (SUBSET_TTF, JP1_TTF, JP2_TTF, FA_WOFF, SYMBOL_HDR):
        if not p.exists():
            sys.exit(f"缺少输入文件：{p}\n"
                     f"  （子集字体由 tools/font/gen_subset.py 生成）")

    syms = [c for c in symbol_codepoints() if c >= 0xF000]
    # 只把私用区（0xF000+）交给 FontAwesome：LVGL 的列表里还有 0x2022(•) 这类
    # 来自 Montserrat 的普通标点，FontAwesome 里没有，交过去 lv_font_conv 会直接报错；
    # 这些标点在汉字字体（GB2312）里本来就有。
    print(f"图标码点 : {len(syms)} 个（来自 {SYMBOL_HDR.name}，私用区）")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    conv = lv_font_conv_argv()

    total = 0
    print("生成     : lv_font_conv ...")
    for ttf, out_name, font_name, bpp, with_icons in SPECS:
        total += build_one(conv, ttf, out_name, font_name, bpp, with_icons, syms)

    print(f"\n合计     : {total} 个字形")
    # ★ 别用"数 0xNN 字面量"来估 flash —— 字形位图里还有大批十进制描述符，
    #   那样会少算约 40%（实测 559 KB vs 真实 976 KB）。真实占用一律这样量：
    #     riscv32-esp-elf-size build/esp-idf/ui_font/libui_font.a
    print("          真实 flash 占用用 riscv32-esp-elf-size 量 "
          "build/esp-idf/ui_font/libui_font.a")
    print("下一步   : idf.py build；UI 字体在 ui_core/include/ui_metrics.h 指向")
    print("           components/ui_font/ui_font.c 里的 fallback 包装字体")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
