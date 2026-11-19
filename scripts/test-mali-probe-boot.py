#!/usr/bin/env python3
"""Verify the exact probe and the additive XNU patch, including its reversal."""
import argparse
import hashlib
import importlib.util
import re
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "probe_boot", Path(__file__).with_name("prepare-mali-probe-boot.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def array_bytes(source, name):
    match = re.search(rf"static const uint8_t {name}\[\] = \{{(.*?)\n    \}};", source, re.S)
    assert match, name
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", match[1]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("--kern-exec", type=Path)
    args = parser.parse_args()
    probe = args.probe.read_bytes()
    patch, report = module.generate(probe)
    assert not any(line.startswith("-") and not line.startswith("---") for line in patch.splitlines())
    if args.kern_exec:
        before = args.kern_exec.read_text(encoding="utf-8")
    else:
        filler = "/* Other code in kern_exec.c. */\n" * 64
        before = filler + module.HELPER_CONTEXT + filler + module.CALL_CONTEXT + filler
    with tempfile.TemporaryDirectory(prefix="bbk-mali-stage-") as directory:
        tree = Path(directory)
        source = tree / "bsd/kern/kern_exec.c"
        source.parent.mkdir(parents=True)
        source.write_text(before, encoding="utf-8", newline="\n")
        patch_file = tree / "probe.patch"
        patch_file.write_text(patch, encoding="utf-8", newline="\n")
        apply = ["git", "-c", "core.autocrlf=false", "apply"]
        for flags in (["--check"], []):
            subprocess.run([*apply, *flags, str(patch_file)], cwd=tree, check=True)
        after = source.read_text(encoding="utf-8")
        assert zlib.decompress(array_bytes(after, "probe_packed")) == probe
        assert array_bytes(after, "probe_sha") == hashlib.sha256(probe).digest()
        assert report["probe_bytes"] == len(probe) and report["probe_path"] in after
        assert report["probe_sha256"] == hashlib.sha256(probe).hexdigest()
        assert report["pagezero_bytes"] == module.HARD_PAGEZERO
        assert report["changes_stock_launchd"] is False
        assert 'PE_parse_boot_argn("bbk-mali-userspace", &mali_probe, sizeof(mali_probe)) && mali_probe == 1' in after
        assert after.index("int data_error = native_owner_data_prepare") < after.index(
            "const int probe_error = native_mali_probe_spawn")
        assert "error = posix_spawn(p, &request, result);" in after
        assert "PSFA_OPEN" in after and "PSFA_DUP2" in after
        assert "M157 mali-user: continue normal init probe_err=" in after
        assert "load_init_program_at_path" in after
        assert "daemon plist visible=" not in patch and "/System/Library/LaunchDaemons" not in patch
        subprocess.run([*apply, "--reverse", str(patch_file)], cwd=tree, check=True)
        assert source.read_bytes() == before.encode("utf-8")
    zero_segment = probe.find(b"__PAGEZERO\0")
    assert zero_segment >= 40
    bad_zero = bytearray(probe)
    struct.pack_into("<Q", bad_zero, zero_segment + 16 + 8, 0x4000)
    try:
        module.validate_layout(bad_zero)
    except ValueError as error:
        assert "4 GiB hard __PAGEZERO" in str(error)
    else:
        raise AssertionError("M157's invalid 16 KiB __PAGEZERO was accepted")
    for malformed in (b"", probe[:16], bytes(32), probe[:12] + struct.pack("<I", 1) + probe[16:],
                      bytes(512 * 1024 + 1)):
        try:
            module.generate(malformed)
        except ValueError:
            pass
        else:
            raise AssertionError("unbounded or wrong Mach-O accepted")
    print("PASS: exact probe, opt-in child spawn, unchanged stock boot source, reverse patch, invalid images")


if __name__ == "__main__":
    main()
