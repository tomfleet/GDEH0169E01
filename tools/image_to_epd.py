#!/usr/bin/env python3
"""Convert a 400x400 RGB image to the GDEH0169E01 6-color EPD C array.

Output format matches Send_HV_Stripe_imageData: column-major, 4bpp, with
upper-half pixels in high nibbles and lower-half pixels in low nibbles.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterable, List, Tuple

from PIL import Image


PALETTE = [
    ("black", (0, 0, 0), 0x0),
    ("white", (255, 255, 255), 0x1),
    ("yellow", (255, 255, 0), 0x2),
    ("red", (255, 0, 0), 0x3),
    ("blue", (0, 0, 255), 0x5),
    ("green", (0, 255, 0), 0x6),
]


def nearest_color(rgb: Tuple[int, int, int]) -> int:
    r, g, b = rgb
    best_code = 0
    best_dist = None
    for _, (pr, pg, pb), code in PALETTE:
        dr = r - pr
        dg = g - pg
        db = b - pb
        dist = dr * dr + dg * dg + db * db
        if best_dist is None or dist < best_dist:
            best_dist = dist
            best_code = code
    return best_code


def nearest_color_rgb(rgb: Tuple[float, float, float]) -> Tuple[Tuple[int, int, int], int]:
    r, g, b = rgb
    best_code = 0
    best_dist = None
    best_rgb = (0, 0, 0)
    for _, (pr, pg, pb), code in PALETTE:
        dr = r - pr
        dg = g - pg
        db = b - pb
        dist = dr * dr + dg * dg + db * db
        if best_dist is None or dist < best_dist:
            best_dist = dist
            best_code = code
            best_rgb = (pr, pg, pb)
    return best_rgb, best_code


def dither_floyd_steinberg(img: Image.Image) -> List[List[Tuple[int, int, int]]]:
    width, height = img.size
    pixels = img.load()
    buf: List[List[Tuple[float, float, float]]] = [
        [tuple(map(float, pixels[x, y])) for x in range(width)]
        for y in range(height)
    ]
    out: List[List[Tuple[int, int, int]]] = [
        [(0, 0, 0) for _ in range(width)] for _ in range(height)
    ]

    for y in range(height):
        for x in range(width):
            old = buf[y][x]
            new_rgb, _ = nearest_color_rgb(old)
            out[y][x] = new_rgb
            err_r = old[0] - new_rgb[0]
            err_g = old[1] - new_rgb[1]
            err_b = old[2] - new_rgb[2]

            def add_error(nx: int, ny: int, factor: float) -> None:
                if 0 <= nx < width and 0 <= ny < height:
                    r, g, b = buf[ny][nx]
                    buf[ny][nx] = (
                        r + err_r * factor,
                        g + err_g * factor,
                        b + err_b * factor,
                    )

            add_error(x + 1, y, 7 / 16)
            add_error(x - 1, y + 1, 3 / 16)
            add_error(x, y + 1, 5 / 16)
            add_error(x + 1, y + 1, 1 / 16)

    return out


def apply_round_mask(img: Image.Image) -> None:
    size = img.size[0]
    cx = (size - 1) / 2.0
    cy = (size - 1) / 2.0
    radius = size / 2.0
    r2 = radius * radius
    pixels = img.load()
    for y in range(size):
        dy = y - cy
        for x in range(size):
            dx = x - cx
            if dx * dx + dy * dy > r2:
                pixels[x, y] = (255, 255, 255)


def image_to_bytes(img: Image.Image, dither: bool, packing: str) -> bytearray:
    size = img.size[0]
    if size % 2 != 0:
        raise ValueError("Image size must be even")
    half = size // 2
    if dither:
        dithered = dither_floyd_steinberg(img)
    else:
        pixels = img.load()
    out = bytearray()
    if packing == "sp6":
        for y in range(size - 1, -1, -1):
            pending = 0
            for x in range(size - 1, -1, -1):
                if dither:
                    value = nearest_color(dithered[y][x])
                else:
                    value = nearest_color(pixels[x, y])
                if (x & 1) == 0:
                    pending = value
                else:
                    out.append((pending << 4) | value)
        return out

    for x in range(size):
        for y in range(half):
            if dither:
                upper = nearest_color(dithered[y][x])
                lower = nearest_color(dithered[y + half][x])
            else:
                upper = nearest_color(pixels[x, y])
                lower = nearest_color(pixels[x, y + half])
            if packing == "byte":
                out.append((upper << 4) | upper)
                out.append((lower << 4) | lower)
            else:
                out.append((upper << 4) | lower)
    return out


def write_c_array(path: Path, var_name: str, data: Iterable[int]) -> None:
    data_list = list(data)
    lines = []
    lines.append(f"const unsigned char {var_name}[{len(data_list)}] = {{")
    for i in range(0, len(data_list), 16):
        chunk = data_list[i : i + 16]
        line = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(line + ",")
    lines.append("};")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", help="Input image (PNG/JPG/etc)")
    parser.add_argument("output", help="Output C header file")
    parser.add_argument("--name", default="gImage1", help="C array name")
    parser.add_argument("--size", type=int, default=400, help="Output size")
    parser.add_argument("--round-mask", action="store_true", help="Mask outside the circle to white")
    parser.add_argument(
        "--dither",
        choices=["none", "fs"],
        default="none",
        help="Dither method: none or fs (Floyd-Steinberg)",
    )
    parser.add_argument(
        "--no-flip-x",
        action="store_true",
        help="Disable default horizontal flip",
    )
    parser.add_argument(
        "--no-flip-y",
        action="store_true",
        help="Disable default vertical flip",
    )
    parser.add_argument(
        "--rotate",
        type=int,
        choices=[0, 90, 180, 270],
        default=0,
        help="Rotate image clockwise (degrees)",
    )
    parser.add_argument(
        "--packing",
        choices=["nibble", "byte", "sp6"],
        default="sp6",
        help="Output packing: sp6 (default), nibble, or byte (0x11/0x22/etc)",
    )
    args = parser.parse_args()

    img = Image.open(args.input).convert("RGB")
    if img.size != (args.size, args.size):
        img = img.resize((args.size, args.size), Image.LANCZOS)

    if not args.no_flip_x:
        img = img.transpose(Image.FLIP_LEFT_RIGHT)

    if not args.no_flip_y:
        img = img.transpose(Image.FLIP_TOP_BOTTOM)

    if args.rotate:
        img = img.rotate(-args.rotate, expand=False)

    if args.round_mask:
        apply_round_mask(img)

    data = image_to_bytes(img, dither=args.dither == "fs", packing=args.packing)
    expected = args.size * args.size // 2
    if args.packing == "byte":
        expected = args.size * args.size
    if len(data) != expected:
        raise RuntimeError(f"Unexpected output size: {len(data)} (expected {expected})")

    write_c_array(Path(args.output), args.name, data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
