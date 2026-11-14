#!/usr/bin/env python3
"""Regress the counted trust-cache index before constructing a Data seed."""
import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "prepare_data_seed", Path(__file__).with_name("prepare-data-seed.py"))
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


def module(*hashes):
    return (struct.pack("<I16sI", 2, b"\0" * 16, len(hashes))
            + b"".join(h + bytes([2, 0, 0, 0]) for h in hashes))


class TrustBundleTests(unittest.TestCase):
    def setUp(self):
        self.owner = module(b"\x01" * 20)
        self.static = module(b"\x02" * 20)
        self.system = module(b"\x03" * 20)

    def test_original_two_module_format(self):
        bundle = seed._pack_trust_modules([self.static, self.system])
        self.assertEqual(struct.unpack_from("<3I", bundle), (2, 12, 60))
        self.assertEqual(bundle[12:], self.static + self.system)

    def test_owner_three_module_format(self):
        bundle = seed._pack_trust_modules([self.owner, self.static, self.system])
        self.assertEqual(struct.unpack_from("<4I", bundle), (3, 16, 64, 112))
        self.assertEqual(bundle[16:], self.owner + self.static + self.system)

    def test_m62_wrong_count_and_offsets_are_rejected(self):
        bundle = (struct.pack("<4I", 2, 12, 60, 108)
                  + self.owner + self.static + self.system)
        with self.assertRaises(ValueError):
            seed._validate_trust_bundle(bundle)

    def test_malformed_modules_are_rejected(self):
        for modules in ([], [self.owner[:-1]],
                        [module(b"\x02" * 20, b"\x01" * 20)],
                        [module(b"\x01" * 20, b"\x01" * 20)]):
            with self.subTest(modules=modules), self.assertRaises(ValueError):
                seed._pack_trust_modules(modules)
        bad_type = self.owner[:44] + b"\0" + self.owner[45:]
        with self.assertRaises(ValueError):
            seed._pack_trust_modules([bad_type])

    def test_production_builder_uses_the_counted_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "trustcache.bundle"
            inputs = {"StaticTrustCache": self.static,
                      "Cryptex1,SystemTrustCache": self.system}
            ipsw = SimpleNamespace(read=inputs.__getitem__)
            with patch.object(seed, "open_ipsw", return_value=(ipsw, {})), \
                 patch.object(seed, "im4p_payload", side_effect=lambda value, _: value), \
                 patch.object(seed, "_cdhash", return_value=b"\x01" * 20):
                seed.build_trust_bundle(Path(tmp), None, output, Path(tmp) / "watchdogd")
            bundle = output.read_bytes()
            self.assertEqual(struct.unpack_from("<4I", bundle), (3, 16, 64, 112))
            seed._validate_trust_bundle(bundle)
            # A trusted Data-backed daemon still needs an explicit launch
            # policy. Do not regress to category 0 or copy the System-only 1.
            self.assertEqual(bundle[40:60], b"\x01" * 20)
            self.assertEqual(bundle[60:64], bytes([2, 0, 5, 0]))
            self.assertEqual(bundle[64:], self.static + self.system)

    def test_pin_mismatch_stops_before_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "trustcache.bundle"
            inputs = {"StaticTrustCache": self.static,
                      "Cryptex1,SystemTrustCache": self.system}
            board = {"payload": {"trust_cache_sha256": ["0" * 64]}}
            ipsw = SimpleNamespace(read=inputs.__getitem__)
            with patch.object(seed, "open_ipsw", return_value=(ipsw, board)), \
                 patch.object(seed, "im4p_payload", side_effect=lambda value, _: value), \
                 self.assertRaises(SystemExit):
                seed.build_trust_bundle(Path(tmp), None, output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
