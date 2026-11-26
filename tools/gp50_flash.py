#!/usr/bin/env python3
"""Check and install GP-50 DSP and control firmware through its factory USB loader (Linux)."""
import argparse
from collections import deque
from dataclasses import dataclass
from hashlib import sha256
from pathlib import Path
import os
import re
import runpy
import select
import stat
import sys
import time
import zipfile

import htfw
from gp5tool import NAMES, Problem, Timing
from gp5_proto import CLASS_CMD, build_sysex, build_update_stream, parse_sysex

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ((0x63, 0x200000), (0x62, 0))
STOCK_SHA256 = "a5f6b6fdf24da12788baddc90d6d383a545a8a755dca3650030372ba54932927"
LOADER_USB = ("84ef", "018b", "GP-50")
STOCK_USB = ("84ef", "018a", "GP-50")
OPEN_USB = ("1209", "0001", "GP-50 Open FW")
LOADER_STATE = b"\x11\x30\x01\x01"


def image_sections(blob):
    header = htfw.parse(blob)
    if header["product"] != "GP-50" or header["comp_flag"] or header["size_field"] != len(blob):
        raise ValueError("expected a complete, uncompressed GP-50 HTFW image")
    cursor = header["payload_base"]
    sections = []
    for entry in header["entries"]:
        start = header["payload_base"] + entry["doff"]
        end = start + entry["dlen"]
        if start != cursor or end <= start or end > len(blob) or entry["pad"]:
            raise ValueError(f"bad section layout at 0x{entry['type']:02x}")
        data = blob[start:end]
        if htfw.stored_crc(data) != entry["crc"]:
            raise ValueError(f"bad section CRC at 0x{entry['type']:02x}")
        sections.append(((entry["type"], entry["flash"]), data))
        cursor = end
    if cursor != len(blob):
        raise ValueError("trailing section data")
    return sections


def candidate(path):
    blob = path.read_bytes()
    sections = image_sections(blob)
    if tuple(key for key, _ in sections) != PROGRAM:
        raise ValueError("expected exactly the GP-50 DSP and app sections in that order")
    dsp, app = (data for _, data in sections)
    build_image = runpy.run_path(str(ROOT / "platform/nds32/pack.py"))["gp50_image"]
    if b"GP-50 Open FW" not in app or build_image(app, dsp) != blob:
        raise ValueError("expected an OpenGP5 experimental GP-50 firmware package")
    return tuple((kind, data) for (kind, _), data in sections)


def stock(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            members = [member for member in archive.infolist() if member.filename.lower().endswith(".bin")]
            if len(members) != 1 or members[0].file_size != 2261024:
                raise ValueError("expected one GP-50 V1.0.5 firmware binary in the ZIP")
            blob = archive.read(members[0])
    else:
        blob = path.read_bytes()
    if sha256(blob).hexdigest() != STOCK_SHA256:
        raise ValueError("firmware does not match the official GP-50 V1.0.5 recovery image")
    sections = dict(image_sections(blob))
    return tuple((kind, sections[(kind, address)]) for kind, address in PROGRAM)


def sys_text(path):
    return path.read_text(encoding="utf-8").strip()


@dataclass(frozen=True)
class Endpoint:
    node: Path
    usb_port: Path
    address: str
    rdev: int


def endpoints(identity, sys_sound=Path("/sys/class/sound"), proc_sound=Path("/proc/asound")):
    found = []
    for path in sys_sound.glob("midiC*D*"):
        match = re.fullmatch(r"midiC(\d+)D(\d+)", path.name)
        if match is None:
            continue
        try:
            device = path.resolve(strict=True)
            usb = next((p for p in device.parents if (p / "idVendor").is_file()), None)
            if usb is None or tuple(sys_text(usb / name) for name in ("idVendor", "idProduct", "product")) != identity:
                continue
            card, midi = match.groups()
            midi_info = sys_text(proc_sound / f"card{card}" / f"midi{midi}")
            if (midi_info.splitlines()[0] != identity[2] or
                    re.findall(r"(?m)^Input (\d+)$", midi_info) != ["0"] or
                    re.findall(r"(?m)^Output (\d+)$", midi_info) != ["0"]):
                continue
            major, minor = map(int, sys_text(path / "dev").split(":"))
            found.append(Endpoint(Path("/dev/snd") / path.name, usb, sys_text(usb / "devnum"), os.makedev(major, minor)))
        except (OSError, ValueError, IndexError):
            continue
    return found


class RawMidi:
    def __init__(self, device):
        self.fd = os.open(device.node, os.O_RDWR | os.O_NONBLOCK | os.O_NOFOLLOW)
        self.frames = deque()
        self.frame = bytearray()
        try:
            info = os.fstat(self.fd)
            if not stat.S_ISCHR(info.st_mode) or info.st_rdev != device.rdev or endpoints(LOADER_USB) != [device]:
                raise Problem("GP-50 loader USB identity changed while opening its MIDI port")
        except BaseException:
            os.close(self.fd)
            raise

    def close(self):
        os.close(self.fd)

    def flush(self):
        self.frames.clear()
        self.frame.clear()
        try:
            os.read(self.fd, 4096)
        except BlockingIOError:
            pass

    def send(self, raw):
        _, ready, _ = select.select([], [self.fd], [], Timing.reply)
        if not ready or os.write(self.fd, raw) != len(raw):
            raise Problem("could not send a complete MIDI frame to the GP-50 loader")

    def recv(self, timeout):
        end = time.monotonic() + timeout
        while not self.frames:
            remaining = end - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                return None
            try:
                chunk = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if not chunk:
                raise EOFError("GP-50 USB MIDI port closed")
            for byte in chunk:
                if byte == 0xF0:
                    self.frame[:] = b"\xf0"
                elif self.frame and byte == 0xF7:
                    self.frames.append(bytes(self.frame) + b"\xf7")
                    self.frame.clear()
                elif self.frame and byte <= 0x0F and len(self.frame) < 519:
                    self.frame.append(byte)
                elif byte < 0xF8:
                    self.frame.clear()
        return self.frames.popleft()


def reply(link, prefix, length, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        raw = link.recv(max(0, end - time.monotonic()))
        if raw is None:
            return None
        if len(raw) != 10 + 2 * length or raw[0] != 0xF0 or raw[-1] != 0xF7 or any(n > 0x0F for n in raw[1:-1]):
            continue
        cls, idx, size, payload, valid = parse_sysex(raw)
        if valid and (cls, idx, size) == (CLASS_CMD, 0, length) and payload.startswith(prefix):
            return payload
    return None


def check_loader(link):
    link.flush()
    link.send(build_sysex(CLASS_CMD, 0, b"\x12\x30"))
    if reply(link, b"\x11\x30", 4, Timing.reply) != LOADER_STATE:
        raise Problem("MIDI endpoint did not report GP-50 factory update mode")


def send_frame(link, raw, label, timeout, reboot=False):
    link.flush()
    link.send(raw)
    try:
        result = reply(link, b"\x14\x08", 3, timeout)
    except (OSError, EOFError):
        if reboot:
            return False
        raise
    if result is None and reboot:
        return False
    if result is None:
        raise Problem(f"no acknowledgement for {label}")
    if result[2] != 0:
        raise Problem(f"loader rejected {label} with status 0x{result[2]:02x}")
    return True


def run_session(link, sections, say):
    if tuple(kind for kind, _ in sections) != tuple(kind for kind, _ in PROGRAM):
        raise ValueError("update must contain exactly the GP-50 program sections")
    check_loader(link)
    enter_started = False
    erase_started = False
    try:
        for section_number, (kind, data) in enumerate(sections):
            name = NAMES[kind]
            stream = build_update_stream(data, kind)
            if section_number == 0:
                enter_started = True
                send_frame(link, stream[0][1], "start of update", Timing.ack)
                time.sleep(Timing.settle)
            say(f"{name}: sending {len(data):,} bytes in {len(stream) - 4:,} packets")
            erase_started = True
            send_frame(link, stream[1][1], f"begin {name} erase", Timing.begin)
            time.sleep(Timing.begin_settle)
            for index, (label, frame) in enumerate(stream[2:-2], 1):
                send_frame(link, frame, f"{name} {label}", Timing.ack)
                if index % 256 == 0:
                    say(f"{name}: acknowledged {index:,}/{len(stream) - 4:,} packets")
            send_frame(link, stream[-2][1], f"finish {name}", Timing.ack)
            say(f"{name}: all packets and section end acknowledged")
            time.sleep(Timing.settle)
        return send_frame(link, stream[-1][1], "end of update", Timing.final, reboot=True)
    except (Problem, OSError, EOFError) as error:
        if erase_started:
            raise Problem(f"{error}. A program section may already have been erased or written. Update stopped.") from error
        if enter_started:
            raise Problem(f"{error}. The loader may have entered an update session. Update stopped.") from error
        raise


def say(line):
    print(line, flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("status", help="query a GP-50 already in factory loader mode")
    sub.add_parser("check", help="validate an experimental GP-50 package offline").add_argument("image", type=Path)
    sub.add_parser("check-stock", help="validate the GP-50 V1.0.5 recovery package offline").add_argument("stock", type=Path)
    flash = sub.add_parser("flash", help="install only the experimental app and DSP")
    flash.add_argument("image", type=Path)
    flash.add_argument("--stock", type=Path, required=True, help="verify this GP-50 V1.0.5 recovery package first")
    sub.add_parser("restore", help="restore the stock V1.0.5 app and DSP only").add_argument("stock", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.action == "flash":
            stock(args.stock)
        if args.action == "status":
            sections = None
        else:
            sections = candidate(args.image) if args.action in ("check", "flash") else stock(args.stock)
            for kind, data in sections:
                say(f"{NAMES[kind]}: validated {len(data):,} bytes")
            if args.action in ("check", "check-stock"):
                return 0
        if not sys.platform.startswith("linux"):
            raise Problem("GP-50 loader access requires Linux ALSA raw MIDI")
        devices = endpoints(LOADER_USB)
        if len(devices) != 1:
            raise Problem("expected exactly one GP-50 factory loader (84ef:018b) with a duplex MIDI port")
        device = devices[0]
        link = RawMidi(device)
        try:
            if args.action == "status":
                check_loader(link)
                say(f"GP-50 factory update mode at {device.usb_port.name} ({device.node})")
                return 0
            acknowledged = run_session(link, sections, say)
        finally:
            link.close()
        if not acknowledged:
            say("Both sections acknowledged. The final restart reply was unavailable.")
        expected = OPEN_USB if args.action == "flash" else STOCK_USB
        end = time.monotonic() + Timing.reenum
        while time.monotonic() < end:
            if any(item.usb_port == device.usb_port for item in endpoints(expected)):
                say(f"{expected[2]} reappeared on USB at {device.usb_port.name}")
                return 0
            time.sleep(Timing.poll)
        raise Problem(f"both sections were acknowledged, but {expected[2]} did not reappear on the same USB port")
    except (OSError, EOFError, Problem, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    sys.exit(main())
