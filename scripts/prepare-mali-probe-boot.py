#!/usr/bin/env python3
"""Generate the explicitly enabled Mali diagnostic kernel patch."""
import argparse
import difflib
import hashlib
import json
import plistlib
import re
import struct
import subprocess
import zlib
from pathlib import Path

PROBE_PATH = "/private/var/ngpu/mali-user/mali-user-probe"
BOOT_ARG = "bbk-mali-userspace"
HARD_PAGEZERO = 0x100000000
PROBE_ENTITLEMENTS = {
    "com.apple.private.security.no-container": True,
    "com.apple.security.iokit-user-client-class": ["NativeSprdMaliUserClient"],
}
TEMPLATE = Path(__file__).with_name("tools") / "mali-user-stage.inc"
HELPER_CONTEXT = (
    "}\n#endif\n\n"
    "static int\n"
    "load_init_program_at_path(proc_t p, user_addr_t scratch_addr, const char* path, user_addr_t envp)\n"
    "{\n"
)
CALL_CONTEXT = (
    "#if CONFIG_MEMORYSTATUS\n"
    "\t(void) memorystatus_init_at_boot_snapshot();\n"
    "#endif /* CONFIG_MEMORYSTATUS */\n"
)


def c_array(name, data):
    lines = [f"    static const uint8_t {name}[] = {{"]
    lines += ["        " + ", ".join(f"0x{byte:02x}" for byte in data[at:at + 16]) + ","
              for at in range(0, len(data), 16)]
    return "\n".join(lines + ["    };"])


def anchored_hunk(before, after, offset):
    diff = list(difflib.unified_diff(before.splitlines(True), after.splitlines(True), n=3))
    if len(diff) < 3:
        raise ValueError("empty generated hunk")
    # Context is an interior excerpt of kern_exec.c. Git relocates the hunk
    # by its exact context after upstream and board patches have been applied.
    return re.sub(r"(?m)^@@ -(\d+)(,\d+)? \+(\d+)(,\d+)? @@",
                  lambda m: f"@@ -{int(m[1]) + offset}{m[2] or ''} "
                            f"+{int(m[3]) + offset}{m[4] or ''} @@", "".join(diff[2:]))


def validate_layout(probe):
    if not 32 <= len(probe) <= 512 * 1024:
        raise ValueError("unexpected signed probe size")
    magic, cpu, _, filetype, commands, command_size, _, _ = struct.unpack_from("<8I", probe)
    if (magic, cpu, filetype) != (0xFEEDFACF, 0x0100000C, 2):
        raise ValueError("probe must be a thin ARM64 Mach-O executable")
    end = 32 + command_size
    if end > len(probe) or commands > command_size // 8:
        raise ValueError("invalid Mach-O command table")
    at = 32
    pagezero = text = None
    for _ in range(commands):
        if at + 8 > end:
            raise ValueError("truncated Mach-O load command")
        command, size = struct.unpack_from("<II", probe, at)
        if size < 8 or size % 8 or at + size > end:
            raise ValueError("invalid Mach-O load command size")
        if command == 0x19:  # LC_SEGMENT_64
            if size < 72:
                raise ValueError("truncated 64-bit segment")
            name, va, vm_size, offset, length, max_prot, init_prot = struct.unpack_from(
                "<16s4Q2I", probe, at + 8)
            name = name.rstrip(b"\0")
            if name == b"__PAGEZERO":
                if pagezero is not None or va or offset or length or max_prot or init_prot:
                    raise ValueError("invalid __PAGEZERO")
                pagezero = vm_size
            if name == b"__TEXT":
                if text is not None or offset or length < end or length > len(probe) or init_prot != 5:
                    raise ValueError("invalid __TEXT")
                text = va
        at += size
    if at != end:
        raise ValueError("Mach-O command extent does not match header")
    if pagezero is None or pagezero < HARD_PAGEZERO:
        raise ValueError(f"ARM64 XNU requires a 4 GiB hard __PAGEZERO (got {pagezero or 0:#x})")
    if text is None or text < pagezero or text % 0x4000:
        raise ValueError("__TEXT overlaps __PAGEZERO or is not 16 KiB aligned")
    return pagezero


def generate(probe):
    pagezero = validate_layout(probe)
    split = "\n/* @MALI_PROBE_BOOT_CALL@ */\n"
    helper, invoke = TEMPLATE.read_text(encoding="utf-8").split(split)
    helper = helper.replace("@PROBE_PATH@", PROBE_PATH).replace("@BOOT_ARG@", BOOT_ARG)
    invoke = invoke.replace("@BOOT_ARG@", BOOT_ARG)
    helper = helper.replace("@PROBE_BYTES@", str(len(probe)))
    helper = helper.replace("@PROBE_PACKED@", c_array("probe_packed", zlib.compress(probe, 9)))
    helper = helper.replace("@PROBE_SHA@", c_array("probe_sha", hashlib.sha256(probe).digest()))
    if re.search(r"@[A-Z_]+@", helper + invoke):
        raise ValueError("unexpanded template token")
    helper_after = HELPER_CONTEXT.replace("\nstatic int\n", "\n" + helper + "\n\nstatic int\n", 1)
    call_after = CALL_CONTEXT.replace("#if CONFIG_MEMORYSTATUS\n", invoke + "\n#if CONFIG_MEMORYSTATUS\n", 1)
    helper_hunk = anchored_hunk(HELPER_CONTEXT, helper_after, 26264)
    helper_lines = helper_after.count("\n") - HELPER_CONTEXT.count("\n")
    call_hunk = anchored_hunk(CALL_CONTEXT, call_after, 26423)
    call_hunk = re.sub(r"(?m)^(@@ -\d+(?:,\d+)? \+)(\d+)(,\d+)? @@",
                       lambda m: f"{m[1]}{int(m[2]) + helper_lines}{m[3] or ''} @@", call_hunk)
    patch = "--- a/bsd/kern/kern_exec.c\n+++ b/bsd/kern/kern_exec.c\n" + helper_hunk + call_hunk
    return patch, {
        "probe_path": PROBE_PATH, "probe_bytes": len(probe),
        "probe_sha256": hashlib.sha256(probe).hexdigest(),
        "boot_arg": BOOT_ARG + "=1",
        "pagezero_bytes": pagezero,
        "launch_mode": "boot-posix-spawn-child",
        "changes_stock_launchd": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
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
    entitlement_blob = subprocess.check_output(
        ["codesign", "-d", "--entitlements", ":-", str(args.probe)], stderr=subprocess.DEVNULL)
    actual_entitlements = plistlib.loads(entitlement_blob)
    if actual_entitlements != PROBE_ENTITLEMENTS:
        raise ValueError("probe requires exactly its own IOKit client class and no-container grant")
    patch, report = generate(args.probe.read_bytes())
    report["cdhash"] = cdhash[1]
    report["codesign_verified"] = True
    report["entitlements"] = actual_entitlements
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(patch, encoding="utf-8", newline="\n")
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
