#!/usr/bin/env python3
"""Verify owner addon records, signed code pages and exact ADT preservation."""
import argparse
import importlib.util
from pathlib import Path
import unittest

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--artifact',type=Path,required=True)
p.add_argument('--adt',type=Path,required=True)
p.add_argument('--core',type=Path,required=True)
a=p.parse_args()
script=Path(__file__).resolve().parents[1]/'make-mali-metal-addon.py'
s=importlib.util.spec_from_file_location('addon',script)
m=importlib.util.module_from_spec(s);s.loader.exec_module(m)

class Tests(unittest.TestCase):
    def test_records(self):
        files=[('bin/own-probe',0o755,b'probe'),('daemons/own.plist',0o644,b'config')]
        pad=lambda b:b+bytes(-len(b)%8)
        encoded=m.encode(files,pad)
        self.assertEqual(m.decode(encoded),files)
        with self.assertRaises(ValueError):m.decode(encoded+b'extra')
        with self.assertRaises(ValueError):m.encode([('../escape',0o755,b'x')],pad)
        with self.assertRaises(ValueError):m.encode(files+files,pad)

    def test_actual_adt(self):
        original=a.adt.read_bytes();modified=m.addon_adt(original,a.core.resolve())
        self.assertEqual(len(modified),len(original)+52)
        with self.assertRaises(ValueError):m.addon_adt(modified,a.core.resolve())
        with self.assertRaises((ValueError,UnicodeDecodeError)):m.addon_adt(original[:-1],a.core.resolve())

    def test_signed_program(self):
        raw=(a.artifact/'viva-gpu/ios/out/tests/native-metal-probe').read_bytes()
        result=m.code_directory(raw,True)
        self.assertEqual(result['entitlements'],m.ENTITLEMENTS)
        self.assertGreater(result['code_pages'],0)
        changed=bytearray(raw);changed[0x4000]^=1
        with self.assertRaises(ValueError):m.code_directory(changed,True)

    def test_signed_bundle(self):
        raw=(a.artifact/'viva-gpu/ios/out/VivaMTLDriver.bundle/VivaMTLDriver').read_bytes()
        result=m.code_directory(raw,False)
        self.assertGreater(result['code_pages'],500)
        changed=bytearray(raw);changed[0x8000]^=1
        with self.assertRaises(ValueError):m.code_directory(changed,False)
        with self.assertRaises(ValueError):m.code_directory(raw,True)

unittest.main(argv=['test-mali-metal-addon'],verbosity=2)
