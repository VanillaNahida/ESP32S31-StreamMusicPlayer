# -*- coding: utf-8 -*-
"""
pack_fonts.py —— 把 lv_font_conv 生成的 LVGL 字体 .c 打包成 fonts 分区镜像

为什么不再把字体编进 app
------------------------
五个位图字体合计约 2.73 MB 常量数据，全塞在 app 里会把 12.31 MB 的 app 槽
挤到只剩几十 KB（搬走后 app 实测 9.31 MB）。位图字体是**只读、按字形随机访问**的数据，天生适合放进
独立的 flash 分区，用 esp_partition_mmap() 映射成只读指针 ——

  * app 槽里不再有字形位图（app 从 ~12.2 MB 降到 ~9.5 MB）；
  * 运行时**不复制**字体数据到 RAM：LVGL 的 glyph_bitmap / glyph_dsc 直接
    指在 flash 映射区（只额外占几 KB 的 cmap 表）；
  * 换字体/加字只重烧 fonts 分区，不用重编整个 app。

为什么要转成自定义 blob，而不是直接用 lv_binfont_create()
----------------------------------------------------------
LVGL 自带的 .bin 字体加载器（lv_binfont_create_from_buffer）会把整个字体
**解析并复制到堆**里（glyph_bitmap / glyph_dsc / cmaps 全 malloc），既花
~2.7 MB PSRAM，又要几百毫秒解析 —— 和"mmap 直接用"的目标相反。

这里把 .c 里的数据原样抽出来，按 LVGL 运行时结构排成可直接读取的表：

    blob header (20 B) + 5 × 目录项 (60 B)
      └─ 每个字体：段头(16 B) + cmap 描述表 + glyph_dsc[] + unicode 表 + 位图

运行期只 malloc 出 lv_font_t / lv_font_fmt_txt_dsc_t / cmap 结构（几 KB），
指针全部指向 mmap 区。blob 里每个表都按 4/16 字节对齐，RISC-V 上不会出现
非对齐访问。

⚠ 依赖 LVGL 的编译配置：CONFIG_LV_FONT_FMT_TXT_LARGE
----------------------------------------------------
lv_font_fmt_txt_glyph_dsc_t 在 LARGE=y 时是 16 字节（bitmap_index/adv_w 32 位，
box/ofs 16 位），=n 时是 8 字节紧凑布局。blob 头里记了 glyph_dsc_size，
固件启动时会和 sizeof() 比对，不一致直接报错（不会画成乱码）。

用法（纯标准库，不需要 venv；在工程根目录）：
    python tools/font/pack_fonts.py            # 生成 build/fonts.bin
    python tools/font/pack_fonts.py --verify   # 生成后再自检一遍
    python tools/font/pack_fonts.py --large    # 手工指定 LARGE 布局（默认读 sdkconfig）

产物：build/fonts.bin（烧到 fonts 分区，见 partitions.csv）。
CMake 在构建时自动调用本脚本，并把它注册到 `idf.py flash`（components/ui_font/CMakeLists.txt）。
"""

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT / "components" / "ui_font"
# 产物放 build/：它是生成物（不签入仓库），也避开某些机器上 assets/ 的写权限限制。
# CMake 会把它作为自定义命令的 OUTPUT 自动生成（见 components/ui_font/CMakeLists.txt）。
OUT_BIN = ROOT / "build" / "fonts.bin"

# 字体在 blob 里的顺序/名字。名字是运行期查找用的稳定标识，改名字要同步改 ui_font.c。
SPECS = [
    ("sc16",   "ui_font_sc16.c"),
    ("sc22",   "ui_font_sc22.c"),
    ("jp1",    "ui_font_jp1_16.c"),
    ("jp2",    "ui_font_jp2_16.c"),
    ("dict16", "ui_font_dict16.c"),
]

MAGIC = b"UIFP"
VERSION = 1

# 头：magic/version/font_count/blob_size/crc32/glyph_dsc_size
HDR = struct.Struct("<4sHHIII")
# 目录项：name[16] + 11 个 32 位字段
ENT = struct.Struct("<16sIIIIIiiiiII")
# 字体段头：cmaps_off / glyph_dsc_off / bitmap_off / reserved
SEC = struct.Struct("<IIII")
# cmap 描述：range_start / unicode_list_off / glyph_id_ofs_off
#            + range_length / glyph_id_start / list_length / type / pad
CMAP = struct.Struct("<IIIHHHBB")

CMAP_TYPES = {
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL": 0,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL": 1,
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY": 2,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY": 3,
}
SUBPX = {
    "LV_FONT_SUBPX_NONE": 0,
    "LV_FONT_SUBPX_HOR": 1,
    "LV_FONT_SUBPX_VER": 2,
    "LV_FONT_SUBPX_BOTH": 3,
}

ALIGN_DSC = 16     # lv_font_fmt_txt_glyph_dsc_t 含 32/64 位字段
ALIGN_LIST = 2     # unicode_list / glyph_id_ofs_list 是 uint16/uint8
ALIGN_BITMAP = 4


def align_up(n, a):
    return (n + a - 1) // a * a


class FontError(RuntimeError):
    pass


def _strip_c_comments(text):
    # 生成文件里的注释只有 /* ... */（字形位图里会夹 "U+4E2D "中"" 这类注释，
    # 里面的数字/十六进制会干扰数字提取，必须先删掉）
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def _parse_int(tok):
    return int(tok, 0)


def _parse_fields(body):
    """把 '{.a = 1, .b = -2}' 里的字段解析成 dict。"""
    out = {}
    for m in re.finditer(r"\.(\w+)\s*=\s*([^,}]+)", body):
        out[m.group(1)] = m.group(2).strip()
    return out


def _parse_arrays(text):
    """抽出 unicode_list_N / glyph_id_ofs_list_N 这些 uint8/uint16 数组。"""
    arrays = {}
    for m in re.finditer(
        r"static const uint(8|16)_t\s+(\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S
    ):
        width = int(m.group(1))
        name = m.group(2)
        body = _strip_c_comments(m.group(3))
        nums = re.findall(r"0[xX][0-9A-Fa-f]+|\d+", body)
        fmt = "<%d%s" % (len(nums), "B" if width == 8 else "H")
        arrays[name] = (width, struct.pack(fmt, *[int(n, 0) for n in nums]))
    return arrays


def parse_font_c(path, large):
    """解析 lv_font_conv 生成的 .c，返回可直接打包的字段。"""
    if not path.exists():
        raise FontError("缺少字体源：%s" % path)
    text = path.read_text(encoding="utf-8", errors="ignore")

    # ---- glyph bitmap ----
    m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise FontError("%s: 找不到 glyph_bitmap[]" % path.name)
    body = _strip_c_comments(m.group(1))
    nums = re.findall(r"0[xX][0-9A-Fa-f]+|\d+", body)
    bitmap = bytes(int(n, 0) & 0xFF for n in nums)

    # ---- glyph descriptors ----
    m = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise FontError("%s: 找不到 glyph_dsc[]" % path.name)
    entries = re.findall(r"\{([^{}]*)\}", m.group(1))
    if not entries:
        raise FontError("%s: glyph_dsc[] 为空" % path.name)
    glyphs = []
    for e in entries:
        f = _parse_fields(e)
        try:
            glyphs.append({
                "bitmap_index": _parse_int(f["bitmap_index"]),
                "adv_w": _parse_int(f["adv_w"]),
                "box_w": _parse_int(f["box_w"]),
                "box_h": _parse_int(f["box_h"]),
                "ofs_x": _parse_int(f["ofs_x"]),
                "ofs_y": _parse_int(f["ofs_y"]),
            })
        except KeyError as ex:
            raise FontError("%s: glyph_dsc 缺字段 %s" % (path.name, ex))
    if glyphs[0]["bitmap_index"] != 0 or glyphs[0]["box_w"] != 0:
        raise FontError("%s: glyph 0 不是保留字形" % path.name)

    # ---- cmaps ----
    m = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise FontError("%s: 找不到 cmaps[]" % path.name)
    cmap_entries = re.findall(r"\{([^{}]*)\}", m.group(1))
    cmaps = []
    for e in cmap_entries:
        f = _parse_fields(e)
        typ = f.get("type", "").strip()
        if typ not in CMAP_TYPES:
            raise FontError("%s: 不认识的 cmap type %r" % (path.name, typ))
        cmaps.append({
            "range_start": _parse_int(f["range_start"]),
            "range_length": _parse_int(f["range_length"]),
            "glyph_id_start": _parse_int(f["glyph_id_start"]),
            "unicode_list": None if f.get("unicode_list", "NULL").strip() == "NULL"
                            else f["unicode_list"].strip(),
            "glyph_id_ofs_list": None if f.get("glyph_id_ofs_list", "NULL").strip() == "NULL"
                                 else f["glyph_id_ofs_list"].strip(),
            "list_length": _parse_int(f.get("list_length", "0")),
            "type": CMAP_TYPES[typ],
            "type_name": typ,
        })

    # ---- font_dsc ----
    m = re.search(r"lv_font_fmt_txt_dsc_t\s+font_dsc\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise FontError("%s: 找不到 font_dsc" % path.name)
    f = _parse_fields(m.group(1))
    bpp = _parse_int(f["bpp"])
    cmap_num = _parse_int(f["cmap_num"])
    bitmap_format = _parse_int(f.get("bitmap_format", "0"))
    kern_classes = _parse_int(f.get("kern_classes", "0"))
    kern_dsc = f.get("kern_dsc", "NULL").strip()
    if kern_dsc != "NULL" or kern_classes != 0:
        raise FontError("%s: 带 kerning 的字体暂不支持（kern_dsc=%s）" % (path.name, kern_dsc))
    if bitmap_format != 0:
        raise FontError("%s: 只支持未压缩位图（bitmap_format=%d）" % (path.name, bitmap_format))
    if cmap_num != len(cmaps):
        raise FontError("%s: cmap_num=%d 与 cmaps[] 条目 %d 不一致"
                        % (path.name, cmap_num, len(cmaps)))

    # ---- 公共 lv_font_t ----
    m = re.search(r"const lv_font_t\s+\w+\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise FontError("%s: 找不到 lv_font_t 定义" % path.name)
    f = _parse_fields(m.group(1))
    subpx_name = f.get("subpx", "LV_FONT_SUBPX_NONE").strip()
    if subpx_name not in SUBPX:
        raise FontError("%s: 不认识的 subpx %r" % (path.name, subpx_name))

    # ---- 数组 ----
    arrays = _parse_arrays(text)

    # ---- 校验 cmap 引用的数组 ----
    for c in cmaps:
        ul = c["unicode_list"]
        gf = c["glyph_id_ofs_list"]
        if ul is not None:
            if ul not in arrays or arrays[ul][0] != 16:
                raise FontError("%s: cmap 引用了不存在的 unicode_list %s" % (path.name, ul))
            n = len(arrays[ul][1]) // 2
            if n != c["list_length"]:
                raise FontError("%s: %s 长度 %d != list_length %d"
                                % (path.name, ul, n, c["list_length"]))
        if gf is not None:
            if gf not in arrays:
                raise FontError("%s: cmap 引用了不存在的 glyph_id_ofs_list %s" % (path.name, gf))
            width = arrays[gf][0]
            expect = 8 if c["type"] in (0, 2) else 16
            if width != expect:
                raise FontError("%s: %s 宽度 %d != 期望 %d"
                                % (path.name, gf, width, expect))
            n = len(arrays[gf][1]) // (1 if width == 8 else 2)
            if n != c["list_length"]:
                raise FontError("%s: %s 长度 %d != list_length %d"
                                % (path.name, gf, n, c["list_length"]))
        if c["type"] == 2 and c["list_length"] != 0:
            raise FontError("%s: FORMAT0_TINY cmap 的 list_length 应为 0" % path.name)
        if c["type"] == 0 and c["list_length"] != c["range_length"]:
            raise FontError("%s: FORMAT0_FULL 的 list_length 应等于 range_length" % path.name)
        if c["type"] in (0, 2) and c["unicode_list"] is not None:
            raise FontError("%s: FORMAT0 cmap 不应有 unicode_list" % path.name)
        if c["type"] in (1, 3) and c["unicode_list"] is None:
            raise FontError("%s: SPARSE cmap 缺少 unicode_list" % path.name)

    # ---- 校验字形位图跨度 ----
    # lv_font_conv 的明文位图是逐字形连续打包（无行对齐），每个字形占
    # ceil(w*h*bpp/8) 字节；bitmap_index 就是它的起始偏移。
    for i, g in enumerate(glyphs):
        if g["box_w"] and g["box_h"]:
            nbytes = (g["box_w"] * g["box_h"] * bpp + 7) // 8
        else:
            nbytes = 0
        nxt = glyphs[i + 1]["bitmap_index"] if i + 1 < len(glyphs) else len(bitmap)
        if g["bitmap_index"] + nbytes != nxt:
            raise FontError(
                "%s: 字形 %d 位图跨度对不上（index=%d + %d != %d）"
                % (path.name, i, g["bitmap_index"], nbytes, nxt))

    if not large:
        for i, g in enumerate(glyphs):
            if not (0 <= g["bitmap_index"] < (1 << 20)):
                raise FontError("%s: 字形 %d bitmap_index 超出 20 位" % (path.name, i))
            if not (0 <= g["adv_w"] < (1 << 12)):
                raise FontError("%s: 字形 %d adv_w 超出 12 位（需要 --large）" % (path.name, i))
            if not (0 <= g["box_w"] <= 255 and 0 <= g["box_h"] <= 255):
                raise FontError("%s: 字形 %d box 超出 8 位" % (path.name, i))
            if not (-128 <= g["ofs_x"] <= 127 and -128 <= g["ofs_y"] <= 127):
                raise FontError("%s: 字形 %d ofs 超出 8 位" % (path.name, i))

    return {
        "name": path.name,
        "bpp": bpp,
        "subpx": SUBPX[subpx_name],
        "line_height": _parse_int(f["line_height"]),
        "base_line": _parse_int(f["base_line"]),
        "underline_position": _parse_int(f.get("underline_position", "0")),
        "underline_thickness": _parse_int(f.get("underline_thickness", "0")),
        "glyphs": glyphs,
        "bitmap": bitmap,
        "cmaps": cmaps,
        "arrays": arrays,
        "large": large,
    }


def pack_glyph_dsc(glyphs, bpp, large):
    out = bytearray()
    for g in glyphs:
        if large:
            out += struct.pack("<IIHHhh", g["bitmap_index"], g["adv_w"],
                               g["box_w"], g["box_h"], g["ofs_x"], g["ofs_y"])
        else:
            word = (g["bitmap_index"] & 0xFFFFF) | ((g["adv_w"] & 0xFFF) << 20)
            out += struct.pack("<I", word)
            out += struct.pack("<BBbb", g["box_w"], g["box_h"], g["ofs_x"], g["ofs_y"])
    return bytes(out)


def build_font_section(font, large):
    """按"段头 + cmap 表 + glyph_dsc + 列表 + 位图"排布，返回 (section_bytes, info)。"""
    glyph_dsc = pack_glyph_dsc(font["glyphs"], font["bpp"], large)
    cmaps = font["cmaps"]
    arrays = font["arrays"]

    # 先算偏移（都相对段起点）
    off = SEC.size
    cmaps_off = off
    off += CMAP.size * len(cmaps)
    off = align_up(off, ALIGN_DSC)
    glyph_dsc_off = off
    off += len(glyph_dsc)

    cmap_offsets = []
    for c in cmaps:
        ul_off = 0
        if c["unicode_list"]:
            off = align_up(off, ALIGN_LIST)
            ul_off = off
            off += len(arrays[c["unicode_list"]][1])
        gf_off = 0
        if c["glyph_id_ofs_list"]:
            off = align_up(off, ALIGN_LIST)
            gf_off = off
            off += len(arrays[c["glyph_id_ofs_list"]][1])
        cmap_offsets.append((ul_off, gf_off))

    off = align_up(off, ALIGN_BITMAP)
    bitmap_off = off
    off += len(font["bitmap"])
    total = off

    buf = bytearray(total)
    buf[0:SEC.size] = SEC.pack(cmaps_off, glyph_dsc_off, bitmap_off, 0)
    buf[glyph_dsc_off:glyph_dsc_off + len(glyph_dsc)] = glyph_dsc
    buf[bitmap_off:bitmap_off + len(font["bitmap"])] = font["bitmap"]

    for i, c in enumerate(cmaps):
        ul_off, gf_off = cmap_offsets[i]
        if c["unicode_list"]:
            data = arrays[c["unicode_list"]][1]
            buf[ul_off:ul_off + len(data)] = data
        if c["glyph_id_ofs_list"]:
            data = arrays[c["glyph_id_ofs_list"]][1]
            buf[gf_off:gf_off + len(data)] = data
        buf[cmaps_off + i * CMAP.size:
            cmaps_off + (i + 1) * CMAP.size] = CMAP.pack(
                c["range_start"], ul_off, gf_off,
                c["range_length"], c["glyph_id_start"], c["list_length"],
                c["type"], 0)

    info = {
        "glyph_count": len(font["glyphs"]),
        "bitmap_size": len(font["bitmap"]),
        "cmap_num": len(cmaps),
        "section_size": total,
    }
    return bytes(buf), info


def build_blob(fonts, large):
    sections = []
    entries = []
    offset = align_up(HDR.size + ENT.size * len(fonts), 16)
    for (name, _filename), font in zip(SPECS, fonts):
        sec, info = build_font_section(font, large)
        offset = align_up(offset, 16)
        entries.append((name, offset, sec, info, font))
        offset += len(sec)

    blob = bytearray(offset)
    hdr = HDR.pack(MAGIC, VERSION, len(fonts), 0, 0, 16 if large else 8)
    blob[0:HDR.size] = hdr

    for i, (name, off, sec, info, font) in enumerate(entries):
        blob[off:off + len(sec)] = sec
        name_bytes = name.encode("ascii")
        if len(name_bytes) > 15:
            raise FontError("字体名太长：%s" % name)
        blob[HDR.size + i * ENT.size:
             HDR.size + (i + 1) * ENT.size] = ENT.pack(
            name_bytes, off, len(sec), info["glyph_count"], info["bitmap_size"],
            info["cmap_num"], font["line_height"], font["base_line"],
            font["underline_position"], font["underline_thickness"],
            font["bpp"], font["subpx"])

    payload = bytes(blob[HDR.size:])
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    blob[HDR.size - 12:HDR.size - 8] = struct.pack("<I", len(blob))
    blob[HDR.size - 8:HDR.size - 4] = struct.pack("<I", crc)
    return bytes(blob)


# --------------------------------------------------------------------------
# 自检：把 blob 当固件那样解码一遍
# --------------------------------------------------------------------------

def verify_blob(blob, large):
    if len(blob) < HDR.size:
        raise FontError("blob 太短")
    magic, version, count, blob_size, crc, dsc_size = HDR.unpack_from(blob, 0)
    if magic != MAGIC or version != VERSION:
        raise FontError("magic/version 不对：%r %d" % (magic, version))
    if blob_size != len(blob):
        raise FontError("blob_size=%d 与实际 %d 不一致" % (blob_size, len(blob)))
    if dsc_size != (16 if large else 8):
        raise FontError("glyph_dsc_size=%d 与 --%s 不一致"
                        % (dsc_size, "large" if large else "small"))
    if (zlib.crc32(blob[HDR.size:]) & 0xFFFFFFFF) != crc:
        raise FontError("CRC 校验失败")
    if count != len(SPECS):
        raise FontError("font_count=%d 与 SPECS 不一致" % count)

    for i in range(count):
        base = HDR.size + i * ENT.size
        (name, off, size, glyph_count, bitmap_size, cmap_num,
         line_height, base_line, ul_pos, ul_thick, bpp, subpx) = ENT.unpack_from(blob, base)
        name = name.split(b"\0", 1)[0].decode("ascii")
        if off % 16 or size <= 0 or off + size > len(blob):
            raise FontError("%s: 段偏移/长度越界" % name)
        cmaps_off, dsc_off, bmp_off, _ = SEC.unpack_from(blob, off)
        if (off + cmaps_off) % 4 or (off + dsc_off) % ALIGN_DSC or (off + bmp_off) % ALIGN_BITMAP:
            raise FontError("%s: 表没有按约定对齐" % name)
        if bmp_off + bitmap_size > size:
            raise FontError("%s: 位图越界" % name)
        if dsc_off + glyph_count * dsc_size > size:
            raise FontError("%s: glyph_dsc 越界" % name)
        if cmaps_off + cmap_num * CMAP.size > size:
            raise FontError("%s: cmap 表越界" % name)
        for j in range(cmap_num):
            rs, ul_off, gf_off, rl, gs, ll, typ, _pad = CMAP.unpack_from(
                blob, off + cmaps_off + j * CMAP.size)
            if ul_off:
                if (off + ul_off) % 2 or off + ul_off + ll * 2 > off + size:
                    raise FontError("%s: cmap %d unicode_list 越界" % (name, j))
            if gf_off:
                width = 1 if typ in (0, 2) else 2
                if (off + gf_off) % 2 or off + gf_off + ll * width > off + size:
                    raise FontError("%s: cmap %d glyph_id_ofs 越界" % (name, j))
        print("  %-7s %6d 字形  %8d B 位图  %2d cmap  %4d B 段"
              % (name, glyph_count, bitmap_size, cmap_num, size))
    print("  总大小 %.2f MB (%d B)，CRC 0x%08X"
          % (len(blob) / 1048576, len(blob), crc))


def detect_large():
    """从 sdkconfig / sdkconfig.defaults 推 LV_FONT_FMT_TXT_LARGE。"""
    for name in ("sdkconfig", "sdkconfig.defaults"):
        p = ROOT / name
        if not p.exists():
            continue
        text = p.read_text(encoding="utf-8", errors="ignore")
        m = re.search(r"^CONFIG_LV_FONT_FMT_TXT_LARGE=(\w+)$", text, re.M)
        if m:
            return m.group(1) == "y"
    return None


def main():
    ap = argparse.ArgumentParser(description="把 LVGL 字体 .c 打包成 fonts 分区镜像")
    ap.add_argument("--out", type=Path, default=OUT_BIN)
    ap.add_argument("--src-dir", type=Path, default=SRC_DIR)
    ap.add_argument("--large", dest="large", action="store_true", default=None,
                    help="按 LV_FONT_FMT_TXT_LARGE=y 的 16 字节描述符打包")
    ap.add_argument("--small", dest="large", action="store_false",
                    help="按紧凑 8 字节描述符打包（LARGE=n）")
    ap.add_argument("--verify", action="store_true", help="写完后解码自检")
    args = ap.parse_args()

    large = args.large
    if large is None:
        large = detect_large()
    if large is None:
        sys.exit("无法判断 CONFIG_LV_FONT_FMT_TXT_LARGE（sdkconfig 不存在？），"
                 "请显式传 --large 或 --small")

    print("布局     : %s（glyph_dsc %d 字节/字形）"
          % ("LARGE" if large else "compact", 16 if large else 8))
    print("字体源   : %s" % args.src_dir)

    fonts = []
    for name, filename in SPECS:
        font = parse_font_c(args.src_dir / filename, large)
        print("  解析 %-24s %6d 字形  %8d B 位图  %2d cmap"
              % (filename, len(font["glyphs"]), len(font["bitmap"]), len(font["cmaps"])))
        fonts.append(font)

    blob = build_blob(fonts, large)
    out_path = args.out if args.out.is_absolute() else (Path.cwd() / args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)
    try:
        shown = out_path.resolve().relative_to(ROOT)
    except ValueError:
        shown = out_path
    print("\n输出     : %s  %.2f MB (%d B)"
          % (shown, len(blob) / 1048576, len(blob)))

    if args.verify:
        print("\n自检     :")
        verify_blob(blob, large)
    print("\n下一步   : idf.py flash（fonts 分区会随 idf.py flash 一起写，见 components/ui_font/CMakeLists.txt）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
