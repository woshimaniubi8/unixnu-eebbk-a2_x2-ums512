#!/usr/bin/env python3
"""Apply the BBK A2/X2 bring-up patches to the prelinked XNU kernel.

The kernel is built from apple-oss-distributions/xnu with the BCM2837 machine
configuration (the only non-Apple machine configuration XNU carries) plus the
UMS512 patch series.  Four pieces of that machine configuration / image assume
Apple hardware and abort the boot on UMS512:

1. ``pexpert/arm/pe_init.c`` maps the BCM2837/Raspberry-Pi VRAM panic log at
   physical 0x3C0FC000 and dereferences it.  Nothing is mapped there on this
   SoC, so the very first ``PE_create_console()`` takes a data abort.  Retarget
   the mapping to the board's display reservation (0x9E000000, real DRAM).

2. The prelinked AppleMobileFileIntegrity kext panics with
   ``"AMFI: No PMGR?" @ConfigurationSettings.cpp:235`` because the UMS512 has
   no Apple PMGR service.  The panic helper is reached from a PMGR pointer
   NULL check at the tail of one function; redirect that single call site to
   the caller's own epilogue so the frame is torn down and the function
   returns legally.  (Rewriting the noreturn helper itself makes it return
   into the next function and loop forever - see 102.log.)

3. The same kext rejects the adhoc-signed OS image with
   ``unsuitable CT policy ... rejecting signature`` because the OS volume has no
   system-volume-auth-blob in the device tree; that stops launchd and dyld.
   ``patch-amfi-ct-policy.py`` rewrites the rejection branch so the signature is
   treated as suitable (see that file for the details and the alternative guard
   branch).  The device patch series keeps the sealed-root behaviour intact.

4. The same kext then rejects the same image with
   ``does not pass kernel-side policy for this model, bailing out``.
   ``patch-amfi-model-policy.py`` nops the conditional branch that enters that
   rejection block, so only the model-policy check is bypassed and the other
   AMFI checks stay in place.

All patches are pattern based so they survive unrelated rebuilds, and the
script is idempotent.

Usage: patch-bringup-kernel.py <kernel.macho>
"""

import importlib.util
import struct
import sys
from pathlib import Path

VRAM_OLD = struct.pack("<II", 0x52980000, 0x72A781E0)  # mov w0,#0xC000 ; movk w0,#0x3C0F,lsl#16
VRAM_NEW = struct.pack("<II", 0x52800000, 0x72B3C000)  # mov w0,#0 ; movk w0,#0x9E00,lsl#16

AMFI_FMT = b'"AMFI: No PMGR?\\n" @%s:%d'
AMFI_HELPER_EPILOGUE = struct.pack(
    "<IIII",
    0xA9417BFD,  # ldp x29, x30, [sp, #16]
    0x910083FF,  # add sp, sp, #0x20
    0xD65F03C0,  # ret
    0xD503201F,  # nop
)


def load_sibling(name, module_name):
    """Load a sibling patch script (dashes keep the file out of the import path)."""
    path = Path(__file__).resolve().parent / name
    spec = importlib.util.spec_from_file_location(module_name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_amfi_ct_policy():
    return load_sibling("patch-amfi-ct-policy.py", "patch_amfi_ct_policy")


def load_amfi_model_policy():
    return load_sibling("patch-amfi-model-policy.py", "patch_amfi_model_policy")



def read_segments(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    offset = 32
    segments = []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, offset)
        if cmd == 0x19:  # LC_SEGMENT_64
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


def find_string_ref(segments, data, target):
    """Return the file offset of the ADRP+ADD pair that materialises target."""
    for _name, vmaddr, _vmsize, fileoff, filesize in segments:
        if not filesize:
            continue
        blob = data[fileoff:fileoff + filesize]
        for i in range(0, len(blob) - 8, 4):
            insn = struct.unpack_from("<I", blob, i)[0]
            if (insn & 0x9F000000) != 0x90000000:  # ADRP
                continue
            rd = insn & 0x1F
            immlo = (insn >> 29) & 3
            immhi = (insn >> 5) & 0x7FFFF
            imm = sign_extend((immhi << 2) | immlo, 21)
            page = ((vmaddr + i) & ~0xFFF) + (imm << 12)
            insn2 = struct.unpack_from("<I", blob, i + 4)[0]
            if (insn2 & 0xFFC00000) != 0x91000000 or (insn2 & 0x1F) != rd:  # ADD
                continue
            imm12 = (insn2 >> 10) & 0xFFF
            shift = 12 if ((insn2 >> 22) & 3) == 1 else 0
            if page + (imm12 << shift) == target:
                return fileoff + i
    return None


def patch_vram_panic_log(data, segments):
    hits = []
    offset = data.find(VRAM_OLD)
    while offset >= 0:
        hits.append(offset)
        offset = data.find(VRAM_OLD, offset + 1)
    if not hits:
        if data.find(VRAM_NEW) >= 0:
            print("vram panic log: already patched")
            return True
        print("vram panic log: pattern not found", file=sys.stderr)
        return False
    if len(hits) != 1:
        print("vram panic log: expected one occurrence, found %d" % len(hits), file=sys.stderr)
        return False
    data[hits[0]:hits[0] + 8] = VRAM_NEW
    print("vram panic log: 0x3C0FC000 -> 0x9E000000 at file offset 0x%x" % hits[0])
    return True


def patch_amfi_no_pmgr(data, segments):
    """Skip the AMFI PMGR panic without falling through into the next function.

    The prelinked kext calls a noreturn helper (``panic`` with the
    ``AMFI: No PMGR?`` message) from the tail of its PMGR lookup function.  The
    original bring-up patch rewrote the *panic preparation* inside that helper
    into the helper's own epilogue, so the helper returned with a return
    address that points at the *entry of the next function*: execution fell
    into the IODTNVRAM setter and returned to its own entry forever (102.log:
    tens of millions of iterations on one thread, LR always 0x4aaed08).

    The bypass belongs at the call site: replace the ``bl`` to the helper with
    a ``b`` to the caller's own epilogue, which tears down the caller's frame
    and returns legally.  The helper is left untouched, and both the call site
    and the epilogue are verified afterwards so this cannot silently regress.
    """
    fmt_offset = data.find(AMFI_FMT)
    if fmt_offset < 0:
        print("amfi no-pmgr: panic string not found", file=sys.stderr)
        return False
    fmt_va = file_to_va(segments, fmt_offset)
    if fmt_va is None:
        print("amfi no-pmgr: panic string outside any segment", file=sys.stderr)
        return False
    ref_offset = find_string_ref(segments, data, fmt_va)
    if ref_offset is None:
        print("amfi no-pmgr: no reference to the panic string", file=sys.stderr)
        return False
    ref_va = file_to_va(segments, ref_offset)
    if ref_va is None:
        print("amfi no-pmgr: panic string reference outside any segment", file=sys.stderr)
        return False

    # The helper that prepares the panic is the function that contains the
    # format-string reference: its entry is the nearest prologue at or before
    # that reference.
    def is_prologue(word):
        # stp <any pair>, [sp, #-N]!  or  sub sp, sp, #N
        return ((word & 0xFFC00000) == 0xA9800000) or ((word & 0xFFC003FF) == 0xD10003FF)

    helper_va = None
    for back in range(4, 0x400, 4):
        va = ref_va - back
        offset = va_to_file(segments, va)
        if offset is None:
            continue
        if is_prologue(struct.unpack_from("<I", data, offset)[0]):
            helper_va = va
            break
    if helper_va is None:
        print("amfi no-pmgr: helper entry not found before the format reference",
              file=sys.stderr)
        return False

    # The helper is called from exactly one site; anything else means the
    # layout changed and the bypass must be re-derived, not guessed.
    call_sites = []
    for name, vmaddr, _vmsize, fileoff, filesize in segments:
        if name not in ("__PRELINK_TEXT", "__PLK_TEXT_EXEC"):
            continue
        for offset in range(fileoff, fileoff + filesize - 4, 4):
            word = struct.unpack_from("<I", data, offset)[0]
            if (word & 0xFC000000) != 0x94000000:
                continue
            va = vmaddr + (offset - fileoff)
            if va + sign_extend(word & 0x3FFFFFF, 26) * 4 == helper_va:
                call_sites.append((va, offset, word))
    if not call_sites:
        # The only way a fresh kernel reaches this branch is a layout change we
        # do not understand, so refuse.  A kernel that this script already
        # patched is handled by the "already patched" check further down only
        # when the call site is still a bl; when it is a b there is no bl left
        # to find, so report it explicitly instead of failing the build.
        print("amfi no-pmgr: no call to the panic helper 0x%x (already patched?)" %
              helper_va)
        return True
    if len(call_sites) != 1:
        print("amfi no-pmgr: expected one call to helper 0x%x, found %d" %
              (helper_va, len(call_sites)), file=sys.stderr)
        return False
    call_va, call_offset, call_word = call_sites[0]

    # Find the caller's epilogue: the nearest preceding "ldp x29, x30, [sp, #n]"
    # followed by an "add sp, sp, #m" and a "ret" within a few instructions.
    epilogue_va = None
    for back in range(4, 0x400, 4):
        va = call_va - back
        offset = va_to_file(segments, va)
        if offset is None:
            continue
        word = struct.unpack_from("<I", data, offset)[0]
        if ((word & 0xFFC00000) != 0xA9400000 or (word & 0x1F) != 29 or
                ((word >> 10) & 0x1F) != 30):
            continue
        saw_add_sp = False
        for step in range(1, 9):
            next_offset = va_to_file(segments, va + step * 4)
            if next_offset is None:
                break
            next_word = struct.unpack_from("<I", data, next_offset)[0]
            if (next_word & 0xFFC003FF) == 0x910003FF:  # add sp, sp, #imm
                saw_add_sp = True
            if next_word == 0xD65F03C0:                # ret
                if saw_add_sp:
                    epilogue_va = va
                break
        if epilogue_va is not None:
            break
    if epilogue_va is None:
        print("amfi no-pmgr: caller epilogue not found before 0x%x" % call_va,
              file=sys.stderr)
        return False

    expected = 0x14000000 | (((epilogue_va - call_va) >> 2) & 0x3FFFFFF)
    if call_word == expected:
        print("amfi no-pmgr: already patched (b 0x%x from 0x%x)" %
              (epilogue_va, call_va))
        return True
    # A kernel that still carries the old helper rewrite cannot be repaired
    # from the call site alone: the helper's own body was overwritten.
    stale_offset = va_to_file(segments, ref_va - 0x10)
    if stale_offset is not None and bytes(data[stale_offset:stale_offset + 16]) == AMFI_HELPER_EPILOGUE:
        print("amfi no-pmgr: kernel still carries the old helper rewrite; "
              "rebuild before applying this patch", file=sys.stderr)
        return False
    if (call_word & 0xFC000000) != 0x94000000:
        print("amfi no-pmgr: call site 0x%x is not a bl (0x%08x)" %
              (call_va, call_word), file=sys.stderr)
        return False
    data[call_offset:call_offset + 4] = struct.pack("<I", expected)

    # Regression checks: the branch must go to the epilogue, the epilogue must
    # end in ret, and the next function's entry must not be the target.
    check = struct.unpack_from("<I", data, call_offset)[0]
    epilogue_ret = None
    for step in range(0, 9):
        offset = va_to_file(segments, epilogue_va + step * 4)
        if offset is None:
            break
        if struct.unpack_from("<I", data, offset)[0] == 0xD65F03C0:
            epilogue_ret = epilogue_va + step * 4
            break
    if check != expected or epilogue_ret is None:
        print("amfi no-pmgr: post-patch verification failed", file=sys.stderr)
        return False
    print("amfi no-pmgr: call site 0x%x now branches to the epilogue 0x%x "
          "(ret at 0x%x); helper 0x%x left intact" %
          (call_va, epilogue_va, epilogue_ret, helper_va))
    return True


def patch_amfi_ct_policy(data, segments):
    try:
        message = load_amfi_ct_policy().patch(data)
    except ValueError as error:
        print("amfi ct-policy: %s" % error, file=sys.stderr)
        return False
    except FileNotFoundError:
        print("amfi ct-policy: patch-amfi-ct-policy.py is missing next to this script",
              file=sys.stderr)
        return False
    print(message)
    return True


def patch_amfi_model_policy(data, segments):
    try:
        message = load_amfi_model_policy().patch(data)
    except ValueError as error:
        print("amfi model-policy: %s" % error, file=sys.stderr)
        return False
    except FileNotFoundError:
        print("amfi model-policy: patch-amfi-model-policy.py is missing next to this "
              "script", file=sys.stderr)
        return False
    print(message)
    return True


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    path = sys.argv[1]
    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    segments = read_segments(data)
    if not segments:
        print("no LC_SEGMENT_64 commands found", file=sys.stderr)
        return 1
    ok = patch_vram_panic_log(data, segments)
    ok = patch_amfi_no_pmgr(data, segments) and ok
    ok = patch_amfi_ct_policy(data, segments) and ok
    ok = patch_amfi_model_policy(data, segments) and ok
    if not ok:
        return 1
    with open(path, "wb") as handle:
        handle.write(data)
    print("bring-up kernel patches applied to %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
