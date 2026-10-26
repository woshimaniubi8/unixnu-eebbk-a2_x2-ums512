##
#  EDK2 platform description for BBK/EEBBK A2 / X2 / X2P
#  (Unisoc UMS512, ums512_1h190, SharkL5Pro, T610 family).
#
#  The device has no UEFI firmware: cboot (U-Boot 2015.07) loads the boot.img
#  kernel field to 0x80080000 and jumps to it with x0 = physical address of the
#  merged device tree.  BootShim (../BootShim) turns that into a normal EDK2
#  entry: it copies the firmware volume to PcdFdBaseAddress (0x80800000) and
#  branches to SEC, keeping x0 intact so that SEC stashes it in the
#  gSiliciumEntryArgHobGuid HOB for DtPlatformDxe and BbkA2PayloadDxe.
#
#  The reusable EDK2/Silicium layer (SEC, DXE core, BDS, DtPlatformDxe,
#  ArmGicDxe, TimerDxe, CpuDxe, RamManagerDxe, SMBIOS, ACPI infrastructure) is
#  taken unchanged from UniXNU/uefi-sheng's Silicon/Silicium; only the SoC and
#  board layers below are specific to this port.  Layout follows the working
#  UMS9621 port (_repos/e5-xnu/uefi/Platforms/Unisoc/e5Pkg).
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = bbkA2
  PLATFORM_GUID                  = 4D2A7C31-8E5F-4B96-A1C3-7F0D2E9B6A54
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/bbkA2Pkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = bbkA2Pkg/bbkA2.fdf
  #
  # The UMS512 DPU/panel path is not confirmed (format/stride unknown), so the
  # platform supplies its own GOP driver over the "Display Reserved" region.
  # The 1200x1920 panel would also satisfy SimpleFbDxe; flip this to 0 to use
  # it once the framebuffer format is known.
  #
  USE_CUSTOM_DISPLAY_DRIVER      = 1

!include UMS512Pkg/UMS512Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR: 2 GiB at 0x80000000 (vendor DT memory node).
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI stack: inside the "UEFI Stack" region of MemoryMapLib.
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x80480000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x00040000

  #
  # Debug output level.  0x8007EECF = ERROR(0x80000000) | INFO(0x40) |
  # DISPATCH(0x80) | the inherited 0x0007EE0F bits.  INFO makes library traces
  # visible; DISPATCH makes DXE Core print "Loading driver at ..." so a crash
  # names the driver.  Inherited from the UMS9621 port, where these values were
  # established empirically.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8007EECF

  #
  # Debug property mask 0x23 = ASSERT_ENABLE(0x01) | PRINT_ERROR(0x02) |
  # ASSERT_DEADLOOP(0x20).  Deliberately clear:
  #   bit2 DEBUG_CODE_ENABLE(0x04): the CpuExceptionHandlerLib stack walker
  #        double-faults on a poisoned pool and loses the register dump;
  #   bit3 CLEAR_MEMORY_ENABLED(0x08): CoreFreePages poisons freed pages with
  #        0xAF, and 0xAF & TT_TYPE_MASK == 3 == TT_TYPE_TABLE_ENTRY makes the
  #        MMU walker treat a freed page-table page as a live table (the UMS9621
  #        port lost a day to exactly this).
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x23

  #
  # BDS looks this GUID up in the FV.  The value inherited from
  # SiliciumPkg.dsc.inc points at a file this platform never puts in its FV,
  # so point it at the UEFI Shell's FILE_GUID instead.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{ 0x83, 0xA5, 0x04, 0x7C, 0x3E, 0x9E, 0x1C, 0x4F, 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 }

  #
  # BDS copies this into gST->FirmwareVendor.  Pin it so the VOID* token
  # resolves inside this platform's own FV.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"BBK"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareRevision|0x00010000

  #
  # Bring-up safety net: with TRUE, BbkA2BringupDxe resets the machine at the
  # end of DXE dispatch (PSCI SYSTEM_RESET via ResetSystemRuntimeDxe), so every
  # experiment ends with the tablet back in Android and the ramoops log
  # readable instead of parked in BDS waiting for a key that does not exist.
  # Set to FALSE once a boot target (Shell/UniXNU) is confirmed to come up.
  #
  gUms512PkgTokenSpaceGuid.PcdBbkA2ResetAtEndOfDxe|TRUE

  #
  # SMBIOS.
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"BBK"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"A2"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"bbk-a2"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"ums512_1h190"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"ums512_1h190"

  #
  # Panel: 1200x1920 (U-Boot DTS lcd_himax8279_boe_mipi_fhd_bbk_H190).
  # TODO(hardware): colour depth.  32 bpp XRGB8888 is the assumption (Android
  # display pipeline + logobuffer 0x9e4000 is big enough for 1200*1920*4);
  # if the panel shows swapped colours, try 16 (RGB565) - the UMS9621 DPU used
  # format 4 = RGB565.
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1200
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|1920
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

[LibraryClasses]
  #
  # Board memory map: vendor reserved-memory + cboot-owned regions + the
  # peripherals EDK2 needs.  Also the source of "Display Reserved", which both
  # SEC (to clear the panel) and SprdGopDxe (to publish GOP) look up by name.
  #
  MemoryMapLib|bbkA2Pkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Platform SEC hooks: paint stage markers on the panel and write a banner on
  # UART1 before the serial library is up.  Overrides the SiliciumNull default.
  #
  PlatformSecLib|bbkA2Pkg/Library/BbkA2PlatformSecLib/BbkA2PlatformSecLib.inf

  #
  # UEFI Shell libraries.  These three are the only ShellPkg libraries the
  # include chain does not already name; the rest is inherited from
  # SiliciumPkg.dsc.inc and must not be repeated here.
  #
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf

[Components]
  #
  # Display: our own GOP over the cboot logobuffer.
  #
  UMS512Pkg/Drivers/SprdGopDxe/SprdGopDxe.inf

  #
  # FV as a read-only file system, so the Shell can auto-run startup.nsh.
  #
  MdeModulePkg/Universal/FvSimpleFileSystemDxe/FvSimpleFileSystemDxe.inf

  #
  # Bring-up probe: dumps the DXE view of the world at the end of dispatch and
  # resets, so every experiment ends with a readable ramoops log.
  #
  bbkA2Pkg/Drivers/BbkA2BringupDxe/BbkA2BringupDxe.inf

  #
  # Payload volume: reads /chosen linux,initrd-start/-end from the FDT that SEC
  # saved in the entry-argument HOB and publishes the ramdisk as a read-only
  # block device (512-byte blocks).  In [Components] so it keeps compiling;
  # added to the FDF only after DXE is confirmed on hardware (a fault here
  # would hide the EndOfDxe marker).
  #
  bbkA2Pkg/Drivers/BbkA2PayloadDxe/BbkA2PayloadDxe.inf

  #
  # UEFI Shell: the boot target BDS can find by FILE_GUID.  Only the Level1/2/3
  # command profiles are linked; Driver1/Install1/Debug1/Network1 would each
  # add another NULL library and drag in unused stacks.
  #
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
  }

  #
  # TODO(integration): the UniXNU boot application and DarwinBoot are NOT in
  # this skeleton because they live in UniXNU's package/ and drivers/DarwinBoot
  # checkouts, which are assembled by `./unixnu build firmware` through
  # PACKAGES_PATH.  Add them here once the UniXNU tree is in place:
  #
  #   UniXNUPkg/Application/UniXNU/UniXNU.inf
  #   UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  #   UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  #   DarwinBootPkg/Driver/DarwinBoot.inf
  #
  # and mirror them into bbkA2.fdf.  See analysis/uefi/FEASIBILITY.md §6.
