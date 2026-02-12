#!/usr/bin/env python3
"""Upload raw sp6 image bytes to the ESP32 HTTP endpoint."""

from __future__ import annotations

import argparse
from pathlib import Path
from urllib.request import Request, urlopen


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("raw", help="Raw sp6 file to upload")
    parser.add_argument("--url", default="http://espressif.lan/image", help="POST target URL")
    args = parser.parse_args()

    data = Path(args.raw).read_bytes()
    req = Request(args.url, data=data, method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    req.add_header("Content-Length", str(len(data)))

    with urlopen(req, timeout=30) as resp:
        body = resp.read().decode("utf-8", errors="ignore")
        print(body)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
