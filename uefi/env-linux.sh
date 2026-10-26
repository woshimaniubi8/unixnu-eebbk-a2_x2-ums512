#!/bin/sh
#
# EDK2 build environment for the BBK A2 / X2 / X2P (Unisoc UMS512) firmware on
# Linux (WSL Ubuntu 22.04 is the reference environment).
#
# Nothing is installed into the system: iasl/nasm/libuuid can be unpacked under
# $BBK_A2_DEPS/local and put on PATH/CPATH/LIBRARY_PATH instead.  See
# analysis/uefi/FEASIBILITY.md for the dependency checkouts.
#
# Use it either directly
#   BBK_A2_ENV=$PWD/uefi-bbk-a2/env-linux.sh sh uefi-bbk-a2/build-bbk-a2.sh DEBUG
# or as the well-known copy build-bbk-a2.sh falls back to:
#   cp uefi-bbk-a2/env-linux.sh ~/bbk-a2-uefi/env.sh
#
# $BBK_A2_DEPS  where the dependency repos and the extracted RPMs live
# $BBK_A2_REPO  this repository (build-bbk-a2.sh sets it to its own root)

BBK_A2_DEPS=${BBK_A2_DEPS:-$HOME/bbk-a2-uefi}
BBK_A2_REPO=${BBK_A2_REPO:-$HOME/Workspace/uefi-bbk-a2}

# EDK2 workspace: holds BaseTools (a symlink to mu_basecore is enough), Conf/
# and Build/.
export WORKSPACE=${WORKSPACE:-$BBK_A2_DEPS/work}
# BaseTools lives in the mu_basecore checkout.  An explicit EDK_TOOLS_PATH is
# honoured so builds also work where WORKSPACE/BaseTools cannot be a symlink
# (drvfs mounts, or a dependency tree cloned outside the workspace).
export EDK_TOOLS_PATH=${EDK_TOOLS_PATH:-$WORKSPACE/BaseTools}
if [ ! -d "$EDK_TOOLS_PATH" ] && [ -d "$BBK_A2_DEPS/probe/BaseTools" ]; then
  export EDK_TOOLS_PATH=$BBK_A2_DEPS/probe/BaseTools
fi
export EDK_TOOLS_BIN=$EDK_TOOLS_PATH/Source/C/bin

# EDK2 needs Conf/ next to WORKSPACE.  Seed it from the templates so sourcing
# this file is the only setup step; never overwrite local tweaks.
if [ -d "$EDK_TOOLS_PATH/Conf" ]; then
  mkdir -p "$WORKSPACE/Conf"
  for t in target tools_def build_rule; do
    [ -f "$WORKSPACE/Conf/$t.txt" ] || cp "$EDK_TOOLS_PATH/Conf/$t.template" "$WORKSPACE/Conf/$t.txt"
  done
fi

# Every package root the platform DSC refers to.  A missing entry makes
# "build" print "- Failed -" with no error message at all, so the list has to
# be complete:
#   mu_basecore      MdePkg/MdeModulePkg/ArmPkg/EmbeddedPkg/ShellPkg/FatPkg
#   mu_plus          MsCorePkg/MsGraphicsPkg/PcBdsPkg
#   mu_oem_sample    (referenced by SiliciumPkg.dsc.inc)
#   uefi-sheng       Silicon/Silicium (SEC, RamManagerDxe, SMBIOS, GOP helper)
#   this repository  Platforms/Unisoc and Silicon/Unisoc
export PACKAGES_PATH=$WORKSPACE\
:$BBK_A2_DEPS/mu_basecore\
:$BBK_A2_DEPS/mu_plus\
:$BBK_A2_DEPS/mu_oem_sample\
:$BBK_A2_DEPS/uefi-sheng/Silicon/Silicium\
:$BBK_A2_REPO/Platforms/Unisoc\
:$BBK_A2_REPO/Silicon/Unisoc

# clang/llvm for CLANGDWARF.  On Ubuntu clang, llvm-ar, llvm-objcopy, llvm-rc
# and ld.lld all live in /usr/bin; the builder symlinks versioned names into
# build/toolchain-bin if needed.
export CLANGDWARF_BIN=${CLANGDWARF_BIN:-/usr/bin/}
export CLANG_BIN=${CLANG_BIN:-/usr/bin/}

# iasl/nasm from unpacked packages, the BaseTools Python wrappers and the C
# tools built from mu_basecore.
export PATH=$BBK_A2_DEPS/local/usr/bin:$EDK_TOOLS_BIN:$EDK_TOOLS_PATH/BinWrappers/PosixLike:$PATH

# BaseTools links against libuuid.
export CPATH=$BBK_A2_DEPS/local/usr/include${CPATH:+:$CPATH}
export LIBRARY_PATH=$BBK_A2_DEPS/local/usr/lib64${LIBRARY_PATH:+:$LIBRARY_PATH}
export EXTRA_LDFLAGS="-L$BBK_A2_DEPS/local/usr/lib64${EXTRA_LDFLAGS:+ $EXTRA_LDFLAGS}"

# The BinWrappers exec "python" unless PYTHON_COMMAND is set.
export PYTHON_COMMAND=${PYTHON_COMMAND:-python3}

export BBK_A2_FD_BASE=${BBK_A2_FD_BASE:-0x80800000}
export BBK_A2_FD_SIZE=${BBK_A2_FD_SIZE:-0x400000}
export BBK_A2_FD_BLOCKS=${BBK_A2_FD_BLOCKS:-0x400}
