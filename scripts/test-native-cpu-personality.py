#!/usr/bin/env python3
"""Validate the native CPU matching fix against a real prelinked kernel."""
import argparse
import copy
import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "cpu_personality_patch", Path(__file__).with_name("patch-native-cpu-personality.py"))
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("kernel", type=Path)
    parser.add_argument("--prelink-tools", type=Path,
                        default=Path.cwd() / "kits/XnuPrelink")
    args = parser.parse_args()
    data = args.kernel.read_bytes()
    tools = patcher.prelink_tools(args.prelink_tools)
    native = tools[0]
    image = native.macho(data)
    region = next(s for s in image["segments"] if s["name"] == "__PRELINK_INFO")
    offset = native.backed_offset(image["segments"], region["va"], region["filesize"], len(data))
    original_xml = data[offset:offset + region["filesize"]].split(b"\0", 1)[0]
    original_metadata = native.unserialize_xml(original_xml)

    class CpuPersonalityTests(unittest.TestCase):
        def test_only_apple_cpu_automatic_match_is_removed(self):
            changed, report = patcher.patch_kernel(data, tools)
            self.assertTrue(report["changed"])
            expected = copy.deepcopy(original_metadata)
            entry = next(e for e in expected["_PrelinkInfoDictionary"]
                         if e["CFBundleIdentifier"] == patcher.IDENTIFIER)
            del entry["IOKitPersonalities"][patcher.PERSONALITY]
            actual = native.unserialize_xml(changed[offset:offset + region["filesize"]])
            self.assertEqual(actual, expected)

        def test_executable_symbols_and_addresses_are_unchanged(self):
            changed, _ = patcher.patch_kernel(data, tools)
            self.assertEqual(len(changed), len(data))
            self.assertEqual(changed[:offset], data[:offset])
            self.assertEqual(changed[offset + region["filesize"]:], data[offset + region["filesize"]:])
            self.assertEqual(native.macho(changed), image)
            self.assertEqual(native.exports(changed, image), native.exports(data, image))

        def test_patch_is_idempotent(self):
            changed, _ = patcher.patch_kernel(data, tools)
            again, report = patcher.patch_kernel(changed, tools)
            self.assertEqual(again, changed)
            self.assertFalse(report["changed"])

        def test_absent_native_driver_is_rejected(self):
            marker = b"__ZTV6ArmCpu\0"
            self.assertEqual(data.count(marker), 1)
            invalid = data.replace(marker, b"__ZTV6BadCpu\0")
            with self.assertRaises(ValueError):
                patcher.patch_kernel(invalid, tools)

        def test_unknown_vendor_matching_contract_is_rejected(self):
            marker = b'<key>AppleARMCPU</key><dict><key>IOClass</key><string>AppleARMCPU</string>'
            at = original_xml.index(marker)
            end = original_xml.index(b"</dict>", at)
            altered = original_xml[:at] + original_xml[at:end].replace(
                b'<string>cpu</string>', b'<string>gpu</string>') + original_xml[end:]
            invalid = data[:offset] + altered + bytes(region["filesize"] - len(altered)) + data[offset + region["filesize"]:]
            with self.assertRaises(ValueError):
                patcher.patch_kernel(invalid, tools)

    result = unittest.TextTestRunner(verbosity=1).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(CpuPersonalityTests))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
