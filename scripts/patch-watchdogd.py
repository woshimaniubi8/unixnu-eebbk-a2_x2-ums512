#!/usr/bin/env python3
"""Patch the verified watchdogd gates for the offline BBK A2 port.

The original IPSW watchdogd (SHA-256 below) checks
os_variant_has_internal_diagnostics() before reading -wdt_no_pmu from
kern.bootargs.  The 119.log run proved the argument is present but not
consumed, so the exact CBZ at RVA 0xA114 is replaced with NOP.

The same binary carries the names of services that the watchdogd monitors.
This board has no cellular/audio userspace contract, so CommCenter and
audiomxd are renamed in the binary's own service-name table; they are not
reported as unresponsive and are not killed by the userspace watchdog.  The
SpringBoard/backboardd GPU services remain monitored.  This is not a fake
checkin and does not change the kernel IOWatchdog assertions.

Re-signing with the original entitlements (notably
com.apple.private.iowatchdog.user-access) is a separate macOS step and is
mandatory before deployment.
"""
import hashlib
import pathlib
import sys

EXPECTED_SHA256 = "ded4c688e1c2add74acf3e7f9eea5fe2c2c569180ce81492221877944d913a01"
OFFSET = 0xA114
OLD = bytes.fromhex("40010034")  # cbz w0, 0x10000a13c
NEW = bytes.fromhex("1f2003d5")  # nop
SERVICE_RENAMES = (
    (b"CommCenter", b"XommCenter"),
    (b"audiomxd", b"Xudiomxd"),
)


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: patch-watchdogd.py <watchdogd> [output]")
    source = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2]) if len(sys.argv) == 3 else source.with_name(source.name + ".patched")
    data = bytearray(source.read_bytes())
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_SHA256:
        raise SystemExit(f"unexpected watchdogd SHA-256: {digest}")
    before = bytes(data[OFFSET:OFFSET + len(OLD)])
    if before != OLD:
        raise SystemExit(f"unexpected bytes at 0x{OFFSET:x}: {before.hex()} (want {OLD.hex()})")
    data[OFFSET:OFFSET + len(NEW)] = NEW
    print(f"watchdogd: 0x{OFFSET:x} {OLD.hex()} -> {NEW.hex()}")
    for old, new in SERVICE_RENAMES:
        count = data.count(old)
        if count == 0:
            raise SystemExit(f"watchdogd: service string not found: {old!r}")
        data = data.replace(old, new)
        print(f"watchdogd: service {old.decode()} -> {new.decode()} ({count} occurrence(s))")
    output.write_bytes(data)
    after = hashlib.sha256(data).hexdigest()
    print(f"watchdogd: {digest} -> {after}")
    print(f"watchdogd: wrote {output}")


if __name__ == "__main__":
    main()
