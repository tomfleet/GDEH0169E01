#!/usr/bin/env python3
"""Upload raw sp6 image bytes to the ESP32 HTTP endpoint."""

from __future__ import annotations

import argparse
from pathlib import Path
from urllib.request import Request, urlopen


RLE_MAGIC = b"SP6R"
HS_MAGIC = b"HSK1"


def encode_heatshrink_wasm(raw: bytes, wasm_path: Path, window_bits: int,
                           lookahead_bits: int) -> bytes:
    try:
        from wasmtime import Instance, Module, Store
    except ImportError as exc:
        raise SystemExit("Install wasmtime: pip install wasmtime") from exc

    store = Store()
    module = Module.from_file(store.engine, str(wasm_path))
    instance = Instance(store, module, [])
    exports = instance.exports(store)

    hs_alloc = exports["hs_alloc"]
    hs_free = exports["hs_free"]
    hs_encode = exports["hs_encode"]
    memory = exports["memory"]

    out_cap = len(raw) + max(64, len(raw) // 8)
    in_ptr = hs_alloc(store, len(raw))
    out_ptr = hs_alloc(store, out_cap)
    if in_ptr == 0 or out_ptr == 0:
        if in_ptr:
            hs_free(store, in_ptr)
        if out_ptr:
            hs_free(store, out_ptr)
        raise SystemExit("heatshrink wasm allocation failed")

    memory.write(store, raw, in_ptr)
    out_size = hs_encode(store, in_ptr, len(raw), out_ptr, out_cap,
                         window_bits, lookahead_bits)
    if out_size <= 0:
        hs_free(store, in_ptr)
        hs_free(store, out_ptr)
        raise SystemExit("heatshrink wasm encoding failed")

    payload = memory.read(store, out_ptr, out_ptr + out_size)
    hs_free(store, in_ptr)
    hs_free(store, out_ptr)

    header = HS_MAGIC + len(raw).to_bytes(4, "little") + bytes([
        window_bits & 0xFF,
        lookahead_bits & 0xFF,
    ])
    return header + payload


def rle_encode_sp6_nibbles(raw: bytes) -> bytes:
    out = bytearray()
    out.extend(RLE_MAGIC)
    out.extend(len(raw).to_bytes(4, "little"))

    run_value = None
    run_length = 0

    def flush() -> None:
        nonlocal run_length, run_value
        if run_length == 0:
            return
        out.append(run_length & 0xFF)
        out.append(run_value & 0x0F)
        run_length = 0

    for byte in raw:
        for nibble in ((byte >> 4) & 0x0F, byte & 0x0F):
            if run_value is None:
                run_value = nibble
                run_length = 1
                continue
            if nibble == run_value and run_length < 255:
                run_length += 1
            else:
                flush()
                run_value = nibble
                run_length = 1

    flush()
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("raw", help="Raw sp6 file to upload")
    parser.add_argument("--url", default="http://espressif.lan/image", help="POST target URL")
    parser.add_argument("--rle", action="store_true", help="Upload with nibble RLE")
    parser.add_argument("--heatshrink-wasm", action="store_true",
                        help="Upload with heatshrink (WASM encoder)")
    parser.add_argument("--wasm", default="",
                        help="Path to heatshrink.wasm for encoding")
    parser.add_argument("--window-bits", type=int, default=10,
                        help="Heatshrink window bits")
    parser.add_argument("--lookahead-bits", type=int, default=4,
                        help="Heatshrink lookahead bits")
    args = parser.parse_args()

    data = Path(args.raw).read_bytes()
    if args.heatshrink_wasm:
        wasm_path = Path(args.wasm) if args.wasm else Path(__file__).with_name("heatshrink.wasm")
        if not wasm_path.exists():
            raise SystemExit(f"WASM not found: {wasm_path}")
        data = encode_heatshrink_wasm(data, wasm_path, args.window_bits, args.lookahead_bits)
    elif args.rle:
        data = rle_encode_sp6_nibbles(data)
    req = Request(args.url, data=data, method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    req.add_header("Content-Length", str(len(data)))

    with urlopen(req, timeout=30) as resp:
        body = resp.read().decode("utf-8", errors="ignore")
        print(body)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
