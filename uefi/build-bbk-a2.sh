#!/bin/sh
#
# Build the BBK A2 / X2 / X2P (Unisoc UMS512) UEFI firmware and the boot shim
# that cboot can jump to.
#
#   sh build-bbk-a2.sh [DEBUG|RELEASE]
#
# Needs an EDK2 workspace as described in analysis/uefi/FEASIBILITY.md:
# mu_basecore, mu_plus, mu_oem_sample and UniXNU/uefi-sheng (only
# Silicon/Silicium is used) reachable through PACKAGES_PATH.  The environment
# comes from $BBK_A2_ENV, then $BBK_A2_DEPS/env.sh, then env-linux.sh next to
# this script.
#
# Output (in uefi-bbk-a2/build/):
#   SILICIUM_UEFI.fd     raw firmware volume, linked for FD_BASE (0x80800000)
#   BootShim.bin         the trampoline linked at 0x80080000
#   bbk-a2-uefi.bin      BootShim.bin + SILICIUM_UEFI.fd, the boot.img kernel
#                        field (the stock-layout packer wraps it in a v2 header)
#
# The boot image itself is produced by the workspace-level tooling
# (build/rebuild.sh -> ./unixnu bootimg / build/pack-device-bootimg.py) because
# the device tree and the Android header live outside this repository.
#
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
set -e

MODE=${1:-DEBUG}

FD_BASE=${BBK_A2_FD_BASE:-0x80800000}
FD_SIZE=${BBK_A2_FD_SIZE:-0x400000}
FD_BLOCKS=${BBK_A2_FD_BLOCKS:-0x400}
ROOT=`cd \`dirname $0\` && pwd`
OUT=$ROOT/build

# Extra flags for the shim: -DBBK_ARM_WATCHDOG, -DBBK_STAGE_COLORS,
# -DBBK_KEEP_EL2, -DBBK_FB_BPP=16.  See BootShim/BootShim.S.
SHIM_FLAGS=${SHIM_FLAGS:-}

export BBK_A2_REPO=$ROOT

ENV_SH=${BBK_A2_ENV:-$ROOT/env-linux.sh}
if [ ! -f "$ENV_SH" ]; then
  ENV_SH="${BBK_A2_DEPS:-$HOME/bbk-a2-uefi}/env.sh"
fi
if [ ! -f "$ENV_SH" ]; then
  echo "== no EDK2 environment found: set BBK_A2_ENV=/path/to/env.sh ==" >&2
  exit 1
fi
echo "== environment: $ENV_SH"
. "$ENV_SH"

LLVM=${BBK_A2_LLVM_BIN:-${CLANGDWARF_BIN:-/usr/bin}}
LLVM=${LLVM%/}
PYTHON=${BBK_A2_PYTHON:-`command -v python3 || echo /usr/bin/python3`}
BUILD_DIR=${BBK_A2_BUILD_DIR:-$WORKSPACE/Build/bbkA2Pkg/${MODE}_CLANGDWARF}

mkdir -p "$OUT"

# The FDF references the Shell startup script as a workspace-relative file.
cp "$ROOT/Platforms/Unisoc/bbkA2Pkg/payload/startup.nsh" "$WORKSPACE/bbk-a2-startup.nsh"

echo "== building EDK2 platform ($MODE) =="
build -a AARCH64 -t CLANGDWARF -b "$MODE" -p bbkA2Pkg/bbkA2.dsc -n 8 \
  -D FD_BASE=$FD_BASE -D FD_SIZE=$FD_SIZE -D FD_BLOCKS=$FD_BLOCKS

FD=$BUILD_DIR/FV/SILICIUM_UEFI.fd
test -f "$FD"
cp "$FD" "$OUT/SILICIUM_UEFI.fd"

echo "== building boot shim =="
# Same flags UniXNU's scripts/unixnu/bootimg.py:compile_shim() uses, so the
# header contract it enforces holds for both packers.
$LLVM/clang --target=aarch64-none-elf -c "$ROOT/BootShim/BootShim.S" \
  -o "$OUT/BootShim.o" \
  -DFD_BASE=$FD_BASE -DFD_SIZE=$FD_SIZE -DREQUIRES_KERNEL_HEADER=1 $SHIM_FLAGS
$LLVM/llvm-objcopy -O binary "$OUT/BootShim.o" "$OUT/BootShim.bin"

echo "== assembling boot payload =="
$PYTHON "$ROOT/tools/mk-boot-payload.py" \
  "$OUT/BootShim.bin" "$OUT/SILICIUM_UEFI.fd" "$OUT/bbk-a2-uefi.bin"

echo "== verifying header contract =="
$PYTHON - "$OUT/BootShim.bin" "$OUT/SILICIUM_UEFI.fd" "$FD_BASE" "$FD_SIZE" <<'PY'
import struct, sys
shim = open(sys.argv[1], 'rb').read()
fd   = open(sys.argv[2], 'rb').read()
base = int(sys.argv[3], 16)
size = int(sys.argv[4], 16)
assert shim[56:60] == b'ARM\x64', 'no ARMd magic at offset 56'
got = struct.unpack_from('<QQ', shim, 8)
want = (base, len(shim) + size)
assert got == want, f'image header {got} != {want}'
assert len(fd) == size, f'FD is {len(fd):#x} bytes, config says {size:#x}'
print(f'shim {len(shim)} bytes, FD {len(fd):#x} bytes, header {got[0]:#x}/{got[1]:#x} ok')
PY

ls -l "$OUT"
