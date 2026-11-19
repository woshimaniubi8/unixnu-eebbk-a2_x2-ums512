#!/usr/bin/env python3
"""Stage the signed board GPU probe through the existing owner boot writer.

The unchanged Apple launchd reads the existing merged XPC cache.  Add one
RunAtLoad job to that cache and embed the exact signed executable in the kernel
patch.  No APFS metadata or vendor daemon is edited to install the probe.
"""
import argparse
import copy
import difflib
import hashlib
import json
import plistlib
import re
import struct
import subprocess
import zlib
from pathlib import Path

PREWARM_SHA256 = "94433b3eee2e4fefe8f938a79f472cca384d8c0563d59d08c72f15cc3d0aa953"
PROBE_PATH = "/private/var/ngpu/mali-user/mali-user-probe"
JOB_PATH = "/System/Library/LaunchDaemons/org.unixnu.bbk.mali-probe.plist"


def c_array(name, data):
    lines = [f"    static const uint8_t {name}[] = {{"]
    lines += ["\t" + ", ".join(f"0x{byte:02x}" for byte in data[at:at + 16]) + ","
              for at in range(0, len(data), 16)]
    return "\n".join(lines + ["    };"])


def extract_function(patch):
    additions = "\n".join(line[1:] for line in patch.splitlines()
                          if line.startswith("+") and not line.startswith("+++")) + "\n"
    start = additions.index("/* The original loader consumes")
    end = additions.index("\n}\n", start) + 3
    return additions[start:end]


def array_bytes(function, name):
    match = re.search(rf"static const uint8_t {name}\[(?:32)?\] = \{{(.*?)\n    \}};",
                      function, re.S)
    if not match:
        raise ValueError(f"missing {name} array")
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", match[1]))


def replace_array(function, name, value):
    pattern = rf"    static const uint8_t {name}\[(?:32)?\] = \{{.*?\n    \}};"
    result, count = re.subn(pattern, lambda _: c_array(name, value), function,
                           flags=re.S)
    if count != 1:
        raise ValueError(f"expected exactly one {name} array")
    return result


def build_cache(original):
    if hashlib.sha256(original).hexdigest() != PREWARM_SHA256:
        raise ValueError("the qualified launchd prewarm cache changed")
    baseline = plistlib.loads(original)
    merged = copy.deepcopy(baseline)
    jobs = merged["LaunchDaemons"]
    if JOB_PATH in jobs:
        raise ValueError("GPU probe job already present")
    jobs[JOB_PATH] = {
        "Label": "org.unixnu.bbk.mali-probe",
        "ProgramArguments": [PROBE_PATH],
        "UserName": "root",
        "RunAtLoad": True,
        "KeepAlive": False,
        "ProcessType": "Background",
        "StandardOutPath": "/private/var/ngpu/mali-user/stdout.log",
        "StandardErrorPath": "/private/var/ngpu/mali-user/stderr.log",
    }
    encoded = plistlib.dumps(merged, fmt=plistlib.FMT_BINARY, sort_keys=False)
    # Compare the full decoded record, including all stock launch policies.
    restored = plistlib.loads(encoded)
    added = restored["LaunchDaemons"].pop(JOB_PATH)
    if restored != baseline or added != jobs[JOB_PATH]:
        raise ValueError("an existing launchd cache entry changed")
    return encoded


def stage_function(probe):
    packed = zlib.compress(probe, 9)
    return ("static int\nnative_mali_user_probe_prepare(proc_t p, user_addr_t scratch)\n{\n"
            f'    static const char path[] = "{PROBE_PATH}";\n'
            + c_array("probe_packed", packed) + "\n"
            + c_array("probe_sha", hashlib.sha256(probe).digest()) + "\n"
            f"    const size_t bytes = {len(probe)};\n"
            "    vfs_context_t ctx = vfs_context_current();\n"
            "    uint8_t *buffer = kalloc_data(bytes, Z_WAITOK);\n"
            "    if (!buffer) return ENOMEM;\n"
            "    int error = native_gpu_addon_inflate(probe_packed, sizeof(probe_packed), buffer, bytes);\n"
            "    uint8_t digest[32];\n"
            "    SHA256_CTX hash;\n"
            "    if (!error) {\n"
            "        SHA256_Init(&hash); SHA256_Update(&hash, buffer, bytes); SHA256_Final(digest, &hash);\n"
            "        if (memcmp(digest, probe_sha, sizeof(digest))) error = EINVAL;\n"
            "    }\n"
            "    if (!error) error = native_gpu_addon_parents(p, scratch, path);\n"
            "    if (!error) {\n"
            "        error = native_gpu_addon_write(path, buffer, bytes, 0755, ctx);\n"
            "        if (error == -1) error = 0;\n"
            "    }\n"
            "    vnode_t vp = NULLVP;\n"
            "    int verified = 0;\n"
            "    if (!error) error = vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);\n"
            "    if (!error) {\n"
            "        verified = native_gpu_addon_equal(vp, buffer, bytes, 0755, ctx);\n"
            "        const int closed = vnode_close(vp, FREAD, ctx);\n"
            "        if (closed) error = closed;\n"
            "        else if (!verified) error = EIO;\n"
            "    }\n"
            "    kfree_data(buffer, bytes);\n"
            '    printf("M155 mali-user: staged bytes=%u verified=%d err=%d\\n", (unsigned)bytes, verified, error);\n'
            "    return error;\n}\n\n")


def generate(prewarm_patch, probe):
    if not 32 <= len(probe) <= 512 * 1024:
        raise ValueError("unexpected signed probe size")
    if struct.unpack_from("<II", probe) != (0xFEEDFACF, 0x0100000C):
        raise ValueError("probe must be a thin ARM64 Mach-O")
    before = extract_function(prewarm_patch)
    original = zlib.decompress(array_bytes(before, "packed"))
    cache = build_cache(original)
    after = replace_array(before, "packed", zlib.compress(cache, 9))
    after = replace_array(after, "modified_sha", hashlib.sha256(cache).digest())
    after, count = re.subn(r"configuration_size = \d+;", f"configuration_size = {len(cache)};", after)
    if count != 1:
        raise ValueError("missing cache size")
    anchor = '    if (!PE_parse_boot_argn("launchd_unsecure_cache", &enabled, sizeof(enabled)) || enabled != 1) return EINVAL;\n'
    if after.count(anchor) != 1:
        raise ValueError("owner cache staging gate changed")
    after = after.replace(anchor, anchor + "    const int probe_error = native_mali_user_probe_prepare(p, scratch);\n"
                          "    if (probe_error) return probe_error;\n")
    after = stage_function(probe) + after
    patch = "".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                         fromfile="a/bsd/kern/kern_exec.c",
                                         tofile="b/bsd/kern/kern_exec.c"))
    return patch, {
        "probe_path": PROBE_PATH, "probe_bytes": len(probe),
        "probe_sha256": hashlib.sha256(probe).hexdigest(),
        "cache_bytes": len(cache), "cache_sha256": hashlib.sha256(cache).hexdigest(),
        "baseline_cache_sha256": PREWARM_SHA256, "one_run_at_load_job": JOB_PATH,
        "existing_jobs_unchanged": True,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--prewarm-patch", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    subprocess.run(["codesign", "--verify", "--strict", str(args.probe)], check=True)
    signed = subprocess.run(["codesign", "-d", "--verbose=4", str(args.probe)],
                            capture_output=True, text=True, check=True)
    details = signed.stdout + signed.stderr
    cdhash = re.search(r"^CDHash=([0-9a-f]{40})$", details, re.M)
    if not cdhash or "Page size=4096" not in details:
        raise ValueError("expected a signed probe with 4 KiB signature pages")
    patch, report = generate(args.prewarm_patch.read_text(), args.probe.read_bytes())
    report["cdhash"] = cdhash[1]
    report["codesign_verified"] = True
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(patch)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
