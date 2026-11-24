"""Interpreted scanner/signature tests; no driver/toolchain build is needed."""

import importlib.util
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("hook_scan", ROOT / "tools" / "hook_scan.py")
hooks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hooks)


class HookScannerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = hooks.load_patterns()
        cls.patterns = cls.manifest["patterns"]

    def reference_bytes(self, pattern):
        return b"".join(struct.pack("<I", word["reference"]) for word in pattern["words"])

    def test_shipped_c_table_matches_manifest(self):
        self.assertEqual(hooks.GENERATED.read_text(encoding="utf-8"),
                         hooks.generated_source(self.manifest))

    def test_profile_scan_windows_cover_reference_hooks(self):
        profiles = (ROOT / "os_profiles.c").read_text(encoding="utf-8")
        windows = re.findall(
            r"\.scan_begin\s*=\s*(0x[0-9a-f]+),\s*"
            r"\.scan_end\s*=\s*(0x[0-9a-f]+)", profiles)
        self.assertEqual(len(windows), 6)
        for begin_text, end_text in windows:
            begin, end = int(begin_text, 16), int(end_text, 16)
            self.assertEqual((begin, end), (0x10300000, 0x10500000))
            for pattern in self.patterns:
                self.assertGreaterEqual(pattern["reference_address"], begin)
                self.assertLessEqual(pattern["reference_address"] +
                                     len(pattern["words"]) * 4, end)

    def test_all_reference_instruction_sequences(self):
        for pattern in self.patterns:
            with self.subTest(hook=pattern["id"]):
                data = self.reference_bytes(pattern)
                self.assertTrue(hooks.matches_at(data, 0, pattern))
                self.assertFalse(hooks.matches_at(data[:-1], 0, pattern))

    def test_captured_cas_candidates_and_profile(self):
        report = (ROOT / "tests/fixtures/usb-hooks-46-01.txt").read_text()
        profiles = (ROOT / "os_profiles.c").read_text()
        cas = profiles.split('.name = "CX II 6.2.0.333 CAS",', 1)[1].split(
            '.os_id = 47,', 1)[0]
        self.assertIn('.reviewed = 1', cas)
        self.assertIn('.abi = USB_OS_ABI_NUCLEUS_CX2_V1', cas)
        ncas = profiles.split('.name = "CX II 6.2.0.333 NCAS",', 1)[1].split(
            '.os_id = 45,', 1)[0]
        layout_fields = (
            'device_softc_offset', 'device_ivars_offset', 'usb_device_address_offset',
            'usb_device_speed_offset', 'pipe_endpoint_offset',
            'endpoint_descriptor_offset', 'high_speed_value')
        for field in layout_fields:
            expression = rf'\.{field}\s*=\s*(0x[0-9a-f]+|[0-9]+)'
            self.assertEqual(re.search(expression, cas).group(1),
                             re.search(expression, ncas).group(1), field)
        sections = report.split("\nHOOK ")[1:]
        self.assertEqual(len(sections), len(self.patterns))
        for pattern, section in zip(self.patterns, sections):
            self.assertTrue(section.startswith(pattern["name"] + " ["))
            candidates = re.findall(
                r"candidate = (0x[0-9a-f]+)\n((?:  actual[^\n]*\n)+)", section)
            self.assertEqual(len(candidates), 2 if pattern["id"] == "rename" else 1)
            for address, words in candidates:
                data = b"".join(struct.pack("<I", int(word, 16)) for word in
                                re.findall(r"actual \+0x[0-9a-f]+: ([0-9a-f]+)", words))
                self.assertTrue(hooks.matches_at(data, 0, pattern), address)
            selected = "0x104cbdb8" if len(candidates) == 2 else candidates[0][0]
            self.assertIn(f'[{hooks.enum_name(pattern)}] = {selected}', cas)
        self.assertIn("ops[17] = 0x104cbdb8", report)
        self.assertIn("parsedrive relation matches scan: yes", report)

    def test_known_offsets_in_synthetic_reference_image(self):
        base = 0x10000000
        end = max(p["reference_address"] + len(p["words"]) * 4 for p in self.patterns)
        image = bytearray(b"\xcc" * (end - base))
        for pattern in self.patterns:
            data = self.reference_bytes(pattern)
            offset = pattern["reference_address"] - base
            image[offset:offset + len(data)] = data
        results = hooks.scan(image, base, self.patterns)
        for pattern in self.patterns:
            self.assertEqual(results[pattern["id"]], [pattern["reference_address"]])

    def test_relocation_bits_are_ignored_but_opcodes_are_checked(self):
        for pattern in self.patterns:
            data = bytearray(self.reference_bytes(pattern))
            for index, word in enumerate(pattern["words"]):
                relocated = word["reference"] ^ (~word["mask"] & 0xFFFFFFFF)
                struct.pack_into("<I", data, index * 4, relocated)
            self.assertTrue(hooks.matches_at(data, 0, pattern))
            data[0] ^= 1
            self.assertFalse(hooks.matches_at(data, 0, pattern))

    def test_duplicate_missing_alignment_and_report(self):
        pattern = self.patterns[0]
        data = self.reference_bytes(pattern)
        image = bytearray(b"\xcc" * 8192)
        image[4092:4092 + len(data)] = data  # Crosses the C scanner's block boundary.
        results = hooks.scan(image, 0x10000000, self.patterns)
        self.assertEqual(results[pattern["id"]], [0x10000FFC])
        image[128:128 + len(data)] = data
        results = hooks.scan(image, 0x10000000, self.patterns)
        self.assertEqual(len(results[pattern["id"]]), 2)
        text = hooks.report(self.manifest, image, 0x10000000, results, 49)
        self.assertIn("[AMBIGUOUS] count=2", text)
        self.assertIn(".reviewed = 0", text)
        self.assertIn("[USB_HOOK_FINDMOUNT] = 0x00000000", text)
        self.assertIn("[MISSING]", text)
        self.assertEqual(hooks.scan(b"\xcc" + data, 0x10000000, [pattern])[pattern["id"]], [])

    def test_arm_decoders(self):
        self.assertEqual(hooks.decode_branch(0x10499C50, 0xEBFFFFDD), 0x10499BCC)
        self.assertEqual(hooks.decode_literal(0x10499C64, 0xE59F506C), 0x10499CD8)
        self.assertEqual(hooks.decode_literal(0x10000100, 0xE51F0010), 0x100000F8)
        with self.assertRaises(ValueError):
            hooks.decode_branch(0, 0xEAFFFFFB)
        with self.assertRaises(ValueError):
            hooks.read_word(b"\0" * 4, 0x10000000, 0x10000004)

    def test_offline_command_writes_report_without_modifying_dump(self):
        with tempfile.TemporaryDirectory() as directory:
            dump = Path(directory) / "sdram.bin"
            output = Path(directory) / "hooks.txt"
            original = self.reference_bytes(self.patterns[0])
            dump.write_bytes(original)
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "hook_scan.py"), "scan", str(dump),
                 "--os-id", "49", "--output", str(output)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertEqual(dump.read_bytes(), original)
            text = output.read_text(encoding="utf-8")
            self.assertIn("os_id = 49", text)
            self.assertIn("HOOK pc_findmount [UNIQUE] count=1", text)
            self.assertIn("[MISSING]", text)


if __name__ == "__main__":
    unittest.main()
