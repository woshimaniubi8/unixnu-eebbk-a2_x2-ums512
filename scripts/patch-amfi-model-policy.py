#!/usr/bin/env python3
"""Accept AMFI signatures that fail the kernel-side model policy.

The release AppleMobileFileIntegrity kext rejects an adhoc-signed binary whose
signature does not satisfy the kernel-side policy of the *device model*:

    AMFI: '<path>' does not pass kernel-side policy for this model, bailing out.

On the BBK A2/X2 bring-up that stops ``/sbin/launchd`` and ``/usr/lib/dyld``
right after the CT-policy patch made them pass the first check.  The rejection
is a single branch in AMFI's signature validator:

    0xfffffff004ab6674  adrp x8, <policy flag page>
    0xfffffff004ab6678  ldrb w8, [x8, #0x9c1]   ; model policy flag (0 here)
    0xfffffff004ab667c  cbz  w8, <reject block> ; <- this branch
    0xfffffff004ab6680  ...                     ; policy-passed continuation
    ...
    0xfffffff004ab6844  str  x23, [sp]          ; reject block: log varargs
    0xfffffff004ab6848  adrp x0, <format string>
    0xfffffff004ab684c  add  x0, x0, #0xc21
    0xfffffff004ab6850  bl   <AMFI log>

Replace the reject block's first instruction with an unconditional branch to
the policy-passed continuation (the instruction right after the cbz), so the
block is still entered but immediately continues as if the policy had passed:
only this model-policy check is bypassed, the other AMFI checks (including the
CT-policy one patched separately) stay as they are.  Patching the block entry
instead of the cbz keeps the patch idempotent, because the block is located by
content every time (format string -> adrp+add reference -> the block that logs
it); the branch into it is only needed on the first run.  Fails closed.

Usage: patch-amfi-model-policy.py <kernel.macho> [--check]
"""

import struct
import sys

FORMAT_STRING = b"AMFI: '%s' does not pass kernel-side policy for this model, bailing out.\n"
BRANCH_OPCODE = 0x14000000
BRANCH_MASK = 0xFC000000
VARARGS_STORES = (0xF90003F7,  # str x23, [sp]
                  0xA9006FF7)  # stp x23, x27, [sp]


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


def va_to_file(segments, address):
    for _name, vmaddr, _vmsize, fileoff, filesize in segments:
        if vmaddr <= address < vmaddr + filesize:
            return fileoff + (address - vmaddr)
    return None


def sign_extend(value, bits):
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return value


def branch_target(word, pc):
    """Return the branch target of an unconditional/conditional branch, else None."""
    if (word & 0x7C000000) == 0x14000000:      # b / bl (bit 31 clear for b)
        return pc + sign_extend(word & 0x3FFFFFF, 26) * 4
    if (word & 0xFF000010) == 0x54000000:      # b.cond
        return pc + sign_extend((word >> 5) & 0x7FFFF, 19) * 4
    if (word & 0x7E000000) == 0x34000000:      # cbz / cbnz
        return pc + sign_extend((word >> 5) & 0x7FFFF, 19) * 4
    if (word & 0x7E000000) == 0x36000000:      # tbz / tbnz
        return pc + sign_extend((word >> 5) & 0x3FFF, 14) * 4
    return None


def is_conditional_branch(word):
    return (word & 0xFF000010) == 0x54000000 or \
           (word & 0x7E000000) == 0x34000000 or \
           (word & 0x7E000000) == 0x36000000


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
    """Return (branch_offset, branch_va, branch_word) of the model-policy rejection."""
    if segments is None:
        segments = read_segments(data)
    if data.count(FORMAT_STRING) != 1:
        raise ValueError("expected exactly one AMFI model-policy format string, found %d"
                         % data.count(FORMAT_STRING))
    string_va = file_to_va(segments, data.find(FORMAT_STRING))
    if string_va is None:
        raise ValueError("the AMFI model-policy format string is not file-backed")
    refs = find_string_refs(data, segments, string_va)
    if len(refs) != 1:
        raise ValueError("expected exactly one reference to the AMFI model-policy format "
                         "string, found %d" % len(refs))
    block_start = refs[0] - 4
    if block_start < 0:
        raise ValueError("the reject block starts before the file")
    block_word = struct.unpack_from("<I", data, block_start)[0]
    block_va = file_to_va(segments, block_start)
    if block_va is None:
        raise ValueError("the reject block is not file-backed")
    if (block_word & BRANCH_MASK) == BRANCH_OPCODE:
        target = branch_target(block_word, block_va)
        if target is None or va_to_file(segments, target) is None:
            raise ValueError("the reject block starts with an unexpected branch 0x%08x"
                             % block_word)
        return "patched", block_start, block_va, block_word
    if block_word not in VARARGS_STORES:
        raise ValueError("unexpected reject block prologue 0x%08x at file 0x%x"
                         % (block_word, block_start))
    candidates = []
    for _name, vmaddr, _vmsize, fileoff, filesize in segments:
        if not filesize:
            continue
        blob = data[fileoff:fileoff + filesize]
        for index in range(0, len(blob) - 4, 4):
            word = struct.unpack_from("<I", blob, index)[0]
            pc = vmaddr + index
            if branch_target(word, pc) == block_va:
                candidates.append((fileoff + index, pc, word))
    if len(candidates) != 1:
        raise ValueError("expected exactly one branch into the AMFI model-policy reject "
                         "block, found %d" % len(candidates))
    branch_offset, branch_va, branch = candidates[0]
    if not is_conditional_branch(branch):
        raise ValueError("the branch into the reject block is not conditional (0x%08x)"
                         % branch)
    accept = branch_va + 4
    delta = accept - block_va
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError("the policy-passed continuation 0x%x is out of branch range"
                         % accept)
    return "patch", block_start, block_va, BRANCH_OPCODE | ((delta // 4) & 0x3FFFFFF)


def patch(data, segments=None):
    """Redirect the model-policy rejection block to the policy-passed path."""
    state, block_offset, block_va, word = locate(data, segments)
    if state == "patched":
        return "amfi model-policy: already patched (b 0x%x) at 0x%x" % (
            branch_target(word, block_va), block_va)
    data[block_offset:block_offset + 4] = struct.pack("<I", word)
    return "amfi model-policy: reject block 0x%x now branches to the policy-passed " \
           "path (0x%x)" % (block_va, branch_target(word, block_va))


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
        state, block_offset, block_va, word = locate(data)
    except ValueError as error:
        print("amfi model-policy: %s" % error, file=sys.stderr)
        return 1
    print("amfi model-policy: format string at 0x%x, reject block at file 0x%x "
          "(VA 0x%x)" % (data.find(FORMAT_STRING), block_offset, block_va))
    if state == "patched":
        print("amfi model-policy: already patched (b 0x%x)"
              % branch_target(word, block_va))
        return 0
    if check:
        print("amfi model-policy: patch would be applied (b 0x%x)"
              % branch_target(word, block_va))
        return 0
    data[block_offset:block_offset + 4] = struct.pack("<I", word)
    with open(path, "wb") as handle:
        handle.write(data)
    print("amfi model-policy: reject block 0x%x now branches to the policy-passed path "
          "(0x%x)" % (block_va, branch_target(word, block_va)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
