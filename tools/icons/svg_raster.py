# -*- coding: utf-8 -*-
"""
svg_raster.py —— 极简 SVG 路径光栅化器（**纯标准库**，不装任何东西）

为什么自己写
------------
`assets/icons/` 里的状态栏图标是 SVG（Material Symbols 的单个 `<path d>`），
而本工程既没有 cairosvg / PIL（AGENTS.md 第 9 节：只用组件自带的 venv，
禁止全局 pip），也没有 ImageMagick / Inkscape 可依赖。

这些图标的路径**只用到直线与二/三次贝塞尔**，没有变换、渐变、描边、裁剪，
所以一个"解析 d → 拍平曲线 → 扫描线填充"的小实现就够了，而且完全可复现：
同一个 SVG 在谁的机器上都烘出同一张位图。

口径
----
* 支持 `M/m L/l H/h V/v C/c S/s Q/q T/t Z/z`（`A/a` 圆弧不支持，遇到直接报错
  ——本工程的图标没有用到，真要用请先在设计工具里把弧线转成曲线）。
* 填充规则 **nonzero**（SVG 默认）—— Material Symbols 的"挖空"就是靠子路径
  反向缠绕做的，用 even-odd 会把实心部分挖穿。
* 颜色：算出来的位图只有"填充色 + alpha 覆盖"，填充色由 `fill` 参数给
  （默认白色）。**运行时用 `lv_obj_set_style_image_recolor()` 上色**，
  所以这里烘成什么颜色都不影响显示，白色只是"没上色时也看得见"。
* 抗锯齿靠**超采样**：先按 `size * ss`（默认 4 倍）光栅化，再交给
  `gen_icon.py` 的 box filter 缩到显示尺寸 —— 每个边沿像素有 16 级灰阶。
* 坐标系按 `viewBox` 归一：Material Symbols 的 `viewBox="0 -960 960 960"`
  （y 轴向上为负）会被自动映射到 `0..size` 的正向像素坐标。

输出：`(w, h, rgba_bytes)`，`rgba` 是**非预乘**的 R,G,B,A（与 LVGL 的
`LV_COLOR_FORMAT_ARGB8888` 一致，见 lv_draw_sw_blend_to_rgb565.c 的
`argb8888_image_blend()`：它按 `src_a/255` 直混，不预乘）。
"""
import math
import re
import struct
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

# 数字 / 命令的词法：SVG 允许 "440-80"（无分隔符）与 "1.5.5"（隐式小数点）这种写法，
# 所以不能用 split()，必须按正则逐个抓。
_TOKEN = re.compile(
    r"([MmLlHhVvCcSsQqTtAaZz])"
    r"|([-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?)"
)

# 每条命令要吃多少个数字
_ARGC = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "T": 2}


class SvgError(Exception):
    pass


def _tokens(d: str):
    out = []
    for m in _TOKEN.finditer(d):
        if m.group(1):
            out.append(m.group(1))
        else:
            out.append(float(m.group(2)))
    return out


def _cubic(p0, p1, p2, p3, steps):
    pts = []
    for k in range(1, steps + 1):
        t = k / steps
        mt = 1.0 - t
        a, b, c, e = mt * mt * mt, 3 * mt * mt * t, 3 * mt * t * t, t * t * t
        pts.append((a * p0[0] + b * p1[0] + c * p2[0] + e * p3[0],
                    a * p0[1] + b * p1[1] + c * p2[1] + e * p3[1]))
    return pts


def _quad(p0, p1, p2, steps):
    pts = []
    for k in range(1, steps + 1):
        t = k / steps
        mt = 1.0 - t
        a, b, c = mt * mt, 2 * mt * t, t * t
        pts.append((a * p0[0] + b * p1[0] + c * p2[0],
                    a * p0[1] + b * p1[1] + c * p2[1]))
    return pts


def flatten_path(d: str, steps: int = 12):
    """把 path 的 `d` 拍平成若干个子路径（每个是 [(x, y), ...]）。

    闭合由填充阶段隐式完成（SVG 的填充规则本来就认为子路径是闭合的），
    所以这里不区分有没有写 `Z`。
    """
    t = _tokens(d)
    i = 0
    subs = []          # type: list
    cur = None         # 当前子路径的点表
    x = y = 0.0        # 当前点
    sx = sy = 0.0      # 子路径起点
    cmd = None
    last_c = None      # 上一个三次控制点（S/s 用，绝对坐标）
    last_q = None      # 上一个二次控制点（T/t 用，绝对坐标）

    def peek_num():
        return i < len(t) and not isinstance(t[i], str)

    def take(n):
        nonlocal i
        vals = t[i:i + n]
        if len(vals) < n or any(isinstance(v, str) for v in vals):
            raise SvgError("命令参数不足: %r" % (d[:60],))
        i += n
        return vals

    while i < len(t):
        if isinstance(t[i], str):
            cmd = t[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    subs.append(cur)
                    cur = None
                x, y = sx, sy
                last_c = last_q = None
                cmd = None
                continue
        if cmd is None:
            raise SvgError("路径以非命令开头: %r" % (d[:60],))

        upper = cmd.upper()
        rel = cmd.islower()
        if upper == "A":
            raise SvgError("不支持圆弧命令 A/a（本工程图标不需要，"
                           "请在设计工具里转成曲线）")
        need = _ARGC[upper]
        if not peek_num():
            continue
        args = take(need)
        ox, oy = (x, y) if rel else (0.0, 0.0)

        if upper == "M":
            x, y = args[0] + ox, args[1] + oy
            if cur:
                subs.append(cur)
            cur = [(x, y)]
            sx, sy = x, y
            last_c = last_q = None
            # moveto 之后跟着的数字对是 **lineto**（隐式命令切换）
            cmd = "l" if rel else "L"
        elif upper == "L":
            x, y = args[0] + ox, args[1] + oy
            cur.append((x, y))
            last_c = last_q = None
        elif upper == "H":
            x = args[0] + (x if rel else 0.0)
            cur.append((x, y))
            last_c = last_q = None
        elif upper == "V":
            y = args[0] + (y if rel else 0.0)
            cur.append((x, y))
            last_c = last_q = None
        elif upper == "C":
            p1 = (args[0] + ox, args[1] + oy)
            p2 = (args[2] + ox, args[3] + oy)
            p3 = (args[4] + ox, args[5] + oy)
            cur.extend(_cubic((x, y), p1, p2, p3, steps))
            x, y = p3
            last_c, last_q = p2, None
        elif upper == "S":
            p1 = (2 * x - last_c[0], 2 * y - last_c[1]) if last_c else (x, y)
            p2 = (args[0] + ox, args[1] + oy)
            p3 = (args[2] + ox, args[3] + oy)
            cur.extend(_cubic((x, y), p1, p2, p3, steps))
            x, y = p3
            last_c, last_q = p2, None
        elif upper == "Q":
            p1 = (args[0] + ox, args[1] + oy)
            p2 = (args[2] + ox, args[3] + oy)
            cur.extend(_quad((x, y), p1, p2, steps))
            x, y = p2
            last_q, last_c = p1, None
        elif upper == "T":
            p1 = (2 * x - last_q[0], 2 * y - last_q[1]) if last_q else (x, y)
            p2 = (args[0] + ox, args[1] + oy)
            cur.extend(_quad((x, y), p1, p2, steps))
            x, y = p2
            last_q, last_c = p1, None

    if cur:
        subs.append(cur)
    return subs


def _fill_nonzero(edges, w: int, h: int) -> bytearray:
    """扫描线 + 非零环绕填充。edges 是像素坐标系里的 (x0,y0,x1,y1)。"""
    cov = bytearray(w * h)
    for py in range(h):
        yc = py + 0.5
        xs = []
        for (x0, y0, x1, y1) in edges:
            if y0 <= yc < y1:
                xs.append((x0 + (yc - y0) * (x1 - x0) / (y1 - y0), 1))
            elif y1 <= yc < y0:
                xs.append((x0 + (yc - y0) * (x1 - x0) / (y1 - y0), -1))
        if len(xs) < 2:
            continue
        xs.sort(key=lambda p: p[0])
        wind = 0
        row = py * w
        for k in range(len(xs) - 1):
            wind += xs[k][1]
            if wind == 0:
                continue
            xa = xs[k][0]
            xb = xs[k + 1][0]
            ia = int(math.ceil(xa - 0.5))
            ib = int(math.floor(xb - 0.5))
            if ia < 0:
                ia = 0
            if ib > w - 1:
                ib = w - 1
            for px in range(ia, ib + 1):
                cov[row + px] = 255
    return cov


def parse_svg(path: Path):
    """返回 ((vx, vy, vw, vh), [path_d, ...])。"""
    root = ET.parse(str(path)).getroot()
    vb = root.get("viewBox")
    if vb:
        parts = [float(v) for v in re.split(r"[,\s]+", vb.strip()) if v]
        if len(parts) != 4:
            raise SvgError("viewBox 不是 4 个数: %r" % (vb,))
        vx, vy, vw, vh = parts
    else:
        def _len(name, dflt):
            v = root.get(name)
            if not v:
                return dflt
            m = re.match(r"[-+]?[\d.]+", v)
            return float(m.group(0)) if m else dflt
        vx, vy, vw, vh = 0.0, 0.0, _len("width", 24.0), _len("height", 24.0)

    paths = []
    for el in root.iter():
        if el.tag.split("}")[-1] != "path":
            continue
        d = el.get("d")
        if not d:
            continue
        if (el.get("fill") or "").strip().lower() == "none":
            continue
        paths.append(d)
    if not paths:
        raise SvgError("%s 里没有可填充的 <path>" % path)
    return (vx, vy, vw, vh), paths


def render_svg(path, size: int, ss: int = 4, fill=(255, 255, 255),
               steps: int = 12):
    """把 SVG 渲染成 size*ss 见方的 RGBA（返回 (w, h, rgba)）。"""
    path = Path(path)
    (vx, vy, vw, vh), paths = parse_svg(path)
    if vw <= 0 or vh <= 0:
        raise SvgError("%s 的 viewBox 尺寸非法" % path)

    w = h = int(size) * int(ss)
    kx, ky = w / vw, h / vh

    edges = []
    for d in paths:
        for sub in flatten_path(d, steps):
            if len(sub) < 2:
                continue
            pts = [((px - vx) * kx, (py - vy) * ky) for (px, py) in sub]
            n = len(pts)
            for k in range(n):
                x0, y0 = pts[k]
                x1, y1 = pts[(k + 1) % n]
                if y0 != y1:
                    edges.append((x0, y0, x1, y1))
    if not edges:
        raise SvgError("%s 拍平后没有任何边（d 有问题？）" % path)

    cov = _fill_nonzero(edges, w, h)
    r, g, b = fill
    rgba = bytearray(w * h * 4)
    for idx in range(w * h):
        if cov[idx]:
            o = idx * 4
            rgba[o] = r
            rgba[o + 1] = g
            rgba[o + 2] = b
            rgba[o + 3] = 255
    return w, h, bytes(rgba)


def write_png(path, w: int, h: int, rgba: bytes):
    """写 8-bit RGBA PNG（只给 --preview 用，看一眼烘出来长什么样）。"""
    raw = bytearray()
    for y in range(h):
        raw.append(0)                      # filter type 0
        raw += rgba[y * w * 4:(y + 1) * w * 4]

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    Path(path).write_bytes(png)


if __name__ == "__main__":
    import argparse
    import sys

    ap = argparse.ArgumentParser(description="SVG → PNG（调试用；固件走 gen_icon.py）")
    ap.add_argument("svg")
    ap.add_argument("-s", "--size", type=int, default=22)
    ap.add_argument("--ss", type=int, default=4, help="超采样倍数")
    ap.add_argument("--fill", default="ffffff", help="填充色 RRGGBB")
    ap.add_argument("-o", "--out", default=None)
    a = ap.parse_args()
    try:
        f = tuple(int(a.fill[i:i + 2], 16) for i in (0, 2, 4))
    except Exception:
        sys.exit("--fill 要是 6 位十六进制，如 ffffff")
    W, H, px = render_svg(a.svg, a.size, a.ss, f)
    out = a.out or (str(Path(a.svg).with_suffix("")) + ".png")
    write_png(out, W, H, px)
    print("%s -> %s (%dx%d, ss=%d)" % (a.svg, out, W, H, a.ss))
