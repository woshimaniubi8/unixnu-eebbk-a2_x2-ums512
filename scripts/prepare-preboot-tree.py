#!/usr/bin/env python3
"""Put the original Preboot firmware tree into the OwnerData seed (macOS only).

The offline image has no Preboot volume: the OS window carries only the system
volume and the SystemOS window only the SystemCryptex volume.  launchd's
mount-phase-1 builds /private/preboot/<boot-manifest-hash>/usr/standalone/
firmware from the boot-manifest-hash, and the kernel's owner cache mount puts
SystemCryptex at /private/preboot for the dyld shared cache.

The existing native_cryptex_os_view path bindfs-unions the Data volume's
/private/var/ngpu/preboot over /private/preboot.  This helper fills that tree
with the original IPSW's Firmware/all_flash files, so the union exposes a real
firmware directory to mount-phase-1 while SystemCryptex stays the lower layer
for the dyld cache.

The OwnerData volume UUID is rewritten to a fixed value after detach so the
kernel config can name it without a build-order dependency.  Every APFS volume
superblock copy is patched and checksummed.
"""

import argparse
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import uuid
from pathlib import Path

DATA_UUID = "11111111-2222-3333-4444-555555555555"


def run(command, check=True, capture=False):
    print("+ " + " ".join(str(part) for part in command))
    return subprocess.run(command, check=check, capture_output=capture, text=True)


def attach(image):
    out = subprocess.check_output(["hdiutil", "attach", "-nobrowse", "-owners", "off", "-plist", str(image)])
    return plistlib.loads(out)


def detach(dev):
    run(["hdiutil", "detach", dev], check=False)


def apfs_checksum(block):
    """APFS obj_phys checksum: s2 in the high word, -(s1+s2) in the low word."""
    block = bytearray(block)
    block[0:8] = b"\0" * 8
    s1 = s2 = 0
    for at in range(0, len(block), 4):
        word = struct.unpack_from("<I", block, at)[0]
        s1 = (s1 + word) % 0xFFFFFFFF
        s2 = (s2 + s1) % 0xFFFFFFFF
    return (s2 << 32) | ((0xFFFFFFFF - ((s1 + s2) % 0xFFFFFFFF)) % 0xFFFFFFFF)


def patch_volume_uuid(image, volume_name, new_uuid):
    data = bytearray(image.read_bytes())
    raw = uuid.UUID(new_uuid).bytes
    patched = 0
    for at in range(0, len(data) - 0x400, 4096):
        if data[at + 0x20:at + 0x24] != b"APSB":
            continue
        name = data[at + 0x2C0:at + 0x3C0].split(b"\0", 1)[0].decode("utf-8", "replace")
        if name != volume_name:
            continue
        data[at + 0xF0:at + 0x100] = raw
        struct.pack_into("<Q", data, at, apfs_checksum(data[at:at + 4096]))
        patched += 1
    if not patched:
        raise SystemExit(f"no APFS superblock named {volume_name!r} in {image}")
    image.write_bytes(data)
    print(f"patched {patched} {volume_name} superblock(s) to {new_uuid}")
    return patched


def patch_volume_role(image, volume_name, role):
    """Set apfs_role on every volume superblock copy and verify the read-back."""
    data = bytearray(image.read_bytes())
    patched = 0
    for at in range(0, len(data) - 0x400, 4096):
        if data[at + 0x20:at + 0x24] != b"APSB":
            continue
        name = data[at + 0x2C0:at + 0x3C0].split(b"\0", 1)[0].decode("utf-8", "replace")
        if name != volume_name:
            continue
        struct.pack_into("<H", data, at + 0x3C4, role)
        struct.pack_into("<Q", data, at, apfs_checksum(data[at:at + 4096]))
        patched += 1
    if not patched:
        raise SystemExit(f"no APFS superblock named {volume_name!r} in {image}")
    roles = []
    for at in range(0, len(data) - 0x400, 4096):
        if data[at + 0x20:at + 0x24] != b"APSB":
            continue
        name = data[at + 0x2C0:at + 0x3C0].split(b"\0", 1)[0].decode("utf-8", "replace")
        if name == volume_name:
            roles.append(struct.unpack_from("<H", data, at + 0x3C4)[0])
    if not roles or any(value != role for value in roles):
        raise SystemExit(f"{volume_name} role read-back failed: {roles}, expected {role}")
    image.write_bytes(data)
    print(f"patched {patched} {volume_name} superblock(s) to role {role}")
    return patched


def read_volume_group(image, volume_name):
    """Return the APFS volume-group UUID recorded in the first superblock copy."""
    data = image.read_bytes()
    for at in range(0, len(data) - 0x400, 4096):
        if data[at + 0x20:at + 0x24] != b"APSB":
            continue
        name = data[at + 0x2C0:at + 0x3C0].split(b"\0", 1)[0].decode("utf-8", "replace")
        if name == volume_name:
            return str(uuid.UUID(bytes=bytes(data[at + 0x3F0:at + 0x400])))
    raise SystemExit(f"no APFS superblock named {volume_name!r} in {image}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seed", type=Path, required=True,
                        help="the 32 MiB OwnerData APFS container produced by prepare-owner-data-fixture.py")
    parser.add_argument("--firmware-dir", type=Path, required=True,
                        help="directory holding Firmware/all_flash contents")
    parser.add_argument("--boot-manifest-hash", required=True,
                        help="boot-manifest-hash published in /chosen (the Preboot directory name)")
    parser.add_argument("--data-uuid", default=DATA_UUID)
    parser.add_argument("--output", type=Path, default=None,
                        help="report path (default: seed.with_name('preboot.json'))")
    args = parser.parse_args()

    if not args.seed.is_file():
        raise SystemExit(f"seed not found: {args.seed}")
    if not re.fullmatch(r"[0-9A-Fa-f]{96}", args.boot_manifest_hash):
        raise SystemExit("--boot-manifest-hash must be 96 hex digits")
    if not args.firmware_dir.is_dir():
        raise SystemExit(f"firmware directory not found: {args.firmware_dir}")

    attached = attach(args.seed)
    entities = attached.get("system-entities", [])
    whole = entities[0].get("dev-entry") if entities else None
    owner = next((e for e in entities if e.get("mount-point")), None)
    if not whole or not owner:
        detach(entities[0].get("dev-entry", ""))
        raise SystemExit("could not identify the APFS container and OwnerData volume")
    print(f"container {whole}, OwnerData {owner['dev-entry']} at {owner['mount-point']}")

    try:
        mount = Path(owner["mount-point"])
        # The union source the kernel's native_cryptex_os_view expects.
        tree = mount / "ngpu" / "preboot"
        target = tree / args.boot_manifest_hash / "usr" / "standalone" / "firmware"
        target.mkdir(parents=True, exist_ok=True)
        for name in ("Cryptexes/OS/System", "Cryptexes/OS/usr"):
            (tree / name).mkdir(parents=True, exist_ok=True)

        files = 0
        total = 0
        for root, _dirs, names in os.walk(args.firmware_dir):
            for name in names:
                source = Path(root) / name
                relative = source.relative_to(args.firmware_dir)
                destination = target / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
                files += 1
                total += source.stat().st_size
        print(f"copied {files} firmware file(s), {total} bytes to {target}")
        if files == 0:
            raise SystemExit("the firmware directory is empty")
    finally:
        detach(whole)

    patch_volume_uuid(args.seed, "OwnerData", args.data_uuid)
    patch_volume_role(args.seed, "OwnerData", 64)
    group_id = read_volume_group(args.seed, "OwnerData")

    report = {
        "seed": str(args.seed),
        "seed_bytes": args.seed.stat().st_size,
        "data_volume_uuid": args.data_uuid,
        "data_volume_role": 64,
        "data_volume_group_id": group_id,
        "boot_manifest_hash": args.boot_manifest_hash,
        "firmware_files": files,
        "firmware_bytes": total,
        "preboot_source": "/private/var/ngpu/preboot",
        "preboot_path": f"/{args.boot_manifest_hash}/usr/standalone/firmware",
    }
    output = args.output or args.seed.with_name("preboot.json")
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    sys.exit(main())
