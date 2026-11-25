#!/usr/bin/env python3
"""Map boot UI trace PCs to dyld shared cache images (macOS runner only).

The kernel UI trace prints raw user-space PCs. The IPSW that the payload build
already downloaded is scanned for the dyld shared cache (the OS dmg does not
carry it; it lives in the System cryptex image), then each PC is resolved to
the cache image that contains it plus the offset inside that image.
"""
import json
import mmap
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

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


def ipsw_url():
    profile = ROOT / "UniXNU/profiles/os/ipados-17.7.11-21H461/profile.toml"
    if not profile.is_file():
        return None
    for line in profile.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("ipsw_url"):
            return stripped.split("=", 1)[1].strip().strip('"').strip("'")
    return None


def ensure_ipsw(tempdir):
    existing = sorted(ROOT.glob("UniXNU/out/ipsw/*/*.ipsw")) + sorted(ROOT.glob("UniXNU/.cache/**/*.ipsw"))
    if existing:
        return existing[0]
    url = ipsw_url()
    if not url:
        return None
    target = Path(tempdir) / "restore.ipsw"
    print(f"downloading {url}", flush=True)
    subprocess.run(["curl", "-sSL", "--retry", "3", "-o", str(target), url], check=True)
    print(f"ipsw: {target} ({target.stat().st_size} bytes)", flush=True)
    return target


def find_candidates():
    found = []
    found += sorted(ROOT.glob("UniXNU/out/ipsw/*/*.ipsw"))
    found += sorted(ROOT.glob("UniXNU/.cache/**/*.ipsw"))
    found += sorted(ROOT.glob("UniXNU/out/ipsw/*/*.dmg"))
    return found


def scan_image(image, label, tempdir):
    mount = Path(tempfile.mkdtemp(prefix="dmg-mount-", dir=tempdir))
    subprocess.run(["hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", str(mount), str(image)],
                   check=True, stdout=subprocess.DEVNULL)
    try:
        found = []
        for root, dirs, files in os.walk(mount):
            depth = len(Path(root).relative_to(mount).parts)
            if depth > 10:
                dirs[:] = []
                continue
            for name in files:
                if name.startswith("dyld_shared_cache"):
                    found.append(Path(root) / name)
        print(f"{label}: {[str(p.relative_to(mount)) for p in found[:8]]}", flush=True)
        for path in found:
            if ".symbols" in path.name or ".map" in path.name or ".dyldlinkedit" in path.name:
                continue
            return path
        return None
    finally:
        subprocess.run(["hdiutil", "detach", str(mount)], check=False)


def symbolize(cache):
    print(f"cache: {cache} ({cache.stat().st_size} bytes)", flush=True)
    report = {}
    with cache.open("rb") as handle:
        buf = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
        images_offset, images_count = struct.unpack_from("<II", buf, 0x18)
        if images_count == 0:
            images_offset, images_count = struct.unpack_from("<II", buf, 0xE0)
        print(f"images: offset=0x{images_offset:x} count={images_count}", flush=True)
        images = []
        for index in range(images_count):
            address, _mod, _ino, path_offset, _pad = struct.unpack_from("<QQQII", buf, images_offset + index * 32)
            images.append((address, cstring(buf, path_offset)))
        images.sort()
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
    print(json.dumps(report, indent=2), flush=True)


def main():
    candidates = find_candidates()
    print(f"root={ROOT} candidates={[str(p) for p in candidates]}", flush=True)
    if not candidates:
        sys.exit(2)
    with tempfile.TemporaryDirectory(prefix="ipsw-scan-") as tempdir:
        for image in candidates:
            if image.suffix.lower() == ".dmg":
                cache = scan_image(image, image.name, tempdir)
                if cache:
                    symbolize(cache)
                    return
        if ensure_ipsw(tempdir) is not None:
            candidates = candidates + [ensure_ipsw(tempdir)]
        for ipsw in candidates:
            if ipsw.suffix.lower() != ".ipsw":
                continue
            listing = subprocess.run(["unzip", "-l", str(ipsw)], capture_output=True, text=True, check=True).stdout
            names = [line.split()[-1] for line in listing.splitlines() if line.strip().endswith(".dmg")]
            print(f"{ipsw.name}: dmgs={names}", flush=True)
            for name in names:
                subprocess.run(["unzip", "-o", "-j", str(ipsw), name, "-d", str(tempdir)],
                               check=True, stdout=subprocess.DEVNULL)
                target = Path(tempdir) / Path(name).name
                if not target.is_file():
                    continue
                cache = scan_image(target, target.name, tempdir)
                if cache:
                    symbolize(cache)
                    return
                target.unlink(missing_ok=True)
    print("no dyld shared cache found in any image", flush=True)
    sys.exit(1)


if __name__ == "__main__":
    main()
