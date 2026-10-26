# Build the BBK A2 / X2 / X2P boot image from a real UEFI firmware volume.
#
#   sh tools/build-bootimg.sh [FD] [OUT]
#
# Defaults:
#   FD  = uefi/build/SILICIUM_UEFI.fd   (produced by uefi/build-bbk-a2.sh)
#   OUT = dist/boot.img
#
# Environment:
#   UNIXNU_TREE  UniXNU core checkout (default: ../UniXNU next to this repo,
#                or $GITHUB_WORKSPACE/UniXNU in CI)
#
# The image uses the stock A2 layout: raw BootShim+FD in the kernel field and
# the device tree in the Android header v2 dtb_size/dtb_addr fields.  This is
# what the Unisoc cboot loads; do NOT use the gzip+appended-dtb layout of
# `./unixnu bootimg` on this device.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FD=${1:-$ROOT/uefi/build/SILICIUM_UEFI.fd}
OUT=${2:-$ROOT/dist/boot.img}
UNIXNU_TREE=${UNIXNU_TREE:-$ROOT/../UniXNU}
DTB=$ROOT/build/bbk-a2-device-tree.dtb

if [ ! -f "$FD" ]; then
    echo "missing firmware volume: $FD" >&2
    echo "build it first: sh uefi/build-bbk-a2.sh DEBUG" >&2
    exit 1
fi
if [ ! -d "$UNIXNU_TREE/scripts/unixnu" ]; then
    echo "missing UniXNU core checkout: $UNIXNU_TREE" >&2
    echo "set UNIXNU_TREE=/path/to/UniXNU" >&2
    exit 1
fi

mkdir -p "$ROOT/build" "$(dirname "$OUT")"

echo "== compile device tree"
dtc -I dts -O dtb \
    -i "$ROOT/soc/UniXNU-ums512-common/dts" \
    -o "$DTB" "$ROOT/dts/bbk-a2.dts"

echo "== pack boot image"
python3 "$ROOT/tools/pack-device-bootimg.py" \
    --unixnu "$UNIXNU_TREE" \
    --device "$ROOT" \
    --uefi "$ROOT/uefi" \
    --fd "$FD" \
    --dtb "$DTB" \
    -o "$OUT"

echo "== verify boot image"
python3 "$ROOT/tools/verify-bootimg.py" "$OUT"

sha256sum "$OUT" | tee "$OUT.sha256"
