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
import argparse
import sys
from pathlib import Path

try:
    from fontTools import subset
except ImportError:
    sys.exit("需要 fontTools：请用 tools/font/venv 里的 python 运行，"
             "先执行 pip install fonttools")

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "assets" / "fonts" / "DreamHanSansSC-W17.ttf"
# 输入法词典目录（决定候选词里会出现哪些字；见 dict_charset()）。
#
# ⚠ **不要在这里写死绝对路径** ✗（原来写过 `D:\Codes\lvgl_pinyin_ime\...`，换机器就废 ✗）。
#   默认值按仓库布局推导：本工程用 `override_path: ../../lvgl_pinyin_ime` 引输入法组件 ✓，
#   所以 ROOT 的**同级目录**就是它 ✓。要换位置用命令行参数：
#       python tools/font/gen_subset.py --dict-dir /path/to/lvgl_pinyin_ime/data/dict
DICT_DIR_DEFAULT = ROOT.parent / "lvgl_pinyin_ime" / "data" / "dict"
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


def dict_charset(dict_dir: Path) -> set:
    """从 **lvgl_pinyin_ime 的词典文件**里提取所有需要显示的字符。

    为什么必须带进来：主字体原来只有 GB2312 + JIS，而输入法的候选词来自
    `rawdict_utf16_65105_freq.txt`（google 拼音词库）+ `valid_utf16.txt` ——
    里面含 GB2312/JIS 之外的**生僻字**，缺字形时屏幕上就是方框/空白
    （真机 2026-10 用户报"有些字符不会显示"✗）。

    ⚠ 编码坑（我踩过 ✗）：这两个文件是 **UTF-16**（另有一个 `.be` 变体是**大端**），
      而且每行的后半段（拼音/词频）是 **GBK 字节** ✗ ——
      所以这里用 `utf-16-le` + `errors="ignore"` 容错解码，再**只收 CJK 区**的字符，
      拼音/数字/控制字节自然被过滤掉 ✓。
    """
    chars = set()
    # 用户自定义补充表（同目录下的 extra_chars.txt）：里面写什么就收什么 ✓
    extra = dict_dir / "extra_chars.txt"
    if extra.exists():
        txt = extra.read_text(encoding="utf-8", errors="ignore")
        before = len(chars)
        for line in txt.splitlines():
            line = line.split("#", 1)[0]          # `#` 起是注释
            for ch in line:
                if not ch.isspace():              # 空白本来就有字形，不用收
                    chars.add(ch)
        print(f"  {extra.name:38s} {extra.stat().st_size / 1024:8.1f} KB → 补充 {len(chars) - before} 字（自定义）")
    files = [
        dict_dir / "rawdict_utf16_65105_freq.txt",
        dict_dir / "valid_utf16.txt",
    ]
    for f in files:
        if not f.exists():
            print(f"  ⚠ 词典不存在，跳过：{f}")
            continue
        raw = f.read_bytes()
        text = raw.decode("utf-16-le", errors="ignore")
        got = 0
        for ch in text:
            o = ord(ch)
            if (0x3400 <= o <= 0x9FFF) or (0xF900 <= o <= 0xFAFF) or (0x20000 <= o <= 0x2FA1F):
                if ch not in chars:
                    chars.add(ch)
            got += 0
        print(f"  {f.name:38s} {f.stat().st_size / 1024:8.1f} KB → 累计 CJK {len(chars)} 字")
    return chars


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
    ap = argparse.ArgumentParser(
        description="为 LVGL 位图字体切子集 TTF（主字体 / 日文补充 / 输入法词典生僻字）")
    ap.add_argument("--dict-dir", type=Path, default=DICT_DIR_DEFAULT,
                    help=("lvgl_pinyin_ime 的 data/dict 目录（默认按仓库布局推导："
                          f"{DICT_DIR_DEFAULT}）；同目录下的 extra_chars.txt 是用户"
                          "自定义补充字符表，会被一并读入"))
    ap.add_argument("--no-dict", action="store_true",
                    help="不并入输入法词典里的生僻字（只切 GB2312 + 日文）")
    args = ap.parse_args()

    if not SRC.exists():
        sys.exit(f"找不到源字体：{SRC}")

    print(f"源字体 : {SRC.name}  {SRC.stat().st_size / 1048576:.2f} MB")
    DST.parent.mkdir(parents=True, exist_ok=True)

    gb = gb2312_charset()
    print(f"\n[1/3] 主字体（GB2312 + ASCII + 符号）")
    print("  从输入法词典补字（生僻字：候选词里会出现，原来会显示成方框 ✗）：")
    if not args.no_dict:
        gb |= dict_charset(args.dict_dir)
    print(f"  合并后 {len(gb)} 个字符")
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
