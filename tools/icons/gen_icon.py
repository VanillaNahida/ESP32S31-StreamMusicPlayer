# -*- coding: utf-8 -*-
"""
gen_icon.py —— PNG → LVGL 图像 C 数组（ARGB8888）

为什么自己解 PNG
----------------
工程里**没有开任何 LVGL 的 PNG 解码器**（`LV_USE_LIBPNG` / `LV_USE_LODEPNG` /
`LV_USE_TJPGD` 全是 n），`tools/font/venv` 里也没有 PIL（而且 AGENTS.md 禁止全局
pip 装东西）。所以这里只用标准库 `zlib` 手解 PNG —— 对"图标"这种小图完全够用，
而且产物是**已经解码好的像素**，运行时不需要任何解码器、不占额外的 flash 代码。

口径
----
* 支持 8 bit、非隔行的 PNG：灰度(0)/真彩(2)/调色板(3)/灰度+α(4)/真彩+α(6)；
  1/2/4 bit 与 Adam7 隔行会直接报错退出（图标不该是那种）。
* 输出 **ARGB8888**（LVGL 的字节序是 B,G,R,A）—— 带 alpha，能直接盖在首页那个
  圆角色块上；缩放用面积平均（box filter），缩小时比最近邻干净得多。
* 目标尺寸默认按 `UI_HOME_ICON`（96 px）给，改 -s 可覆盖。**渲染成显示尺寸**，
  避免 LVGL 运行期缩放（那是每帧都算的）。

用法
----
    tools\\font\\venv\\Scripts\\python.exe tools\\icons\\gen_icon.py
        assets/icons/netease.png --name netease_logo --size 96
        -o components/ui_core/ui_icons.c
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}   # colortype → 每像素通道数（8 bit）


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png(path: Path):
    """返回 (w, h, rgba_bytes)。只支持 8 bit / 非隔行。"""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path} 不是 PNG")

    pos, idat = 8, bytearray()
    w = h = depth = ctype = None
    palette = trns = None
    while pos + 8 <= len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, depth, ctype, comp, filt, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8:
                sys.exit(f"只支持 8 bit PNG，这个是 {depth} bit")
            if interlace != 0:
                sys.exit("不支持隔行(Adam7) PNG")
            if ctype not in CHANNELS:
                sys.exit(f"不支持的 color type {ctype}")
        elif typ == b"PLTE":
            palette = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
    if w is None:
        sys.exit("PNG 里没有 IHDR")

    raw = zlib.decompress(bytes(idat))
    ch = CHANNELS[ctype]
    stride = w * ch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        # 反滤波（PNG 规范 9.2 的 5 种）
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                c = prev[i - ch] if i >= ch else 0
                line[i] = (line[i] + paeth(a, prev[i], c)) & 0xFF
        elif f != 0:
            sys.exit(f"第 {y} 行有未知滤波类型 {f}")
        out[y * stride:(y + 1) * stride] = line
        prev = line

    # 统一转成 RGBA
    rgba = bytearray(w * h * 4)
    if ctype == 6:
        rgba[:] = out
    elif ctype == 2:
        for i in range(w * h):
            rgba[i * 4:i * 4 + 3] = out[i * 3:i * 3 + 3]
            rgba[i * 4 + 3] = 255
    elif ctype == 0:
        for i in range(w * h):
            v = out[i]
            rgba[i * 4:i * 4 + 4] = bytes((v, v, v, 255))
    elif ctype == 4:
        for i in range(w * h):
            v = out[i * 2]
            rgba[i * 4:i * 4 + 4] = bytes((v, v, v, out[i * 2 + 1]))
    else:  # 调色板
        for i in range(w * h):
            idx = out[i]
            rgba[i * 4:i * 4 + 3] = palette[idx * 3:idx * 3 + 3]
            a = trns[idx] if (trns and idx < len(trns)) else 255
            rgba[i * 4 + 3] = a
    return w, h, rgba


def resize_rgba(w: int, h: int, rgba: bytes, tw: int, th: int) -> bytes:
    """面积平均缩放（box filter）。"""
    if (w, h) == (tw, th):
        return rgba
    out = bytearray(tw * th * 4)
    for ty in range(th):
        y0 = ty * h // th
        y1 = max(y0 + 1, (ty + 1) * h // th)
        for tx in range(tw):
            x0 = tx * w // tw
            x1 = max(x0 + 1, (tx + 1) * w // tw)
            r = g = b = a = n = 0
            for y in range(y0, y1):
                base = y * w
                for x in range(x0, x1):
                    i = (base + x) * 4
                    # 按 alpha 加权，避免透明区把颜色拉向黑
                    al = rgba[i + 3]
                    r += rgba[i] * al
                    g += rgba[i + 1] * al
                    b += rgba[i + 2] * al
                    a += al
                    n += 1
            o = (ty * tw + tx) * 4
            if a:
                out[o] = r // a
                out[o + 1] = g // a
                out[o + 2] = b // a
            out[o + 3] = a // n
    return bytes(out)


def emit_icon(name: str, tw: int, th: int, rgba: bytes, src_name: str) -> str:
    """单个图标的 C 代码块（静态像素表 + lv_image_dsc_t），不含 #include。"""
    lines = [
        "/*",
        f" * {name} —— 由 tools/icons/gen_icon.py 从 {src_name} 生成。",
        " * 不要手改：重新生成 =",
        " *   tools/font/venv/Scripts/python.exe tools/icons/gen_icon.py \\",
        f" *       {src_name} --name {name} --size {tw}",
        " * （或按 assets/icons/icons.txt 清单一次生成全部，见文件头）",
        " *",
        f" * {tw}x{th} ARGB8888（LVGL 字节序 B,G,R,A），像素已解码好 —— 运行时不需要",
        " * 任何 PNG 解码器（本工程没开 LV_USE_LIBPNG/LODEPNG）。",
        " */",
        f"static const uint8_t {name}_map[] = {{",
    ]
    per = 16   # 每行 16 个字节
    for i in range(0, len(rgba), 4):
        r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
        if (i // 4) % per == 0:
            lines.append("    ")
        lines.append(f"0x{b:02x},0x{g:02x},0x{r:02x},0x{a:02x},")
        if (i // 4) % per == per - 1:
            lines.append("")
    lines.append("")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_image_dsc_t {name} = {{")
    lines.append("    .header.magic  = LV_IMAGE_HEADER_MAGIC,")
    lines.append("    .header.cf     = LV_COLOR_FORMAT_ARGB8888,")
    lines.append(f"    .header.w      = {tw},")
    lines.append(f"    .header.h      = {th},")
    lines.append(f"    .header.stride = {tw * 4},")
    lines.append(f"    .data_size     = sizeof({name}_map),")
    lines.append(f"    .data          = {name}_map,")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


FILE_HEADER = """/*
 * ui_icons.c —— 烘进固件的位图图标（全部由 tools/icons/gen_icon.py 生成，不要手改）
 *
 * 一次生成全部（清单在 assets/icons/icons.txt）：
 *   tools/font/venv/Scripts/python.exe tools/icons/gen_icon.py \\
 *       --manifest assets/icons/icons.txt -o components/ui_core/ui_icons.c
 *
 * 清单里那几个源图里有两个"扩展名撒谎"的（见 tools/icons/prepare_sources.ps1）：
 * default.png 其实是 JPEG、MomoIcon.png 其实是 ICO，另外那个 ✕ 本来是内联 SVG。
 * 换素材后先跑 prepare_sources.ps1 转成标准 PNG，再跑本脚本。
 *
 * 为什么烘成 C 数组而不是运行时解码：
 *   图标是固定尺寸的小图，编译期解好 = 运行时零解码开销、也不占解码器代码；
 *   声明见 include/ui_icons.h。
 */
#include "lvgl.h"

"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("png", nargs="?", help="单个 PNG 源（不用 --manifest 时必填）")
    ap.add_argument("--manifest", default=None,
                    help="图标清单文件：每行 `路径 符号名 尺寸`，# 开头是注释")
    ap.add_argument("--name", default="netease_logo")
    ap.add_argument("--size", type=int, default=96, help="输出边长（默认 UI_HOME_ICON=96）")
    ap.add_argument("-o", "--out", default=None)
    a = ap.parse_args()

    jobs = []   # (源文件, 符号名, 尺寸)
    if a.manifest:
        man = Path(a.manifest)
        if not man.is_absolute():
            man = ROOT / man
        if not man.exists():
            sys.exit(f"找不到清单 {man}")
        for lineno, raw in enumerate(man.read_text(encoding="utf-8").splitlines(), 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 3:
                sys.exit(f"{man.name}:{lineno} 应该是 `路径 符号名 尺寸`，实际是 {raw!r}")
            jobs.append((parts[0], parts[1], int(parts[2])))
        if not jobs:
            sys.exit("清单里一个图标都没有")
    else:
        if not a.png:
            sys.exit("要么给一个 PNG 源，要么用 --manifest")
        jobs.append((a.png, a.name, a.size))

    blocks = []
    px_bytes = 0
    for path, name, size in jobs:
        src = Path(path)
        if not src.is_absolute():
            src = ROOT / src
        if not src.exists():
            sys.exit(f"找不到 {src}")
        w, h, rgba = decode_png(src)
        note = "" if w == h else f"（源 {w}x{h} 非正方形，按 {size}x{size} 拉伸）"
        print(f"{src.name:<26} {w}x{h} -> {name:<18} {size}x{size} {note}")
        px = resize_rgba(w, h, rgba, size, size)
        px_bytes += len(px)
        blocks.append(emit_icon(name, size, size, px, f"assets/icons/{src.name}"
                                if src.parent.name == "icons" else f"assets/{src.name}"))

    out = Path(a.out) if a.out else (ROOT / "components" / "ui_core" / "ui_icons.c")
    if not out.is_absolute():
        out = ROOT / out
    out.write_text(FILE_HEADER + "\n".join(blocks), encoding="utf-8")
    print(f"写入 : {out.relative_to(ROOT)}  {out.stat().st_size / 1024:.0f} KB 源码 "
          f"（{len(blocks)} 个图标，位图合计 {px_bytes / 1024:.1f} KB flash）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
