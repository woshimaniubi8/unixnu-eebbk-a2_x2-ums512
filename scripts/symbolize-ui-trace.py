#!/usr/bin/env python3
"""Map boot UI trace PCs to dyld shared cache images (macOS runner only).

The kernel's UI trace prints raw user-space PCs. The stock OS dmg that the
payload build already downloaded is mounted, and each PC is resolved to the
cache image that contains it plus the offset inside that image.
"""
import json
import mmap
import plistlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DMG = ROOT / "UniXNU/out/ipsw/21H461/044-76040-038.dmg"

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
    if not DMG.is_file():
        sys.exit(f"missing dmg: {DMG}")
    mount = Path(tempfile.mkdtemp(prefix="dyld-cache-"))
    subprocess.run(["hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", str(mount), str(DMG)], check=True)
    try:
        cache_dir = mount / "System/Library/Caches/com.apple.dyld"
        candidates = sorted(cache_dir.glob("dyld_shared_cache_arm64*"))
        cache = next((p for p in candidates if p.suffix not in (".symbols", ".map") and ".dyldlinkedit" not in p.name), None)
        if cache is None:
            sys.exit(f"no dyld shared cache under {cache_dir}: {[p.name for p in candidates][:8]}")
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
