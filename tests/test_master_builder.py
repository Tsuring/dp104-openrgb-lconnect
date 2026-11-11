"""Hardware-free regressions for final master-image composition."""
import subprocess
import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build_master_payload as builder
import wsl_paths
from plan_firmware_hooks import image_offset, rv_address_pair


class WslPathTests(unittest.TestCase):
    def test_resolves_absolute_path_through_ubuntu_wslpath(self):
        path = Path(__file__).resolve().parents[1] / "build" / "sample.elf"
        converted = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="/mnt/custom/project/build/sample.elf\n", stderr=""
        )
        command = ["wsl.exe", "-d", "Ubuntu", "--exec", "wslpath", "-a", "-u",
                   str(path.resolve())]

        with patch.object(wsl_paths.subprocess, "run", return_value=converted) as run:
            self.assertEqual(wsl_paths.to_wsl_path(path), "/mnt/custom/project/build/sample.elf")

        run.assert_called_once_with(command, capture_output=True, text=True, check=False)

    def test_reports_wslpath_failure(self):
        failed = subprocess.CompletedProcess(
            args=[], returncode=1, stdout="", stderr="mount unavailable\n"
        )
        with patch.object(wsl_paths.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(wsl_paths.WslPathError, "mount unavailable"):
                wsl_paths.to_wsl_path(Path("build/sample.elf"))


class MasterBuilderTests(unittest.TestCase):
    def setUp(self):
        self.raw = bytearray([0x55]) * 256000
        self.floor = 0x20013C00
        self.recipes = []
        for address, old, new in (
            (builder.BSS_END_PAIR, rv_address_pair(builder.BSS_END_PAIR, 0x20012A04, 11),
             rv_address_pair(builder.BSS_END_PAIR, self.floor, 11)),
            (builder.HEAP_FLOOR_PAIR, rv_address_pair(builder.HEAP_FLOOR_PAIR, 0x20012A04, 14),
             rv_address_pair(builder.HEAP_FLOOR_PAIR, self.floor, 14)),
            (builder.INITIAL_HEAP_WORD, struct.pack("<I", 0x20012A04), struct.pack("<I", self.floor)),
        ):
            offset = image_offset("master", address, len(old), self.raw)
            self.raw[offset:offset + len(old)] = old
            self.recipes.append({"role": "master", "runtimeAddress": hex(address),
                                 "imageOffset": offset, "originalHex": old.hex(),
                                 "replacementHex": new.hex(), "purpose": "test reservation"})

    def compose(self):
        windows = {".copy_shim": bytes([0x11]) * 44, ".post_bss": bytes([0x22]) * 8,
                   ".taskcall": bytes([0x33]) * 8, ".uart_callback": bytes([0x44]) * 8}
        plan = {"reservations": {"master": {"newBssEnd": hex(self.floor)}},
                "reservationRecipesNotApplied": self.recipes}
        patched, _, _ = builder.apply_payload_patches(
            bytes(self.raw), 0x8000, plan, windows, bytes([0x66]) * 32,
            bytes([0x77]) * 16, builder.SUFFIX_FLASH_START + 32,
            builder.SUFFIX_FLASH_START + 48, bytes([0x88]) * 16)
        return patched, windows

    def test_packing_preserves_every_flat_reservation_and_hook(self):
        patched, windows = self.compose()
        for recipe in self.recipes:
            offset = recipe["imageOffset"]
            expected = bytes.fromhex(recipe["replacementHex"])
            self.assertEqual(patched[offset:offset + len(expected)], expected)
        for name, address in ((".copy_shim", builder.COPY_SHIM_SITE),
                              (".post_bss", builder.POST_BSS_SITE),
                              (".taskcall", builder.TASK_CALL_SITE),
                              (".uart_callback", builder.UART_CALLBACK_PAIR)):
            offset = image_offset("master", address, len(windows[name]), patched)
            self.assertEqual(patched[offset:offset + len(windows[name])], windows[name])

    def test_data_check_reads_actual_heap_word_and_rejects_corruption(self):
        patched, _ = self.compose()
        result = builder.verify_data_lma_preserved(bytes(self.raw), patched, 0x8000, self.floor)
        self.assertEqual(result["heap_word_new"], self.floor)
        damaged = bytearray(patched)
        damaged[builder.DATA_LMA_START - 0x8000] ^= 1
        with self.assertRaises(builder.RestoreBuildError):
            builder.verify_data_lma_preserved(bytes(self.raw), bytes(damaged), 0x8000, self.floor)
        with self.assertRaises(builder.RestoreBuildError):
            builder.verify_data_lma_preserved(bytes(self.raw), patched, 0x8000, self.floor + 16)


if __name__ == "__main__":
    unittest.main()
