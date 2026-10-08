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


def emit_c(name: str, tw: int, th: int, rgba: bytes, src_name: str) -> str:
    lines = [
        "/*",
        f" * {name} —— 由 tools/icons/gen_icon.py 从 assets/icons/{src_name} 生成。",
        " * 不要手改：重新生成 =",
        f" *   tools/font/venv/Scripts/python.exe tools/icons/gen_icon.py \\",
        f" *       assets/icons/{src_name} --name {name} --size {tw}",
        " *",
        f" * {tw}x{th} ARGB8888（LVGL 字节序 B,G,R,A），像素已解码好 —— 运行时不需要",
        " * 任何 PNG 解码器（本工程没开 LV_USE_LIBPNG/LODEPNG）。",
        " */",
        '#include "lvgl.h"',
        "",
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("--name", default="netease_logo")
    ap.add_argument("--size", type=int, default=96, help="输出边长（默认 UI_HOME_ICON=96）")
    ap.add_argument("-o", "--out", default=None)
    a = ap.parse_args()

    src = Path(a.png)
    if not src.is_absolute():
        src = ROOT / src
    if not src.exists():
        sys.exit(f"找不到 {src}")

    w, h, rgba = decode_png(src)
    print(f"输入 : {src.name}  {w}x{h}")
    if w != h:
        print(f"注意 : 输入不是正方形（{w}x{h}），会按 {a.size}x{a.size} 拉伸")
    px = resize_rgba(w, h, rgba, a.size, a.size)
    print(f"输出 : {a.name}  {a.size}x{a.size} ARGB8888  {len(px) / 1024:.1f} KB 位图")

    out = Path(a.out) if a.out else (ROOT / "components" / "ui_core" / "ui_icons.c")
    if not out.is_absolute():
        out = ROOT / out
    out.write_text(emit_c(a.name, a.size, a.size, px, src.name), encoding="utf-8")
    print(f"写入 : {out.relative_to(ROOT)}  {out.stat().st_size / 1024:.0f} KB 源码")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
