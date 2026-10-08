# -*- coding: utf-8 -*-
"""
gen_subset.py —— 把 DreamHanSansSC-W17.ttf 子集化为"项目够用"的几个字体

为什么不能全量：DreamHanSansSC-W17.ttf 约 22 MB，整片 Flash 只有 16 MB，
无论做成 C 数组 / 分区 / 文件系统都放不下。因此按实际字符集裁剪。

产物（三个子集，对应三个 LVGL 位图字体，见 gen_lvgl_font.py）
------------------------------------------------------------------
1. DreamHanSansSC-W17-subset.ttf   中文/UI 主字体（4 bpp）
   * GB2312 全集（6763 汉字 + 全角标点 + 拉丁 + 希腊 + 西里尔 + 数字全角），
     由 Python 的 gb2312 编解码表反推，保证与系统输入法口径一致；
   * ASCII 可见字符 0x20~0x7E；
   * 少量 UI 常用但不在 GB2312 的符号（如 ° ℃ × ÷ 等已在 GB2312 内，此处补 · 等）。

2. DreamHanSansSC-W17-jp1.ttf      日文"常用"补充（4 bpp）
   * JIS X 0208 **第 1 水準**汉字（区 16~47，2965 字，覆盖全部常用汉字/常用漢字）；
   * JIS X 0208 区 1~8 的记号/假名里 GB2312 **没有**的那些
     （典型：々 U+3005 叠字符、〜 U+301C 波浪线 —— 日文标题里很常见）；
   * 减去 GB2312 已有的部分，只留增量（不重复占体积）。

3. DreamHanSansSC-W17-jp2.ttf      日文"生僻"补充（2 bpp）
   * JIS X 0208 **第 2 水準**汉字（区 48~84，3390 字，人名/地名里的生僻字，
     如 凪 雫 栞 等），同样减去 GB2312。

为什么要分三个、而不是合成一个大字体
------------------------------------
app 分区只剩 ~335 KB。合并成一个 4 bpp 大字体需要 +384 KB（放不下）；
而降成 2 bpp 又会让**整套中文和拉丁字母**一起变糙。
分成"主字体 + 日文补充"后：
  * 中文/UI 文字保持原来的 4 bpp 质量（一点没动）；
  * 日文**常用**字也是 4 bpp，和旁边的中文字形观感一致；
  * 只有日文**生僻**字降到 2 bpp（本来就极少见，且找不到同排的 4 bpp 邻居对比）。
三条按 `fallback` 串起来（见 components/ui_font/ui_font.c）。

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
DST_JP1 = ROOT / "assets" / "fonts" / "DreamHanSansSC-W17-jp1.ttf"
DST_JP2 = ROOT / "assets" / "fonts" / "DreamHanSansSC-W17-jp2.ttf"


def gb2312_charset() -> set:
    """GB2312 全集 + ASCII + 少量补充符号（主字体的口径）。"""
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

    return chars


def euc_jp_rows(hi_from: int, hi_to: int) -> set:
    """EUC-JP 的 hi/lo 两字节区间 → JIS X 0208 对应区的字符集合。

    JIS X 0208 的行与 EUC-JP 高字节一一对应（行 n ↔ 0xA0 + n）：
      区 1~8   → 0xA1~0xA8  记号 / 假名 / 拉丁 / 希腊 / 西里尔
      区 16~47 → 0xB0~0xCF  第 1 水準汉字
      区 48~84 → 0xD0~0xF4  第 2 水準汉字
    """
    chars = set()
    for hi in range(hi_from, hi_to + 1):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("euc_jp"))
            except UnicodeDecodeError:
                pass
    return chars


def jis_symbols() -> set:
    """JIS X 0208 区 1~8：记号、平假名、片假名、拉丁、希腊、西里尔。"""
    return euc_jp_rows(0xA1, 0xA8)


def jis_level1() -> set:
    """JIS X 0208 第 1 水準汉字（区 16~47）。"""
    return euc_jp_rows(0xB0, 0xCF)


def jis_level2() -> set:
    """JIS X 0208 第 2 水準汉字（区 48~84）。"""
    return euc_jp_rows(0xD0, 0xF4)


def make_subset(chars: set, dst: Path) -> int:
    """按字符集生成一个子集字体，返回字节数。"""
    text = "".join(sorted(chars))
    args = [
        str(SRC),
        f"--text={text}",
        "--output-file=" + str(dst),
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
    size = dst.stat().st_size
    print(f"  输出 {dst.name:38s} {len(chars):5d} 字  {size / 1024:8.1f} KB")
    return size


def main() -> int:
    if not SRC.exists():
        sys.exit(f"找不到源字体：{SRC}")

    print(f"源字体 : {SRC.name}  {SRC.stat().st_size / 1048576:.2f} MB")
    DST.parent.mkdir(parents=True, exist_ok=True)

    gb = gb2312_charset()
    print(f"\n[1/3] 主字体（GB2312 + ASCII + 符号）：{len(gb)} 个字符")
    make_subset(gb, DST)

    # 日文补充：只留 GB2312 没有的部分（增量），避免和主字体重复占体积
    jp1 = (jis_symbols() | jis_level1()) - gb
    print(f"\n[2/3] 日文常用补充（JIS 区 1~8 记号 + 第 1 水準，减去 GB2312）：{len(jp1)} 个字符")
    make_subset(jp1, DST_JP1)

    jp2 = jis_level2() - gb - jp1
    print(f"\n[3/3] 日文生僻补充（JIS 第 2 水準，减去 GB2312）：{len(jp2)} 个字符")
    make_subset(jp2, DST_JP2)

    print("\n下一步 : 运行 tools/font/gen_lvgl_font.py 生成三个 LVGL 字体")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
