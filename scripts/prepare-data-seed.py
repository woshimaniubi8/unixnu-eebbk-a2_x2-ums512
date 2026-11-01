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
4. ``hdiutil detach`` unmounts the OS volume again.

Pass the printed seed path to ``./unixnu build payload --data-ramdisk FILE``.
The board file needs ``[payload] size_mib`` big enough for the kernel, the ADT
and the seed (the helper warns when it is not).

Usage: prepare-data-seed.py [--tree DIR] [--output FILE] [--force] [--keep-mounted]
"""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

SEED_BYTES = 32 * 1024 * 1024
LAUNCHD_SHA256 = "8cec9a3c44a443b08e977ccc5d577850316ad428c575dc01f09b018dbd5b6a25"
PAYLOAD_OVERHEAD = 8 * 1024 * 1024


def run(command, cwd, **kwargs):
    print("+ " + " ".join(str(part) for part in command))
    return subprocess.run(command, cwd=cwd, check=True, **kwargs)


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
    args = parser.parse_args()

    tree = args.tree.resolve()
    output = (tree / args.output).resolve() if not args.output.is_absolute() else args.output
    mountpoint = tree / ".cache/firmware/target-volumes/os-mount"

    if output.is_file() and not args.force:
        print(f"seed already present: {output} ({output.stat().st_size} bytes, "
              f"sha256 {sha256_file(output)})")
        return 0
    if args.force:
        fixture = output.parent
        for name in ("owner-data-32.cdr", "seed.json"):
            (fixture / name).unlink(missing_ok=True)

    dmg = extract_os_dmg(tree)
    print(f"os image: {dmg} ({dmg.stat().st_size >> 20} MiB)")
    mountpoint.mkdir(parents=True, exist_ok=True)
    attached = False
    try:
        run(["hdiutil", "attach", "-nobrowse", "-readonly",
             "-mountpoint", str(mountpoint), str(dmg)], tree)
        attached = True
        launchd = mountpoint / "sbin/launchd"
        if not launchd.is_file():
            raise SystemExit(f"the OS volume has no sbin/launchd: {mountpoint}")
        digest = sha256_file(launchd)
        if digest != LAUNCHD_SHA256:
            raise SystemExit(f"unexpected sbin/launchd (sha256 {digest}); the seed "
                             "fixture requires the unchanged original OS volume")
        run([sys.executable, "scripts/prepare-owner-data-fixture.py"], tree)
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
