#!/usr/bin/env python3
"""Prepare the writable APFS Data seed for the BBK A2 payload (macOS only).

The Data volume of the offline OS image does not exist: the OS window carries
only the read-only system volume, so the kernel needs a writable APFS Data
volume supplied as the payload's ramdisk (``Payloads/scratch-data.img`` plus the
``RAMDisk``/``ephemeral-storage`` entries in the ADT).

UniXNU's ``scripts/prepare-owner-data-fixture.py`` builds that 32 MiB seed from
the initial ``/private/var`` tree of the original OS volume, and that script
needs the OS volume mounted at ``.cache/firmware/target-volumes/os-mount``.  This
helper does the macOS side of that contract:

1. ``./unixnu ipsw fetch OS`` extracts ``044-76040-038.dmg`` from the pinned
   IPSW into ``out/ipsw/<build>/`` (checked against ``[ipsw.sha256]``; the
   member is about 4.8 GiB, so the extraction dominates the CI step time).
2. ``hdiutil attach`` mounts it read-only at
   ``.cache/firmware/target-volumes/os-mount``.
3. ``scripts/prepare-owner-data-fixture.py`` writes
   ``build/owner-data-fixture/owner-data-32.cdr`` (raw APFS, dense delivery) and
   ``seed.json``.
4. ``scripts/prepare-preboot-tree.py`` copies the IPSW's Firmware/all_flash
   files into the seed's ``/private/var/ngpu/preboot/<boot-manifest-hash>``
   tree (the source the kernel's cryptex union shows over /private/preboot)
   and pins the Data volume UUID to the value in the defconfig.
5. ``hdiutil detach`` unmounts the OS volume again.

Pass the printed seed path to ``./unixnu build payload --data-ramdisk FILE``.
The board file needs ``[payload] size_mib`` big enough for the kernel, the ADT
and the seed (the helper warns when it is not).

Usage: prepare-data-seed.py [--tree DIR] [--output FILE] [--force] [--keep-mounted]
"""

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

SEED_BYTES = 32 * 1024 * 1024
LAUNCHD_SHA256 = "8cec9a3c44a443b08e977ccc5d577850316ad428c575dc01f09b018dbd5b6a25"
PAYLOAD_OVERHEAD = 8 * 1024 * 1024


def run(command, cwd, check=True, **kwargs):
    print("+ " + " ".join(str(part) for part in command))
    return subprocess.run(command, cwd=cwd, check=check, **kwargs)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def extract_os_dmg(tree):
    """Extract the pinned OS dmg from the IPSW (streams progress) and return its path."""
    subprocess.run(["./unixnu", "ipsw", "fetch", "OS"], cwd=tree, check=True)
    candidates = sorted((tree / "out/ipsw").glob("*/044-76040-038.dmg"))
    if not candidates:
        raise SystemExit("unixnu ipsw fetch OS did not produce 044-76040-038.dmg")
    path = candidates[-1]
    if not path.is_file() or path.stat().st_size == 0:
        raise SystemExit(f"the OS image was not extracted: {path}")
    return path


def open_ipsw(tree, device):
    """Open the pinned IPSW through the range reader and return (Ipsw, board)."""
    sys.path.insert(0, str(tree / "scripts"))
    from unixnu.ipsw import Ipsw

    profile = tomllib.loads((tree / "profiles/os/ipados-17.7.11-21H461/profile.toml").read_text())
    board = tomllib.loads((device / "board.toml").read_text())
    board["ipsw"] = profile["ipsw"]

    class Context:
        def __init__(self):
            self.board = board
            self.out_root = tree / "out"

        def option_text(self, *names):
            return None

    return Ipsw(Context()), board


def extract_all_flash(tree, device):
    """Extract Firmware/all_flash from the pinned IPSW through the range reader."""
    ipsw, _board = open_ipsw(tree, device)
    destination = Path(tempfile.mkdtemp(prefix="bbk-preboot-firmware-"))
    count = 0
    for name in ipsw.zip.namelist():
        if not name.startswith("Firmware/all_flash/") or name.endswith("/"):
            continue
        relative = Path(name).relative_to("Firmware/all_flash")
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(ipsw.zip.read(name))
        count += 1
    if not count:
        raise SystemExit("the IPSW has no Firmware/all_flash entries")
    print(f"all_flash: {count} file(s) in {destination}")
    return destination


def _der(data, at):
    """Read one DER TLV; returns (tag, value, next_offset)."""
    if at + 2 > len(data):
        raise ValueError("truncated DER")
    tag, size = data[at], data[at + 1]
    at += 2
    if size & 0x80:
        count = size & 0x7F
        if not 0 < count <= 4 or at + count > len(data):
            raise ValueError("bad DER length")
        size = int.from_bytes(data[at:at + count], "big")
        at += count
    if at + size > len(data):
        raise ValueError("DER exceeds input")
    return tag, data[at:at + size], at + size


def im4p_payload(original, what):
    """The payload field of an IM4P (the trust module itself)."""
    tag, sequence, end = _der(original, 0)
    fields, cursor = [], 0
    while cursor < len(sequence):
        kind, value, cursor = _der(sequence, cursor)
        fields.append((kind, value))
    if tag != 0x30 or end != len(original) or [kind for kind, _ in fields] != [0x16, 0x16, 0x16, 0x04] \
            or fields[0][1] != b"IM4P":
        raise SystemExit("not an IM4P trust cache: " + what)
    return fields[3][1]


def build_trust_bundle(tree, device, output):
    """Build the static + System trust bundle the payload's AMFI path needs."""
    ipsw, board = open_ipsw(tree, device)
    static = im4p_payload(ipsw.read("StaticTrustCache"), "StaticTrustCache")
    system = im4p_payload(ipsw.read("Cryptex1,SystemTrustCache"), "Cryptex1,SystemTrustCache")
    for name, module in (("static", static), ("system", system)):
        version = struct.unpack_from("<I", module, 0)[0]
        count = struct.unpack_from("<I", module, 20)[0]
        if version != 2 or len(module) != 24 + count * 24:
            raise SystemExit(f"not a complete version-2 trust module: {name}")
    bundle = struct.pack("<3I", 2, 12, 12 + len(static)) + static + system
    digest = hashlib.sha256(bundle).hexdigest()
    allowed = board.get("payload", {}).get("trust_cache_sha256", [])
    if allowed and digest not in allowed:
        raise SystemExit(f"trust bundle sha256 {digest} is not one of {allowed}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(bundle)
    print(f"trust bundle: {output} ({len(bundle)} bytes, sha256 {digest})")
    return output


def probe_mount_fstab_schema(mount_root):
    """Print the device-tree fstab schema mount expects (diagnostic only)."""
    binary = mount_root / "sbin/mount"
    print("== mount fstab schema probe ==")
    print("binary:", binary, "exists:", binary.exists())
    if not binary.exists():
        return
    strings = shutil.which("strings") or "/usr/bin/strings"
    try:
        out = subprocess.run([strings, "-a", str(binary)], capture_output=True, text=True, timeout=180).stdout
    except Exception as error:
        print("strings failed:", error)
        return
    lines = out.splitlines()
    for pattern in ("filesystems/fstab", "DT_get_fstab", "IODeviceTree", "fstab"):
        hits = [line for line in lines if pattern in line]
        print("-- %s: %d hit(s)" % (pattern, len(hits)))
        for line in hits[:10]:
            print("   ", line[:200])
    print("-- ramdisk strings (case-insensitive) --")
    for line in lines:
        if "ramdisk" in line.lower():
            print("   ", line[:200])
    print("-- mount strings dump (MNT| prefix, capped) --")
    for line in lines[:30000]:
        print("MNT|" + line[:220])
    print("-- fstab neighbours --")
    for index, line in enumerate(lines):
        if "fstab" in line.lower():
            for neighbour in lines[max(0, index - 3):index + 4]:
                print("   ", neighbour[:200])
            print("   ---")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tree", type=Path, default=Path.cwd(),
                        help="UniXNU checkout (default: the current directory)")
    parser.add_argument("--output", type=Path,
                        default=Path("build/owner-data-fixture/owner-data-32.cdr"),
                        help="seed path relative to --tree")
    parser.add_argument("--force", action="store_true", help="replace an existing seed")
    parser.add_argument("--keep-mounted", action="store_true",
                        help="leave the OS volume mounted for inspection")
    parser.add_argument("--preboot-hash", default=None,
                        help="boot-manifest-hash for the Preboot tree (default: board.toml [payload.adt])")
    args = parser.parse_args()

    tree = args.tree.resolve()
    output = (tree / args.output).resolve() if not args.output.is_absolute() else args.output
    mountpoint = tree / ".cache/firmware/target-volumes/os-mount"
    device = Path(__file__).resolve().parents[1]
    board_config = tomllib.loads((device / "board.toml").read_text())
    preboot_hash = args.preboot_hash or board_config.get("payload", {}).get("adt", {}).get(
        "preboot_manifest_hash")
    if not preboot_hash:
        raise SystemExit("no Preboot boot-manifest-hash: pass --preboot-hash or set "
                         "[payload.adt] preboot_manifest_hash in board.toml")

    if output.is_file() and (output.parent / "preboot.json").is_file() and not args.force:
        print(f"seed already present: {output} ({output.stat().st_size} bytes, "
              f"sha256 {sha256_file(output)})")
        return 0
    if args.force:
        fixture = output.parent
        for name in ("owner-data-32.cdr", "seed.json", "preboot.json"):
            (fixture / name).unlink(missing_ok=True)

    dmg = extract_os_dmg(tree)
    print(f"os image: {dmg} ({dmg.stat().st_size >> 20} MiB)")
    mountpoint.mkdir(parents=True, exist_ok=True)
    attached = False
    try:
        run(["hdiutil", "attach", "-nobrowse", "-readonly",
             "-mountpoint", str(mountpoint), str(dmg)], tree)
        attached = True
        probe_mount_fstab_schema(mountpoint)
        launchd = mountpoint / "sbin/launchd"
        if not launchd.is_file():
            raise SystemExit(f"the OS volume has no sbin/launchd: {mountpoint}")
        digest = sha256_file(launchd)
        if digest != LAUNCHD_SHA256:
            raise SystemExit(f"unexpected sbin/launchd (sha256 {digest}); the seed "
                             "fixture requires the unchanged original OS volume")
        run([sys.executable, "scripts/prepare-owner-data-fixture.py"], tree)
        firmware = extract_all_flash(tree, device)
        try:
            run([sys.executable, str(Path(__file__).with_name("prepare-preboot-tree.py")),
                 "--seed", str(output), "--firmware-dir", str(firmware),
                 "--boot-manifest-hash", preboot_hash], tree)
        finally:
            shutil.rmtree(firmware, ignore_errors=True)
        build_trust_bundle(tree, device, output.parent / "trustcache.bundle")
    finally:
        if attached and not args.keep_mounted:
            run(["hdiutil", "detach", str(mountpoint)], tree, check=False)

    if not output.is_file() or output.stat().st_size != SEED_BYTES:
        raise SystemExit(f"the seed is missing or not {SEED_BYTES} bytes: {output}")
    seed = json.loads((output.parent / "seed.json").read_text())
    print(json.dumps(seed, indent=2))
    print(f"seed: {output} ({output.stat().st_size} bytes, sha256 {sha256_file(output)})")

    board = tree / "device/board.toml"
    if board.is_file():
        import re
        text = board.read_text()
        match = re.search(r"^\[payload\][^\[]*?^size_mib\s*=\s*(\d+)", text, re.M | re.S)
        if match:
            needed = (SEED_BYTES + 52 * 1024 * 1024 + PAYLOAD_OVERHEAD) >> 20
            if int(match.group(1)) < needed:
                print(f"warning: board.toml [payload] size_mib = {match.group(1)} but the "
                      f"kernel plus seed need about {needed} MiB; raise it before "
                      f"building the payload", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
