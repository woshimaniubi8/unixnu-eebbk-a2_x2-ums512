#!/usr/bin/env python3
"""Map boot UI trace PCs to dyld shared cache images (macOS runner only).

The kernel's UI trace prints raw user-space PCs. The stock OS dmg that the
payload build already downloaded is mounted, and each PC is resolved to the
cache image that contains it plus the offset inside that image.
"""
import json
import mmap
import os
import plistlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def find_dmg():
    fixed = [ROOT / "UniXNU/out/ipsw/21H461/044-76040-038.dmg"]
    fixed += sorted(ROOT.glob("UniXNU/out/ipsw/*/044-*.dmg"))
    fixed += sorted(ROOT.glob("UniXNU/.cache/**/044-*.dmg"))
    for path in fixed:
        if path.is_file():
            return path
    return None


DMG = find_dmg()

TRACES = {
    "m264-round14-tid1097": [
        0x1d491cb58, 0x1d49201b0, 0x1d491ffb8, 0x1987f8cac, 0x1987f8b98,
        0x1987f88ac, 0x19882ebb8, 0x19882def4, 0x1b0265e58, 0x1b0266200,
        0x198752cc0, 0x1987240e4, 0x1b02661d8, 0x1b0294460, 0x1d8bd4d60,
        0x1b25fa228,
    ],
    "m253-round20-tid307": [
        0x1cd570970, 0x189a7d228, 0x189a7ceb8, 0x189a7cac8, 0x189a9fd10,
        0x202b79ce0, 0x1914f80e8, 0x202b796b8, 0x202b795b0, 0x202b81c68,
        0x202b81a60, 0x202b81460, 0x1913a6cc0, 0x19138944c, 0x202b8141c,
        0x202b81274, 0x202b7b0a8, 0x202b7b054, 0x1913a6cc0, 0x1913780e4,
        0x202b7b038, 0x1d1846214, 0x1a8ee8548, 0x1d1828d60, 0x1ab24e228,
    ],
}


def cstring(buf, offset):
    end = buf.find(b"\0", offset)
    return buf[offset:end if end >= 0 else offset + 256].decode("utf-8", "replace")


def main():
    print(f"cwd={Path.cwd()} root={ROOT} dmg={DMG}", flush=True)
    if DMG is None or not DMG.is_file():
        listing = sorted(str(p) for p in ROOT.glob("UniXNU/out/ipsw/*/*")[:8])
        print(f"missing dmg; ipsw dir: {listing}", flush=True)
        sys.exit(2)
    mount = Path(tempfile.mkdtemp(prefix="dyld-cache-"))
    subprocess.run(["hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", str(mount), str(DMG)], check=True)
    try:
        print("mount root:", sorted(p.name for p in mount.iterdir())[:24], flush=True)
        for probe in ("System/Library", "System/Library/Caches", "System/Library/dyld",
                      "System/Library/Cryptexes", "System/Cryptexes", "private/var"):
            path = mount / probe
            if path.is_dir():
                try:
                    print(f"{probe}:", sorted(p.name for p in path.iterdir())[:24], flush=True)
                except OSError as error:
                    print(f"{probe}: unreadable {error}", flush=True)
        daemons = mount / "System/Library/LaunchDaemons"
        if daemons.is_dir():
            names = sorted(entry.name for entry in daemons.glob("*.plist"))
            print(f"os-launchdaemons ({len(names)}):", flush=True)
            print("\n".join(names), flush=True)
        cache = None
        found = []
        for root, dirs, files in os.walk(mount):
            depth = len(Path(root).relative_to(mount).parts)
            if depth > 10:
                dirs[:] = []
                continue
            for name in files:
                if name.startswith("dyld_shared_cache"):
                    found.append(Path(root) / name)
            if len(found) > 40:
                break
        print("dyld cache candidates:", [str(p) for p in found[:12]], flush=True)
        for path in found:
            if ".symbols" in path.name or ".map" in path.name or ".dyldlinkedit" in path.name:
                continue
            cache = path
            break
        if cache is None:
            sys.exit("no dyld shared cache found under the mounted OS image")
        print(f"cache: {cache} ({cache.stat().st_size} bytes)")
        with cache.open("rb") as handle:
            buf = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
            images_offset, images_count = struct.unpack_from("<II", buf, 0x18)
            if images_count == 0:
                images_offset, images_count = struct.unpack_from("<II", buf, 0xE0)
            print(f"images: offset=0x{images_offset:x} count={images_count}")
            images = []
            for index in range(images_count):
                address, _mod, _ino, path_offset, _pad = struct.unpack_from("<QQQII", buf, images_offset + index * 32)
                images.append((address, cstring(buf, path_offset)))
            images.sort()
            report = {}
            for label, pcs in TRACES.items():
                rows = []
                for pc in pcs:
                    if pc == 0:
                        continue
                    low, high = 0, len(images) - 1
                    best = None
                    while low <= high:
                        mid = (low + high) // 2
                        if images[mid][0] <= pc:
                            best = images[mid]
                            low = mid + 1
                        else:
                            high = mid - 1
                    if best is None:
                        rows.append({"pc": f"0x{pc:x}", "image": None, "offset": None})
                    else:
                        rows.append({"pc": f"0x{pc:x}", "image": best[1], "offset": f"0x{pc - best[0]:x}"})
                report[label] = rows
            print(json.dumps(report, indent=2))
    finally:
        subprocess.run(["hdiutil", "detach", str(mount)], check=False)


if __name__ == "__main__":
    main()
