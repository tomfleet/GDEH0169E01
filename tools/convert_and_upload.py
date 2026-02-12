#!/usr/bin/env python3
"""Convert an image to raw sp6 bytes and upload to the ESP32."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def run(cmd: list[str]) -> None:
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        raise SystemExit(result.returncode)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", help="Input image (PNG/JPG/etc)")
    parser.add_argument("--url", default="http://espressif.lan/image", help="POST target URL")
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
        choices=["sp6", "nibble", "byte"],
        default="sp6",
        help="Output packing: sp6 (default), nibble, or byte",
    )
    parser.add_argument("--keep-raw", action="store_true", help="Keep the temporary raw file")
    args = parser.parse_args()

    tools_dir = Path(__file__).resolve().parent
    converter = tools_dir / "image_to_epd.py"
    uploader = tools_dir / "upload_image.py"

    with tempfile.NamedTemporaryFile(delete=False, suffix=".sp6") as tmp:
        raw_path = Path(tmp.name)

    cmd = [
        sys.executable,
        str(converter),
        args.input,
        str(raw_path),
        "--output-format",
        "raw",
        "--size",
        str(args.size),
        "--dither",
        args.dither,
        "--packing",
        args.packing,
    ]

    if args.round_mask:
        cmd.append("--round-mask")
    if args.no_flip_x:
        cmd.append("--no-flip-x")
    if args.no_flip_y:
        cmd.append("--no-flip-y")
    if args.rotate:
        cmd.extend(["--rotate", str(args.rotate)])

    run(cmd)

    run([
        sys.executable,
        str(uploader),
        str(raw_path),
        "--url",
        args.url,
    ])

    if args.keep_raw:
        print(f"Raw saved to: {raw_path}")
    else:
        raw_path.unlink(missing_ok=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
