"""Exercise GP-50 flash planning and the update transcript without USB or MIDI."""
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path
from runpy import run_path
from tempfile import TemporaryDirectory
from unittest import TestCase
from unittest.mock import patch

import gp50_flash as flash
from gp5_proto import CLASS_CMD, build_sysex
from test_gp5tool import FakeLink, FakePedal, MODE_LOADER, MODE_STOCK, tiny_gp50_programs


class CapturedLoader(FakePedal):
    def __init__(self, mode=MODE_LOADER, stall_begin=False):
        super().__init__(mode=mode, stall_begin=stall_begin)
        self.frames = []

    def handle(self, raw):
        self.frames.append(raw)
        return super().handle(raw)


class WriteSession(TestCase):
    def setUp(self):
        timer = patch.multiple(flash.Timing, settle=0, begin_settle=0)
        timer.start()
        self.addCleanup(timer.stop)
        self.programs = ((0x63, bytes(range(256)) * 20), (0x62, b"control firmware"))

    def test_program_pair_preserves_other_sections_and_restarts(self):
        loader = CapturedLoader()
        nonprogram = {0x67: b"DSP data", 0x66: b"models", 0x65: b"presets"}
        loader.written.update(nonprogram)
        self.assertTrue(flash.run_session(FakeLink(loader), self.programs, lambda _: None))
        self.assertEqual(loader.written, {**nonprogram, **dict(self.programs)})
        self.assertTrue(loader.ended)
        self.assertEqual(loader.frames[:3], [
            bytes.fromhex("f0050900010000000201020300f7"),
            build_sysex(CLASS_CMD, 0, bytes.fromhex("1160")),
            build_sysex(CLASS_CMD, 0, bytes.fromhex("1161630000000e01000002140000")),
        ])

    def test_stock_state_does_not_start_a_write(self):
        loader = CapturedLoader(mode=MODE_STOCK)
        with self.assertRaisesRegex(flash.Problem, "did not report GP-50 factory update mode"):
            flash.run_session(FakeLink(loader), self.programs, lambda _: None)
        self.assertEqual(len(loader.frames), 1)
        self.assertEqual(loader.written, {})

    def test_lost_enter_ack_stops_without_begin(self):
        class SilentEnter(CapturedLoader):
            def handle(self, raw):
                replies = super().handle(raw)
                return [] if self.sent_cmds[-1] == bytes.fromhex("1160") else replies

        loader = SilentEnter()
        with self.assertRaisesRegex(flash.Problem, "may have entered an update session"):
            flash.run_session(FakeLink(loader), self.programs, lambda _: None)
        self.assertEqual(loader.sent_cmds, [bytes.fromhex("1230"), bytes.fromhex("1160")])
        self.assertFalse(loader.ended)

    def test_lost_begin_ack_does_not_repeat_erase_or_restart(self):
        loader = CapturedLoader(stall_begin=True)
        with self.assertRaisesRegex(flash.Problem, "may already have been erased"):
            flash.run_session(FakeLink(loader), self.programs, lambda _: None)
        self.assertEqual(loader.sent_cmds, [bytes.fromhex("1230"), bytes.fromhex("1160"), bytes.fromhex("1161")])
        self.assertFalse(loader.ended)

    def test_refuses_nonprogram_section_before_querying(self):
        loader = CapturedLoader()
        with self.assertRaises(ValueError):
            flash.run_session(FakeLink(loader), self.programs + ((0x65, b"presets"),), lambda _: None)
        self.assertEqual(loader.frames, [])


class UsbEndpoint(TestCase):
    def test_matching_names_do_not_override_usb_parent_identity(self):
        with TemporaryDirectory() as temp:
            root = Path(temp)
            sys_sound = root / "sys"
            sys_sound.mkdir()
            proc_sound = root / "proc"
            for card, pid in enumerate(("018b", "018a", "0184")):
                usb = root / f"1-{card}"
                midi = usb / "iface" / f"midiC{card}D0"
                midi.mkdir(parents=True)
                (usb / "idVendor").write_text("84ef\n")
                (usb / "idProduct").write_text(pid + "\n")
                (usb / "product").write_text("GP-50\n")
                (usb / "devnum").write_text(f"{card + 2}\n")
                (midi / "dev").write_text(f"116:{20 + card}\n")
                (sys_sound / midi.name).symlink_to(midi)
                card_path = proc_sound / f"card{card}"
                card_path.mkdir(parents=True)
                (card_path / "midi0").write_text("GP-50\n\nType: Legacy\nOutput 0\nInput 0\n")
            devices = flash.endpoints(flash.LOADER_USB, sys_sound, proc_sound)
            self.assertEqual([item.usb_port for item in devices], [root / "1-0"])


class ReadOnlyCommands(TestCase):
    def test_flash_rejects_app_overlapping_journal_before_discovery(self):
        pack = run_path(str(flash.ROOT / "platform" / "nds32" / "pack.py"))["gp50_image"]
        app, dsp = tiny_gp50_programs()
        limit = 0x153000
        image = pack(app.ljust(limit, b"\xff"), dsp)
        oversized = flash.htfw.pack_uncompressed(image[:flash.htfw.HEADER_LEN], (
            (0x63, 0x200000, 0, dsp), (0x62, 0, 0, app.ljust(limit + 1, b"\xff"))))
        with TemporaryDirectory() as temp:
            path = Path(temp) / "overlap.bin"
            path.write_bytes(oversized)
            errors = StringIO()
            with patch.object(flash, "stock"), redirect_stderr(errors), \
                    patch.object(flash, "endpoints", side_effect=AssertionError("MIDI discovery")):
                with self.assertRaises(SystemExit) as stopped:
                    flash.main(["flash", str(path), "--stock", "stock.zip"])
            self.assertEqual(stopped.exception.code, 1)
            self.assertIn("overlaps the GP-50 storage journal", errors.getvalue())

    def test_offline_checks_do_not_discover_or_open_midi(self):
        with patch.object(flash, "candidate", return_value=((0x63, b"dsp"),)), \
                patch.object(flash, "stock", return_value=((0x62, b"app"),)), \
                patch.object(flash, "endpoints", side_effect=AssertionError("MIDI discovery")), \
                patch.object(flash, "RawMidi", side_effect=AssertionError("MIDI access")), \
                patch.object(flash, "say"):
            self.assertEqual(flash.main(["check", "image.bin"]), 0)
            self.assertEqual(flash.main(["check-stock", "stock.zip"]), 0)
