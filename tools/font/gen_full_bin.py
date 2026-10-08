# -*- coding: utf-8 -*-
"""
gen_full_bin.py —— 生成**全量** UI 字体的 LVGL 二进制字体（.bin）

与 gen_lvgl_font.py 的区别（那条是"GB2312 子集 + 图标"编进 app 的老路）：
  - 字体源用**完整**的 DreamHanSansSC-W17.ttf（实测 cmap 覆盖 44812 个码点，
    含假名、日文汉字、韩文谚文、符号），范围直接给 0x20-0xFFFF，字体里没有的
    码点 lv_font_conv 会自动跳过；
  - 输出 --format bin（给 lv_binfont_create / create_from_buffer 用），
    配合 `font` 分区（partitions.csv）在运行时加载，不再编进 app；
  - 依然合并 FontAwesome 图标（LVGL 的 LV_SYMBOL_* 全在私用区）。

用法（在工程根目录）：
    python tools/font/gen_full_bin.py
产物：
    assets/fonts/ui16_full.bin     ← 用 tools/font/flash_font.ps1 写进 font 分区
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FULL_TTF = ROOT / "assets" / "fonts" / "DreamHanSansSC-W17.ttf"
FA_WOFF = next(iter((ROOT / "managed_components").glob("lvgl__lvgl/scripts/built_in_font/FontAwesome*.woff")), None)
SYMBOL_HDR = next(iter((ROOT / "managed_components").glob("lvgl__lvgl/src/font/lv_symbol_def.h")), None)
OUT_BIN = ROOT / "assets" / "fonts" / "ui16_full.bin"

BPP = 4
SIZE = 16


def lv_font_conv_argv():
    """定位真正的 JS 入口（Windows 上 npm 的 .cmd 垫片 CreateProcess 跑不了）。"""
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
    sys.exit("找不到 lv_font_conv，请先 npm i -g lv_font_conv")


def icon_codepoints():
    """从 LVGL 的 lv_symbol_def.h 里抠出图标码点（只要私用区，普通标点汉字字体里有）。"""
    text = SYMBOL_HDR.read_text(encoding="utf-8", errors="ignore")
    found = re.findall(r"/\*\s*\d+,\s*(0x[0-9A-Fa-f]{4,5})\s*\*/", text)
    return sorted({int(v, 16) for v in found if int(v, 16) >= 0xF000})


def main() -> int:
    for p in (FULL_TTF, FA_WOFF, SYMBOL_HDR):
        if p is None or not Path(p).exists():
            sys.exit(f"缺少输入文件：{p}")

    icons = icon_codepoints()
    print(f"字体源   : {FULL_TTF.name}（全量，0x20-0xFFFF）")
    print(f"图标源   : {FA_WOFF.name}，{len(icons)} 个私用区码点")
    OUT_BIN.parent.mkdir(parents=True, exist_ok=True)

    cmd = lv_font_conv_argv() + [
        "--no-compress", "--no-prefilter",
        "--bpp", str(BPP), "--size", str(SIZE),
        "--font", str(FULL_TTF), "-r", "0x20-0xFFFF",
        "--font", str(FA_WOFF), "-r", ",".join(str(c) for c in icons),
        "--format", "bin",
        "-o", str(OUT_BIN),
    ]
    print("运行     : lv_font_conv ... （全量字形，可能要几分钟）")
    subprocess.run(cmd, check=True)

    size = OUT_BIN.stat().st_size
    print(f"输出     : {OUT_BIN.relative_to(ROOT)}  {size / 1048576:.2f} MB ({size} 字节)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
