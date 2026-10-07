# -*- coding: utf-8 -*-
"""
gen_subset.py —— 把 DreamHanSansSC-W17.ttf 子集化为"项目够用"的字体

为什么不能全量：DreamHanSansSC-W17.ttf 约 22 MB，整片 Flash 只有 16 MB，
无论做成 C 数组 / 分区 / 文件系统都放不下。因此按实际字符集裁剪。

字符集口径：
  * GB2312 全集（6763 汉字 + 全角标点 + 拉丁 + 希腊 + 西里尔 + 数字全角），
    由 Python 的 gb2312 编解码表反推，保证与系统输入法口径一致；
  * ASCII 可见字符 0x20~0x7E；
  * 少量 UI 常用但不在 GB2312 的符号（如 ° ℃ × ÷ 等已在 GB2312 内，此处补 · 等）。

用法（务必用本工程 tools/font/venv，禁止全局 pip）：
    tools\\font\\venv\\Scripts\\python.exe tools\\font\\gen_subset.py
"""
import sys
from pathlib import Path

try:
    from fontTools import subset
except ImportError:
    sys.exit("需要 fontTools：请用 tools/font/venv 里的 python 运行，"
             "先执行 pip install fonttools")

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "assets" / "fonts" / "DreamHanSansSC-W17.ttf"
DST = ROOT / "assets" / "fonts" / "DreamHanSansSC-W17-subset.ttf"


def build_charset() -> str:
    chars = set()

    # GB2312：区 0xA1~0xF7，位 0xA1~0xFE
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass

    # ASCII 可见字符（含空格与标点）
    for c in range(0x20, 0x7F):
        chars.add(chr(c))

    # 几个 UI 里会用到的补充符号（GB2312 未覆盖的）
    chars.update("·—…“”‘’《》〈〉【】、。，！？；：（）％＋－／")

    return "".join(sorted(chars))


def main() -> int:
    if not SRC.exists():
        sys.exit(f"找不到源字体：{SRC}")

    text = build_charset()
    print(f"源字体 : {SRC.name}  {SRC.stat().st_size / 1048576:.2f} MB")
    print(f"字符集 : {len(text)} 个字符")

    DST.parent.mkdir(parents=True, exist_ok=True)

    args = [
        str(SRC),
        f"--text={text}",
        "--output-file=" + str(DST),
        "--layout-features=",          # 去掉 GSUB/GPOS 特性，省体积
        "--no-hinting",
        "--recalc-bounds",
        # DreamHanSansSC-W17 是**可变字体**（22 MB 的主因）。FreeType/LVGL 只需要
        # 一个静态字面，保留变体表既没意义又会把体积撑大，且 subset 解析
        # VarStore 会报 "unpack requires a buffer of 2 bytes"。
        # 一次性丢掉全部变体相关表，退化成默认实例的静态字体。
        "--drop-tables+=DSIG,"
        "fvar,gvar,avar,cvar,HVAR,VVAR,MVAR,STAT,"
        "BASE,GDEF,GPOS,GSUB,MATH",
    ]
    subset.main(args)

    size = DST.stat().st_size
    print(f"输出   : {DST.name}  {size / 1048576:.2f} MB  ({size} 字节)")
    # 分区按 4KB 对齐向上取整
    print(f"建议分区大小: 0x{(size + 4095) // 4096 * 4096:X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())