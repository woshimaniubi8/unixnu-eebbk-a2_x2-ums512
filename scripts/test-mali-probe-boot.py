#!/usr/bin/env python3
"""Check the actual executable/cache round trip used by the boot probe."""
import argparse
import hashlib
import importlib.util
import plistlib
import re
import subprocess
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("probe_boot", ROOT / "scripts/prepare-mali-probe-boot.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    probe = args.probe.read_bytes()
    original = (ROOT / "patches/xnu/bbk-a2-splashboard-prewarm.patch").read_text()
    patch, report = module.generate(original, probe)
    before = module.extract_function(original)
    with tempfile.TemporaryDirectory(prefix="bbk-mali-stage-") as directory:
        tree = Path(directory)
        source = tree / "bsd/kern/kern_exec.c"
        source.parent.mkdir(parents=True)
        prefix = "/* Other kernel code before the interior excerpt. */\n" * 32
        suffix = "\n/* Other kernel code after the interior excerpt. */\n"
        source.write_text(prefix + before + suffix, encoding="utf-8", newline="\n")
        patchfile = tree / "probe.patch"
        patchfile.write_text(patch, encoding="utf-8", newline="\n")
        subprocess.run(["git", "apply", "--check", str(patchfile)], cwd=tree, check=True)
        subprocess.run(["git", "apply", str(patchfile)], cwd=tree, check=True)
        after = source.read_text()
        assert zlib.decompress(module.array_bytes(after, "probe_packed")) == probe
        assert module.array_bytes(after, "probe_sha") == hashlib.sha256(probe).digest()
        cache = zlib.decompress(module.array_bytes(after, "packed"))
        assert module.array_bytes(after, "modified_sha") == hashlib.sha256(cache).digest()
        assert len(cache) == report["cache_bytes"]
        assert int(re.search(r"configuration_size = (\d+);", after)[1]) == len(cache)
        decoded = plistlib.loads(cache)
        job = decoded["LaunchDaemons"].pop(module.JOB_PATH)
        disk_plist = module.array_bytes(after, "probe_job_plist")
        assert plistlib.loads(disk_plist) == job == module.probe_job()
        assert hashlib.sha256(disk_plist).hexdigest() == report["daemon_plist_sha256"]
        assert len(disk_plist) == report["daemon_plist_bytes"]
        assert report["daemon_plist_source"] == module.JOB_SOURCE
        assert set(report["stock_daemon_checks"]) == set(module.STOCK_JOBS)
        assert job["ProgramArguments"] == [module.PROBE_PATH]
        assert job["RunAtLoad"] is True and job["KeepAlive"] is False
        assert decoded == plistlib.loads(zlib.decompress(module.array_bytes(before, "packed")))
        subprocess.run(["git", "apply", "--reverse", str(patchfile)], cwd=tree, check=True)
        assert source.read_text() == prefix + before + suffix
    for malformed in (b"", probe[:16], b"\x00" * 32, probe * 9):
        try:
            module.generate(original, malformed)
        except ValueError:
            pass
        else:
            raise AssertionError("unbounded/non-ARM64 executable accepted")
    print("PASS: signed image round trip, identical disk and cache jobs, unchanged stock policies, patch rollback and invalid image refusal")


if __name__ == "__main__":
    main()
