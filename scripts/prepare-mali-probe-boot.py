#!/usr/bin/env python3
"""Stage the signed board GPU probe through the existing owner boot writer.

Publish one RunAtLoad job as both a real daemon plist and a cache record, as
for a stock system daemon. Keep the original daemons visible through a
read-only union and stage only the exact signed probe executable.
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
from pathlib import Path, PurePosixPath

PREWARM_SHA256 = "94433b3eee2e4fefe8f938a79f472cca384d8c0563d59d08c72f15cc3d0aa953"
PROBE_PATH = "/private/var/ngpu/mali-user/mali-user-probe"
JOB_PATH = "/System/Library/LaunchDaemons/org.unixnu.bbk.mali-probe.plist"
JOB_DIR = str(PurePosixPath(JOB_PATH).parent)
JOB_SOURCE_DIR = "/private/var/ngpu/mali-user/LaunchDaemons"
JOB_SOURCE = f"{JOB_SOURCE_DIR}/{PurePosixPath(JOB_PATH).name}"
STOCK_JOBS = ("com.apple.splashboardd.plist", "com.apple.watchdogd.plist")


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


def function_line(patch):
    line = None
    for text in patch.splitlines():
        hunk = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@", text)
        if hunk:
            line = int(hunk[1])
        elif line is not None and text.startswith("+/* The original loader consumes"):
            return line
        elif line is not None and text.startswith((" ", "+")):
            line += 1
    raise ValueError("missing prewarm function source location")


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


def probe_job():
    return {
        "Label": "org.unixnu.bbk.mali-probe",
        "ProgramArguments": [PROBE_PATH],
        "UserName": "root",
        "RunAtLoad": True,
        "KeepAlive": False,
        "ProcessType": "Background",
        "StandardOutPath": "/private/var/ngpu/mali-user/stdout.log",
        "StandardErrorPath": "/private/var/ngpu/mali-user/stderr.log",
    }


def build_cache(original, job):
    if hashlib.sha256(original).hexdigest() != PREWARM_SHA256:
        raise ValueError("the qualified launchd prewarm cache changed")
    baseline = plistlib.loads(original)
    merged = copy.deepcopy(baseline)
    jobs = merged["LaunchDaemons"]
    if JOB_PATH in jobs or any(record.get("Label") == job["Label"] for record in jobs.values()):
        raise ValueError("GPU probe job already present")
    jobs[JOB_PATH] = job
    encoded = plistlib.dumps(merged, fmt=plistlib.FMT_BINARY, sort_keys=False)
    # Compare the full decoded record, including all stock launch policies.
    restored = plistlib.loads(encoded)
    added = restored["LaunchDaemons"].pop(JOB_PATH)
    if restored != baseline or added != jobs[JOB_PATH]:
        raise ValueError("an existing launchd cache entry changed")
    return encoded


def stage_function(probe, job_plist):
    packed = zlib.compress(probe, 9)
    stock_jobs = "".join(f'        "{JOB_DIR}/{name}",\n' for name in STOCK_JOBS)
    return ("static int\nnative_mali_probe_check_file(const char *path, const uint8_t *bytes, size_t size,\n"
            "    uint32_t mode, vfs_context_t ctx)\n{\n"
            "    vnode_t vp = NULLVP;\n"
            "    int error = vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);\n"
            "    if (error) return error;\n"
            "    struct vnode_attr attr;\n"
            "    VATTR_INIT(&attr);\n"
            "    VATTR_WANTED(&attr, va_uid);\n"
            "    error = vnode_getattr(vp, &attr, ctx);\n"
            "    if (!error && (!VATTR_IS_SUPPORTED(&attr, va_uid) || attr.va_uid != 0 || vnode_vtype(vp) != VREG ||\n"
            "        !native_gpu_addon_equal(vp, bytes, size, mode, ctx))) error = EIO;\n"
            "    const int closed = vnode_close(vp, FREAD, ctx);\n"
            "    return error ? error : closed;\n}\n\n"
            "static int\nnative_mali_probe_install_file(proc_t p, user_addr_t scratch, const char *path,\n"
            "    const uint8_t *bytes, size_t size, uint32_t mode, vfs_context_t ctx)\n{\n"
            "    int error = native_gpu_addon_parents(p, scratch, path);\n"
            "    if (error) return error;\n"
            "    error = native_gpu_addon_write(path, bytes, size, mode, ctx);\n"
            "    if (error && error != -1) return error;\n"
            "    return native_mali_probe_check_file(path, bytes, size, mode, ctx);\n}\n\n"
            "static int\nnative_mali_user_probe_prepare(proc_t p, user_addr_t scratch)\n{\n"
            f'    static const char path[] = "{PROBE_PATH}";\n'
            f'    static const char job_source[] = "{JOB_SOURCE}";\n'
            f'    static const char job_path[] = "{JOB_PATH}";\n'
            f'    static const char source_dir[] = "{JOB_SOURCE_DIR}";\n'
            f'    static const char target_dir[] = "{JOB_DIR}";\n'
            "    static const char *const stock_jobs[] = {\n" + stock_jobs + "    };\n"
            + c_array("probe_packed", packed) + "\n"
            + c_array("probe_sha", hashlib.sha256(probe).digest()) + "\n"
            + c_array("probe_job_plist", job_plist) + "\n"
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
            "    if (!error) error = native_mali_probe_install_file(p, scratch, path, buffer, bytes, 0755, ctx);\n"
            "    kfree_data(buffer, bytes);\n"
            '    printf("M156 mali-user: staged bytes=%u verified=%d err=%d\\n", (unsigned)bytes, !error, error);\n'
            "    if (error) return error;\n"
            "    vnode_t existing = NULLVP;\n"
            "    error = vnode_lookup(job_path, 0, &existing, ctx);\n"
            "    if (!error) { vnode_put(existing); error = EEXIST; }\n"
            "    else if (error == ENOENT) {\n"
            "        /* Only a fresh directory containing this one job can cover the system directory. */\n"
            "        error = vnode_lookup(source_dir, 0, &existing, ctx);\n"
            "        if (!error) { vnode_put(existing); error = EEXIST; }\n"
            "        else if (error == ENOENT) error = native_mali_probe_install_file(p, scratch, job_source,\n"
            "            probe_job_plist, sizeof(probe_job_plist), 0644, ctx);\n"
            "    }\n"
            "    int mounted = 0;\n"
            "    if (!error) {\n"
            "        error = native_cryptex_os_bind(p, scratch, source_dir, target_dir, MNT_UNION);\n"
            "        mounted = !error;\n"
            "    }\n"
            "    if (!error) error = native_mali_probe_check_file(job_path, probe_job_plist, sizeof(probe_job_plist), 0644, ctx);\n"
            "    const int job_visible = !error;\n"
            "    unsigned stock_visible = 0;\n"
            "    for (size_t i = 0; !error && i < sizeof(stock_jobs) / sizeof(stock_jobs[0]); ++i) {\n"
            "        vnode_t vp = NULLVP;\n"
            "        error = vnode_lookup(stock_jobs[i], 0, &vp, ctx);\n"
            "        if (!error) {\n"
            "            if (vnode_vtype(vp) == VREG) ++stock_visible;\n"
            "            else error = EINVAL;\n"
            "            vnode_put(vp);\n"
            "        }\n"
            "    }\n"
            "    int rollback = 0;\n"
            "    if (error && mounted) {\n"
            "        struct unmount_args request = {.path = scratch + 0xa20, .flags = 0};\n"
            "        int32_t result = 0;\n"
            "        rollback = copyout(target_dir, request.path, sizeof(target_dir));\n"
            "        if (!rollback) rollback = unmount(p, &request, &result);\n"
            "    }\n"
            '    printf("M156 mali-user: daemon plist visible=%d stock=%u/%u err=%d rollback=%d\\n",\n'
            "        job_visible, stock_visible, (unsigned)(sizeof(stock_jobs) / sizeof(stock_jobs[0])), error, rollback);\n"
            '    if (rollback) panic("A2 Mali daemon overlay rollback failed: %d", rollback);\n'
            "    return error;\n}\n\n")


def generate(prewarm_patch, probe):
    if not 32 <= len(probe) <= 512 * 1024:
        raise ValueError("unexpected signed probe size")
    if struct.unpack_from("<II", probe) != (0xFEEDFACF, 0x0100000C):
        raise ValueError("probe must be a thin ARM64 Mach-O")
    before = extract_function(prewarm_patch)
    original = zlib.decompress(array_bytes(before, "packed"))
    job = probe_job()
    job_plist = plistlib.dumps(job, fmt=plistlib.FMT_BINARY, sort_keys=False)
    cache = build_cache(original, job)
    after = replace_array(before, "packed", zlib.compress(cache, 9))
    after = replace_array(after, "modified_sha", hashlib.sha256(cache).digest())
    after, count = re.subn(r"configuration_size = \d+;", f"configuration_size = {len(cache)};", after)
    if count != 1:
        raise ValueError("missing cache size")
    anchor = '        !error, visible, (unsigned)configuration_size, error);\n'
    if after.count(anchor) != 1:
        raise ValueError("owner cache staging gate changed")
    after = after.replace(anchor, anchor + "    if (!error) {\n"
                          "        const int probe_error = native_mali_user_probe_prepare(p, scratch);\n"
                          '        if (probe_error) printf("M156 mali-user: probe setup failed err=%d\\n", probe_error);\n'
                          "    }\n")
    after = stage_function(probe, job_plist) + after
    patch = "".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                         fromfile="a/bsd/kern/kern_exec.c",
                                         tofile="b/bsd/kern/kern_exec.c"))
    # The excerpt is inside kern_exec.c. A hunk at line one would tell git
    # that its context must match the beginning of the translation unit.
    offset = function_line(prewarm_patch) - 1
    patch = re.sub(r"(?m)^@@ -(\d+)(,\d+)? \+(\d+)(,\d+)? @@",
                   lambda m: f"@@ -{int(m[1]) + offset}{m[2] or ''} "
                             f"+{int(m[3]) + offset}{m[4] or ''} @@", patch)
    return patch, {
        "probe_path": PROBE_PATH, "probe_bytes": len(probe),
        "probe_sha256": hashlib.sha256(probe).hexdigest(),
        "cache_bytes": len(cache), "cache_sha256": hashlib.sha256(cache).hexdigest(),
        "baseline_cache_sha256": PREWARM_SHA256, "one_run_at_load_job": JOB_PATH,
        "daemon_plist_source": JOB_SOURCE,
        "daemon_plist_bytes": len(job_plist),
        "daemon_plist_sha256": hashlib.sha256(job_plist).hexdigest(),
        "stock_daemon_checks": list(STOCK_JOBS),
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
