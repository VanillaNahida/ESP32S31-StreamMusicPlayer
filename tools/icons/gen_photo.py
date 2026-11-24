# -*- coding: utf-8 -*-
"""
gen_photo.py —— PNG → 原始像素文件（.bin），给 target_add_binary_data 直接烘进固件

为什么不像图标那样生成 C 数组
------------------------------
图标是 22~96 px 的小图，写成 C 数组也就几 KB 源码。而封面是 184/210 px 的彩色图，
一个 210x210 的 ARGB8888 就是 176 KB 像素 —— 写成 {0x..,0x..} 是近 1 MB 的 .c 文件，
既难编也难看。这里改成只留一份 .bin 原始像素，构建期用

    target_add_binary_data(<component> "<file>.bin" BINARY RENAME_TO "<symbol>")

把字节原样搬进 flash，运行时只是把一个 lv_image_dsc_t 的头指过去（零解码、零拷贝）。

格式与 LVGL 的约定一致（小端）：
    rgb565    每像素 2 字节，低字节在前      LV_COLOR_FORMAT_RGB565
    rgb565a8  先 w*h*2 的 RGB565，再 w*h 的 A8  LV_COLOR_FORMAT_RGB565A8
    argb8888  每像素 4 字节，顺序 B,G,R,A     LV_COLOR_FORMAT_ARGB8888

⚠ 尺寸按显示尺寸烘（与 assets/icons/icons.txt 同一条纪律）：LVGL 运行期缩放是
  每帧都要重算的，封面这种固定尺寸的东西没理由付那个代价。

用法
----
    # 一次生成清单里的全部（清单在 assets/cover/photos.txt）
    tools/font/venv/Scripts/python.exe tools/icons/gen_photo.py

    # 只生成一条
    tools/font/venv/Scripts/python.exe tools/icons/gen_photo.py assets/defalut_cover.png --name pl_default_cover --size 184 --format rgb565

清单格式：源图路径  输出名  宽  高  格式
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_icon import decode_png, resize_rgba   # 复用 PNG 解码与面积平均缩放

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "assets" / "cover"


def fit_square(w, h, rgba):
    """**补透明边**凑成正方形（letterbox），一个像素都不裁。

    为什么不是"居中裁"：内置素材经常是"紧贴着拍"的（例如黑胶唱片那张，
    唱片本身就顶到了画布左/下边缘），再按 min(w,h) 居中裁会把左右各 14~15 px
    切掉 —— 真机上看着就是"唱片左右被切"（2026-11-24 用户反馈）。
    补边只增加透明像素，可见内容的位置与比例完全不变。
    """
    if w == h:
        return w, h, rgba
    side = max(w, h)
    ox = (side - w) // 2
    oy = (side - h) // 2
    out = bytearray(side * side * 4)          # 全 0 = 透明
    for y in range(h):
        dst = ((y + oy) * side + ox) * 4
        src = y * w * 4
        out[dst:dst + w * 4] = rgba[src:src + w * 4]
    return side, side, bytes(out)


def to_rgb565(w, h, rgba):
    out = bytearray(w * h * 2)
    o = 0
    for i in range(0, w * h * 4, 4):
        r, g, b = rgba[i], rgba[i + 1], rgba[i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[o] = v & 0xFF
        out[o + 1] = (v >> 8) & 0xFF
        o += 2
    return bytes(out)


def to_rgb565a8(w, h, rgba):
    """RGB565 平面 + A8 平面（LVGL 的 LV_COLOR_FORMAT_RGB565A8）。

    比 ARGB8888 省 1/4 的 flash（每像素 3 字节 vs 4），还保留 alpha ——
    内置黑胶唱片这种"照片级 + 四周透明"的图用它最划算。

    ★★ 布局是**平面分离**的，不是逐像素交错：
        [0 .. w*h*2)            RGB565（每像素 2 字节，低字节在前）
        [w*h*2 .. w*h*3)        每像素 1 字节的 alpha
      依据见 lv_draw_sw_img.c 的 RGB565A8 分支：mask_buf = src_buf + stride*h，
      mask_stride = stride/2（stride 就是 RGB 平面的行跨距 w*2）。
      ⚠ 2026-11-24 踩过的坑：早先这里按 RGB,A,RGB,A… 交错写，真机上就是整块"花屏"
        （RGB 平面里混进了 alpha 字节、alpha 平面又全是错位的 RGB 数据）。
    """
    n = w * h
    out = bytearray(n * 3)
    o_rgb = 0
    o_a = n * 2
    for i in range(0, n * 4, 4):
        r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[o_rgb] = v & 0xFF
        out[o_rgb + 1] = (v >> 8) & 0xFF
        out[o_a] = a
        o_rgb += 2
        o_a += 1
    return bytes(out)


def to_argb8888(w, h, rgba):
    out = bytearray(w * h * 4)
    for i in range(0, w * h * 4, 4):
        r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
        out[i] = b
        out[i + 1] = g
        out[i + 2] = r
        out[i + 3] = a
    return bytes(out)


def scale_for(src_w, src_h, rgba, tw, th):
    """补透明边凑方（不裁内容）+ 面积平均缩放到目标尺寸（rgba 输出）。"""
    sw, sh, sq = fit_square(src_w, src_h, rgba)
    return resize_rgba(sw, sh, sq, tw, th)


def build(src, name, tw, th, fmt, preview_dir):
    w, h, rgba = decode_png(src)
    px = scale_for(w, h, rgba, tw, th)
    if fmt == "rgb565":
        data = to_rgb565(tw, th, px)
    elif fmt == "rgb565a8":
        data = to_rgb565a8(tw, th, px)
    elif fmt == "argb8888":
        data = to_argb8888(tw, th, px)
    else:
        sys.exit("未知格式 " + fmt + "（只支持 rgb565 / rgb565a8 / argb8888）")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out = OUT_DIR / (name + "_" + str(tw) + "x" + str(th) + ".bin")
    out.write_bytes(data)
    print("%-34s %dx%d -> %-34s %6.1f KB" % (src.name, w, h, out.name, len(data) / 1024.0))

    if preview_dir:
        from svg_raster import write_png
        preview_dir.mkdir(parents=True, exist_ok=True)
        write_png(preview_dir / (name + ".png"), tw, th, px)
    return len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png", nargs="?", help="单个 PNG 源（不用 --manifest 时必填）")
    ap.add_argument("--manifest", default="assets/cover/photos.txt", help="清单文件")
    ap.add_argument("--name", default=None, help="单个模式的输出名")
    ap.add_argument("--size", type=int, default=0, help="单个模式的目标边长")
    ap.add_argument("--height", type=int, default=0, help="目标高（默认同 --size）")
    ap.add_argument("--format", default="rgb565", choices=["rgb565", "rgb565a8", "argb8888"])
    ap.add_argument("--preview", default=None, help="另存 PNG 目录（肉眼核对，不参与构建）")
    a = ap.parse_args()

    jobs = []
    if a.png:
        if not a.name or not a.size:
            sys.exit("单个模式必须给 --name 与 --size")
        jobs.append((a.png, a.name, a.size, a.height or a.size, a.format))
    else:
        man = Path(a.manifest)
        if not man.is_absolute():
            man = ROOT / man
        if not man.exists():
            sys.exit("找不到清单 " + str(man))
        for lineno, raw in enumerate(man.read_text(encoding="utf-8").splitlines(), 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            p = line.split()
            if len(p) != 5:
                sys.exit("%s:%d 应该是 [路径 输出名 宽 高 格式]，实际是 %r" % (man.name, lineno, raw))
            jobs.append((p[0], p[1], int(p[2]), int(p[3]), p[4]))

    preview_dir = Path(a.preview) if a.preview else None
    if preview_dir and not preview_dir.is_absolute():
        preview_dir = ROOT / preview_dir

    total = 0
    for path, name, tw, th, fmt in jobs:
        src = Path(path)
        if not src.is_absolute():
            src = ROOT / src
        if not src.exists():
            sys.exit("找不到 " + str(src))
        total += build(src, name, tw, th, fmt, preview_dir)
    print("合计 %.1f KB 像素（%d 张）" % (total / 1024.0, len(jobs)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
