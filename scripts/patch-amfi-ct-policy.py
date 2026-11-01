#!/usr/bin/env python3
"""Accept AMFI signatures that are rejected only for an unsuitable CT policy.

The release AppleMobileFileIntegrity kext rejects a signature whose CT policy
does not match the platform:

    AMFI: '<path>': unsuitable CT policy <policy> for this platform/device,
    rejecting signature.

On the BBK A2/X2 bring-up this stops ``/sbin/launchd`` and ``/usr/lib/dyld``
from loading (the OS image is adhoc signed and has no system-volume-auth-blob),
and AMFI ignores its own ``amfi_*`` boot arguments in this release.  The
rejection is one branch in AMFI's signature validator:

    adrp x0, <page of the format string>
    add  x0, x0, <offset of the format string>
    b    <log-and-reject path>        <- this branch
    <policy-suitable continuation>

Rewrite that branch to fall through to the continuation (``b .+4``): the
signature is then treated as suitable, the rejection log is skipped and the
rejected flag is not raised.  Nothing else changes; the guard branch one
instruction earlier (``b.ne``) is an alternative patch point but is not used.

The site is located by content (format string -> adrp+add reference -> branch),
so it survives unrelated rebuilds that move the kernel layout.  The patch is
idempotent and fails closed when the expected instructions are not found.

Usage: patch-amfi-ct-policy.py <kernel.macho> [--check]
"""

import struct
import sys

FORMAT_STRING = (b"AMFI: '%s': unsuitable CT policy %#llx for this platform/device, "
                 b"rejecting signature.\n")
PATCHED_BRANCH = struct.pack("<I", 0x14000001)   # b .+4 (fall through to the continuation)
BRANCH_OPCODE = 0x14000000                        # b
BRANCH_MASK = 0xFC000000
BL_OPCODE = 0x94000000                            # bl
BL_MASK = 0xFC000000


def read_segments(data):
    """Return (name, vmaddr, vmsize, fileoff, filesize) for every LC_SEGMENT_64."""
    ncmds = struct.unpack_from("<I", data, 16)[0]
    offset = 32
    segments = []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, offset)
        if cmd == 0x19:
            name = data[offset + 8:offset + 24].split(b"\x00")[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, offset + 24)
            segments.append((name, vmaddr, vmsize, fileoff, filesize))
        offset += cmdsize
    return segments


def file_to_va(segments, offset):
    for _name, vmaddr, _vmsize, fileoff, filesize in segments:
        if fileoff <= offset < fileoff + filesize:
            return vmaddr + (offset - fileoff)
    return None


def sign_extend(value, bits):
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return value


def find_string_refs(data, segments, target):
    """Return the file offsets of every ADRP+ADD pair that materialises target."""
    hits = []
    for _name, vmaddr, _vmsize, fileoff, filesize in segments:
        if not filesize:
            continue
        blob = data[fileoff:fileoff + filesize]
        for index in range(0, len(blob) - 8, 4):
            insn = struct.unpack_from("<I", blob, index)[0]
            if (insn & 0x9F000000) != 0x90000000:
                continue
            rd = insn & 0x1F
            immlo = (insn >> 29) & 3
            immhi = (insn >> 5) & 0x7FFFF
            page = ((vmaddr + index) & ~0xFFF) + (sign_extend((immhi << 2) | immlo, 21) << 12)
            insn2 = struct.unpack_from("<I", blob, index + 4)[0]
            if (insn2 & 0xFFC00000) != 0x91000000 or (insn2 & 0x1F) != rd:
                continue
            imm12 = (insn2 >> 10) & 0xFFF
            shift = 12 if ((insn2 >> 22) & 3) == 1 else 0
            if page + (imm12 << shift) == target:
                hits.append(fileoff + index)
    return hits


def locate(data, segments=None):
    """Return (branch_offset, branch_va, branch_word) of the CT-policy rejection."""
    if segments is None:
        segments = read_segments(data)
    if data.count(FORMAT_STRING) != 1:
        raise ValueError("expected exactly one AMFI CT-policy format string, found %d"
                         % data.count(FORMAT_STRING))
    string_va = file_to_va(segments, data.find(FORMAT_STRING))
    if string_va is None:
        raise ValueError("the AMFI CT-policy format string is not file-backed")
    refs = find_string_refs(data, segments, string_va)
    if len(refs) != 1:
        raise ValueError("expected exactly one reference to the AMFI CT-policy format "
                         "string, found %d" % len(refs))
    branch_offset = refs[0] + 8
    branch = struct.unpack_from("<I", data, branch_offset)[0]
    if (branch & BRANCH_MASK) != BRANCH_OPCODE:
        raise ValueError("the instruction after the format-string reference is not a "
                         "branch (0x%08x)" % branch)
    if branch == struct.unpack("<I", PATCHED_BRANCH)[0]:
        return branch_offset, file_to_va(segments, branch_offset), branch
    if branch_offset + 4 + 4 > len(data):
        raise ValueError("the rejection branch is at the end of the file")
    continuation = struct.unpack_from("<I", data, branch_offset + 4)[0]
    if (continuation & BL_MASK) != BL_OPCODE:
        raise ValueError("the rejection branch is not followed by the policy-suitable "
                         "continuation (0x%08x)" % continuation)
    return branch_offset, file_to_va(segments, branch_offset), branch


def patch(data, segments=None):
    """Patch the rejection branch in place; returns a status message."""
    branch_offset, branch_va, branch = locate(data, segments)
    if bytes(data[branch_offset:branch_offset + 4]) == PATCHED_BRANCH:
        return "amfi ct-policy: already patched (b .+4) at 0x%x" % branch_va
    data[branch_offset:branch_offset + 4] = PATCHED_BRANCH
    return "amfi ct-policy: rejection branch 0x%08x at 0x%x now falls through (b .+4)" % (
        branch, branch_va)


def main():
    arguments = [argument for argument in sys.argv[1:] if not argument.startswith("--")]
    check = "--check" in sys.argv
    if not arguments:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    path = arguments[0]
    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    try:
        branch_offset, branch_va, branch = locate(data)
    except ValueError as error:
        print("amfi ct-policy: %s" % error, file=sys.stderr)
        return 1
    print("amfi ct-policy: format string at 0x%x, rejection branch 0x%08x at file 0x%x "
          "(VA 0x%x)" % (data.find(FORMAT_STRING), branch, branch_offset, branch_va))
    if check:
        print("amfi ct-policy: patch %s" % ("already applied" if
                                            bytes(data[branch_offset:branch_offset + 4])
                                            == PATCHED_BRANCH else "would be applied"))
        return 0
    if bytes(data[branch_offset:branch_offset + 4]) == PATCHED_BRANCH:
        print("amfi ct-policy: already patched (b .+4) at 0x%x" % branch_va)
        return 0
    data[branch_offset:branch_offset + 4] = PATCHED_BRANCH
    with open(path, "wb") as handle:
        handle.write(data)
    print("amfi ct-policy: rejection branch 0x%08x at 0x%x now falls through (b .+4)"
          % (branch, branch_va))
    return 0


if __name__ == "__main__":
    sys.exit(main())
