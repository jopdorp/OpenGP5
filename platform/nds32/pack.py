#!/usr/bin/env python3
"""Check the control binary and package it as HTFW. No device access or stock image required.

  1. checks the image header our start-up code (start/vectors.S) emits at 0xA4..0x105 against
     header_a4_ff.bin (the layout the factory loader reads: const/user data addresses, chip id, magic, ...)
  2. checks what the factory loader checks (check_user_code @0x4CBA): every word in 0x00..0xA3 starts
     with 0x48 (`j`) and lands inside the image, word at 0xC0 == 0xB0BEBDC9; plus size <= 0x168000
  3. writes the GP-5 standalone app or wraps the GP-50 app with its DSP in one image
"""
import argparse, struct, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import gp5tool
import htfw

HDR0 = 0xA4


def validate_app(app):
    errors = gp5tool.check_app(app)
    if errors:
        raise ValueError("; ".join(errors))
    want = (HERE / "header_a4_ff.bin").read_bytes() + b"\xff\xff\xff\x00\xff\xff\xdd\x9e"
    if app[HDR0:0x108] != want:
        raise ValueError("control app header or return stub differs from the stock loader template")
    if len(app) > 0x168000:
        raise ValueError("control app exceeds the stock header's 0x168000-byte slot")
    for offset in range(0, HDR0, 4):
        word = int.from_bytes(app[offset:offset + 4], "big")
        if not 0x106 <= offset + ((word & 0xFFFFFF) << 1) < len(app):
            raise ValueError(f"control app vector {offset:#x} targets outside its code")


def gp50_image(app, dsp):
    validate_app(app)
    if len(app) > 0x155338:
        raise ValueError("control app exceeds the GP-50 V1.0.5 app section")
    dsp_header = b"\xff" * 8 + b"V105\x00\x44\x00\x00" + bytes(0x400 - 16)
    if len(dsp) != 0xB874 or dsp[:0x400] != dsp_header:
        raise ValueError("expected a GP-50 V1.0.5-layout raw DSP section")
    errors = gp5tool.check_dsp(dsp)
    if errors:
        raise ValueError("; ".join(errors))
    reset_offset = (struct.unpack_from("<I", dsp, 0x404)[0] & ~1) - 0x4000
    if reset_offset < 0x500 or dsp[reset_offset:reset_offset + 2] == b"\xff\xff":
        raise ValueError("DSP reset vector does not target program code")
    header = bytearray(htfw.HEADER_LEN)
    # Match the fixed fields in the V1.0.5 container. Use a custom image version.
    struct.pack_into("<IHH", header, 0, htfw.MAGIC, 0xA0C6, 1)
    header[0x0C:0x11] = b"GP-50"
    header[0x1C] = ord("V")
    return htfw.pack_uncompressed(header, ((0x63, 0x200000, 0, dsp), (0x62, 0, 0, app)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gp50", nargs=3, metavar=("RAW_APP", "RAW_DSP", "OUTPUT"), type=Path)
    args = parser.parse_args()
    out = HERE / "out"
    try:
        if args.gp50:
            app, dsp, output = args.gp50
            image = gp50_image(app.read_bytes(), dsp.read_bytes())
            output.write_bytes(image)
            print(f"Experimental GP-50 HTFW package: {output} ({len(image)} bytes)")
            return
        app = (out / "app.bin").read_bytes()
        validate_app(app)
        app_path = out / "app_0x62.bin"
        image_path = out / "gp5-standalone-app.bin"
        app_path.write_bytes(app)
        image_path.write_bytes(gp5tool.make_image({0x62: app}))
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"OK  app {len(app)} bytes ({len(app):#x}), reset -> {0x10000 + ((int.from_bytes(app[0:4], 'big') & 0xFFFFFF) << 1):#x}")
    print(f"    {app_path.relative_to(ROOT)}\n    {image_path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
