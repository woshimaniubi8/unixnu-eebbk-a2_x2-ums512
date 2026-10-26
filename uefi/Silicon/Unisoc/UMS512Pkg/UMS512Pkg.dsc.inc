##
#  Unisoc UMS512 (SharkL5Pro) SoC package include for EDK2 platform DSCs.
#
#  Everything here is SoC-wide and board independent.  Board values (memory
#  map, panel size, SMBIOS strings, reset policy) are set by the platform DSC
#  after this include so they override anything below.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  #
  # The arch timer runs on the non-secure physical counter on this SoC.
  #
  USE_PHYSICAL_TIMER                         = 1

!include SiliciumPkg/SiliciumPkg.dsc.inc

[BuildOptions]
  #
  # SiliciumPkg only suppresses these warnings for CLANGPDB; this port builds
  # with CLANGDWARF (ELF + GenFw) so the same suppressions are repeated here.
  # -fno-stack-protector: EDK2 does not provide __stack_chk_guard/_fail.
  #
  *_CLANGDWARF_*_CC_FLAGS = -Wno-unused-const-variable -Wno-sometimes-uninitialized -Wno-deprecated-non-prototype -Wno-unaligned-access -fno-stack-protector
  *_CLANGDWARF_*_CC_FLAGS = -D CLANG -D ENABLE_SECUREBOOT=0

[PcdsFixedAtBuild]
  #
  # CPU: 6x Cortex-A55 + 2x Cortex-A75, 2 clusters, brought up through PSCI.
  # Vendor DT (boot-dtb.dts /cpus): cpu@0..@500 = cortex-a55, cpu@600/@700 =
  # cortex-a75; enable-method = "psci".
  #
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|2

  #
  # GICv3: GICD@0x12000000 (0x20000), GICR@0x12040000 (0x100000, stride
  # 0x20000, 8 frames).  Vendor DT values, see Include/Ums512.h.
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x12000000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x12040000

  #
  # Arch timer PPIs.  Vendor DT says <1 13 4>, <1 14 4>, <1 11 4>, <1 10 4>;
  # ArmPkg defaults to 29/30/27/26 and would arm a PPI that never fires, which
  # stalls DXE on the first timer event with no output at all.
  #
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|13
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|14
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|11
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|10

  #
  # SMBIOS strings.  Processor values are SoC facts; system/board strings are
  # overridden by the platform DSC.
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Unisoc UMS512 (SharkL5Pro)"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"UMS512"

[LibraryClasses]
  #
  # Debug output: UART1 at 0x70100000 (polled, baud preserved) plus the
  # ramoops console/dmesg persistent-RAM zones at 0xfff80000.  See the library
  # header for why the UART sink is write-only and probed once.
  #
  SerialPortLib|UMS512Pkg/Library/SprdDebugSerialPortLib/SprdDebugSerialPortLib.inf
