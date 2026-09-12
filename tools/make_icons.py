#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_icons.py - 生成 CapsSwitch 的托盘图标（纯标准库，无第三方依赖）

产出：
    icons/IDI_ON.ico    启用态：绿色「Caps」键帽
    icons/IDI_OFF.ico   暂停态：灰色「Caps」键帽

图标内含 16x16 与 32x32 两档（规格 3.1 要求），均为 32bpp BGRA + AND 掩码
的未压缩 DIB，Windows 7 及以上原生支持，不依赖 PNG 压缩图标。

取图来源（按优先级）：
    1) icons/source_on.png / icons/source_off.png
       设计稿。脚本内置一个最小 PNG 解码器（8bit RGBA / 非交错），
       裁到不透明区域后按面积平均降采样到目标尺寸。
       面积平均在「预乘 alpha」空间里做，避免半透明边缘发黑。
    2) 找不到设计稿时，退化为脚本内置的矢量字形（圆角徽标 + 白色「中」）。

用法：
    python tools/make_icons.py
"""

import os
import struct
import zlib

# --- 路径 ---------------------------------------------------------------
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
ICON_DIR = os.path.normpath(os.path.join(_THIS_DIR, "..", "icons"))

SIZES = (16, 32)     # 需要内嵌的尺寸档位
SS = 8               # 矢量字形的超采样倍数（每边）

SOURCE_ON = os.path.join(ICON_DIR, "source_on.png")
SOURCE_OFF = os.path.join(ICON_DIR, "source_off.png")


# ===========================================================================
# 最小 PNG 解码器（8bit RGBA / 非交错）
# ===========================================================================

_PNG_SIG = b"\x89PNG\r\n\x1a\n"


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png(path):
    """
    解码 8bit RGBA 非交错 PNG。

    返回 (width, height, rgba)，rgba 是长度为 w*h*4 的 bytearray。
    只覆盖本工程用得到的最小区间；遇到不支持的格式直接抛错，
    而不是悄悄给出一张错图。
    """
    with open(path, "rb") as fp:
        data = fp.read()

    if not data.startswith(_PNG_SIG):
        raise ValueError("%s 不是 PNG 文件" % path)

    pos = len(_PNG_SIG)
    width = height = bitdepth = colortype = interlace = None
    idat = bytearray()

    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length          # 跳过 length + type + data + crc

        if ctype == b"IHDR":
            (width, height, bitdepth, colortype,
             _compression, _filter, interlace) = struct.unpack(">IIBBBBB", body)
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break

    if (width, height, bitdepth, colortype, interlace) != \
            (width, height, 8, 6, 0):
        raise ValueError(
            "%s 格式不受支持（需 8bit RGBA 非交错，实际 bitdepth=%s "
            "colortype=%s interlace=%s）" % (path, bitdepth, colortype, interlace))

    bpp = 4
    stride = width * bpp
    raw = zlib.decompress(bytes(idat))

    out = bytearray(stride * height)
    prev = bytearray(stride)
    rp = 0

    for y in range(height):
        ftype = raw[rp]
        rp += 1
        line = bytearray(raw[rp:rp + stride])
        rp += stride

        if ftype == 1:      # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:    # Average
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:    # Paeth
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                upleft = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + _paeth(left, prev[i], upleft)) & 0xFF
        elif ftype != 0:
            raise ValueError("%s 出现未知的行过滤器 %d" % (path, ftype))

        out[y * stride:(y + 1) * stride] = line
        prev = line

    return width, height, out


def alpha_bbox(width, height, rgba, threshold=8):
    """
    求不透明区域的外接矩形，返回 (x0, y0, x1, y1)，右/下为开区间。
    设计稿四周常带透明留白，裁掉它才能让图标占满画布。
    """
    x0, y0, x1, y1 = width, height, 0, 0
    for y in range(height):
        base = y * width * 4
        for x in range(width):
            if rgba[base + x * 4 + 3] > threshold:
                if x < x0:
                    x0 = x
                if x > x1:
                    x1 = x
                if y < y0:
                    y0 = y
                if y > y1:
                    y1 = y
    if x1 < x0 or y1 < y0:
        return 0, 0, width, height     # 全透明，退回整图
    return x0, y0, x1 + 1, y1 + 1


def downsample(width, height, rgba, box, size, bg_hint=(0, 0, 0)):
    """
    把 box 区域的内容按面积平均缩放到 size x size，返回自上而下的 RGBA 行。

    关键点：在预乘 alpha 空间累加再还原。直接平均非预乘的 RGB 会让
    「白色文字 + 半透明边缘」这类像素在缩小后出现暗边。
    """
    x0, y0, x1, y1 = box
    bw, bh = x1 - x0, y1 - y0

    # 等比缩放后居中放置（contain），保留设计稿自带的边距感
    scale = min(size / float(bw), size / float(bh))
    draw_w = bw * scale
    draw_h = bh * scale
    off_x = (size - draw_w) / 2.0
    off_y = (size - draw_h) / 2.0

    rows = []
    for dy in range(size):
        row = []
        for dx in range(size):
            # 目标像素在裁剪区域坐标系里覆盖的矩形
            sx0 = (dx - off_x) / scale
            sx1 = (dx + 1 - off_x) / scale
            sy0 = (dy - off_y) / scale
            sy1 = (dy + 1 - off_y) / scale

            xs = max(0, int(sx0))
            xe = min(bw, int(sx1 + 0.9999))
            ys = max(0, int(sy0))
            ye = min(bh, int(sy1 + 0.9999))

            if xe <= xs or ye <= ys:
                row.append((0, 0, 0, 0))
                continue

            sum_a = 0
            sum_r = sum_g = sum_b = 0
            count = 0
            for sy in range(ys, ye):
                base = ((y0 + sy) * width + (x0 + xs)) * 4
                for sx in range(xs, xe):
                    r = rgba[base]
                    g = rgba[base + 1]
                    b = rgba[base + 2]
                    a = rgba[base + 3]
                    base += 4
                    count += 1
                    if a:
                        sum_a += a
                        sum_r += r * a
                        sum_g += g * a
                        sum_b += b * a

            if count == 0 or sum_a == 0:
                row.append((0, 0, 0, 0))
                continue

            row.append((
                int(round(sum_r / sum_a)),
                int(round(sum_g / sum_a)),
                int(round(sum_b / sum_a)),
                int(round(sum_a / count)),
            ))
        rows.append(row)

    return rows


# ===========================================================================
# 内置矢量字形（没有设计稿时的兜底）
# ===========================================================================

COLOR_ON = (46, 168, 74)
COLOR_OFF = (128, 134, 142)
COLOR_GLYPH = (255, 255, 255)

BG_RECT = (0.03, 0.03, 0.97, 0.97)
BG_RADIUS = 0.22
BAR_X0, BAR_X1 = 0.43, 0.57
BAR_Y0, BAR_Y1 = 0.18, 0.84
BOX_X0, BOX_Y0 = 0.23, 0.36
BOX_X1, BOX_Y1 = 0.77, 0.78
BOX_BORDER = 0.125


def _inside_rounded_rect(x, y, rect, radius):
    ax, ay, bx, by = rect
    if x < ax or x > bx or y < ay or y > by:
        return False
    nx = min(max(x, ax + radius), bx - radius)
    ny = min(max(y, ay + radius), by - radius)
    dx, dy = x - nx, y - ny
    return dx * dx + dy * dy <= radius * radius


def _inside_glyph(x, y):
    if BAR_X0 <= x <= BAR_X1 and BAR_Y0 <= y <= BAR_Y1:
        return True
    if BOX_X0 <= x <= BOX_X1 and BOX_Y0 <= y <= BOX_Y1:
        in_hole = (BOX_X0 + BOX_BORDER <= x <= BOX_X1 - BOX_BORDER and
                   BOX_Y0 + BOX_BORDER <= y <= BOX_Y1 - BOX_BORDER)
        if not in_hole:
            return True
    return False


def render_vector(size, bg_color):
    """绘制兜底的矢量图标：圆角徽标 + 白色「中」。"""
    rows = []
    step = 1.0 / (size * SS)
    half = step * 0.5
    total = SS * SS

    for py in range(size):
        row = []
        for px in range(size):
            cov_bg = cov_glyph = 0
            for sy in range(SS):
                y = (py * SS + sy) * step + half
                for sx in range(SS):
                    x = (px * SS + sx) * step + half
                    if _inside_rounded_rect(x, y, BG_RECT, BG_RADIUS):
                        cov_bg += 1
                        if _inside_glyph(x, y):
                            cov_glyph += 1

            a_bg = cov_bg / total
            a_gl = cov_glyph / total
            if a_bg <= 0.0:
                row.append((0, 0, 0, 0))
                continue

            alpha = a_gl + a_bg * (1.0 - a_gl)
            r = (COLOR_GLYPH[0] * a_gl + bg_color[0] * a_bg * (1.0 - a_gl)) / alpha
            g = (COLOR_GLYPH[1] * a_gl + bg_color[1] * a_bg * (1.0 - a_gl)) / alpha
            b = (COLOR_GLYPH[2] * a_gl + bg_color[2] * a_bg * (1.0 - a_gl)) / alpha
            row.append((int(round(r)), int(round(g)), int(round(b)),
                        int(round(alpha * 255))))
        rows.append(row)
    return rows


# ===========================================================================
# ICO 打包
# ===========================================================================

def build_dib(rows):
    """BITMAPINFOHEADER + XOR 位图 + AND 掩码。"""
    height = len(rows)
    width = len(rows[0])

    header = struct.pack(
        "<IiiHHIIiiII",
        40, width, height * 2, 1, 32, 0, 0, 0, 0, 0, 0,
    )

    xor = bytearray()                       # 32bpp BGRA，自下而上
    for y in range(height - 1, -1, -1):
        for (r, g, b, a) in rows[y]:
            xor += bytes((b, g, r, a))

    row_bytes = ((width + 31) // 32) * 4    # AND 掩码，每行 4 字节对齐
    and_mask = bytearray()
    for y in range(height - 1, -1, -1):
        line = bytearray(row_bytes)
        for x in range(width):
            if rows[y][x][3] < 128:
                line[x >> 3] |= (0x80 >> (x & 7))
        and_mask += line

    return bytes(header) + bytes(xor) + bytes(and_mask)


def build_ico(images):
    """images: [(size, dib_bytes), ...] -> 完整 ICO 文件字节串"""
    count = len(images)
    out = struct.pack("<HHH", 0, 1, count)

    offset = 6 + 16 * count
    entries = bytearray()
    payload = bytearray()

    for (size, dib) in images:
        entries += struct.pack(
            "<BBBBHHII",
            0 if size >= 256 else size,
            0 if size >= 256 else size,
            0, 0, 1, 32, len(dib), offset,
        )
        payload += dib
        offset += len(dib)

    return bytes(out + entries + payload)


def write_icon(path, rows_by_size):
    images = [(size, build_dib(rows)) for (size, rows) in rows_by_size]
    with open(path, "wb") as fp:
        fp.write(build_ico(images))
    print("wrote %s (%d bytes, sizes %s)"
          % (path, os.path.getsize(path),
             "/".join(str(s) for s, _ in rows_by_size)))


# ===========================================================================
# 入口
# ===========================================================================

def build_from_source(png_path):
    width, height, rgba = decode_png(png_path)
    box = alpha_bbox(width, height, rgba)
    return [(size, downsample(width, height, rgba, box, size)) for size in SIZES]


def main():
    os.makedirs(ICON_DIR, exist_ok=True)

    if os.path.isfile(SOURCE_ON) and os.path.isfile(SOURCE_OFF):
        print("来源：设计稿 %s / %s"
              % (os.path.basename(SOURCE_ON), os.path.basename(SOURCE_OFF)))
        write_icon(os.path.join(ICON_DIR, "IDI_ON.ico"),
                   build_from_source(SOURCE_ON))
        write_icon(os.path.join(ICON_DIR, "IDI_OFF.ico"),
                   build_from_source(SOURCE_OFF))
    else:
        print("来源：内置矢量字形（未找到 icons/source_on.png、source_off.png）")
        write_icon(os.path.join(ICON_DIR, "IDI_ON.ico"),
                   [(s, render_vector(s, COLOR_ON)) for s in SIZES])
        write_icon(os.path.join(ICON_DIR, "IDI_OFF.ico"),
                   [(s, render_vector(s, COLOR_OFF)) for s in SIZES])


if __name__ == "__main__":
    main()
