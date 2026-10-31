/*
#
  BBK/EEBBK A2 / X2 / X2P (Unisoc UMS512, ums512_1h190) memory map.

  Every address below comes from one of three places:

    [DT]  the vendor boot device tree of this device
          (analysis/extracted/boot/boot-appended.dtb -> _scratch/boot-dtb.dts)
    [CB]  the cboot / U-Boot 2015.07 device tree and disassembly
          (analysis/extracted/uboot/uboot-base.dtb, uboot-payload.bin)
    [E5]  the working UMS9621 port (_repos/e5-xnu) for values that are shared
          between the two SoCs.  Marked TODO where UMS512 has not been measured.

  Layout rationale (cboot hands control to us at 0x80080000 with MMU off):

    0x80080000  cboot drops the boot.img kernel field here; BootShim copies the
                4 MiB firmware volume that follows it up to FD_BASE.
    0x80480000  primary core stack (0x40000, room for 8 cores).
    0x80680000  SEC heap.
    0x80780000  MMU page tables.
    0x80800000  UEFI FD, 4 MiB, this is where SEC really runs.
    0x80C00000  DXE heap, 20 MiB, ending at the FDT that cboot passes in x0.
    0x82000000  merged FDT (cboot passes x0 = 0x82000000; [CB] start_linux).
    0x82100000  Initrd window: the boot.img ramdisk load address is not
                measured on UMS512 yet.  The window covers both the low
                candidate (just above the FDT) and the header-derived
                candidate (0x80000000 + ramdisk_addr 0x05400000 = 0x85400000)
                and stops at the vendor audio-mem reservation at 0x87500000.
                The real bounds always come from /chosen at run time.
    0x94440000  free DRAM after the vendor tos-mem reservation; reserved as the
                "High RAM" window donor DarwinBoot needs (contiguous, 2 MiB
                aligned, ends at cboot's secboot-verify buffer at 0x9A000000).
    0x9A000000  cboot secboot-verify buffer (U-Boot DT), 48 MiB - not mapped.
    0x9D000000  cboot bmp-mem / 0x9E000000 logobuffer (the panel framebuffer).
    0xFFF80000  ramoops (persistent log + watchdog dump).

  Deliberately unmapped:
    * 0x9A000000..0x9E9E4000 - cboot scratch / BMP / framebuffer sources; the
      framebuffer itself is mapped through the "Display Reserved" entry only.
    * the AON watchdog block 0x322F0000 - TODO(hardware): its clock gate bits
      are not verified; leaving it unmapped turns an accidental access into a
      translation fault instead of a bus hang.
#

*/

#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // [CB] the blob cboot copied into DRAM: our shim plus the firmware volume it moves up.
  {"Boot Shim",          0x80080000, 0x00400000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},

  // DDR: firmware owned
  {"UEFI Stack",         0x80480000, 0x00200000, AddMem,  SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"SEC Heap",           0x80680000, 0x00100000, AddMem,  SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"MMU PageTables",     0x80780000, 0x00040000, AddMem,  SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI FD",            0x80800000, 0x00400000, AddMem,  SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  // DXE heap: 0x80C00000..0x81FF0000.  It has to stop at the FDT cboot passes
  // in x0 ([CB] start_linux x0 = 0x82000000), at the Initrd window below and
  // at the vendor RebootEscrow reservation at 0x81FF0000 (0x10000 long) which
  // sits just under the FDT; the Silicium MemoryPeim rejects overlapping
  // regions with a CpuDeadLoop, and the first hardware boot died exactly on
  // "DXE Heap" vs "RebootEscrow" (0x81FF0000 inside 0x80C00000..0x82000000).
  // 19.94 MiB is enough for SEC/DXE + Shell + FAT + the payload block device;
  // it is NOT enough for a large DarwinBoot kernel window, which is what the
  // "High RAM" donor further down is for.
  {"DXE Heap",           0x80C00000, 0x013D0000, AddMem,  SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"VarStore",           0x81FD0000, 0x00020000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, WRITE_BACK},

  // [CB] merged vendor FDT handed over in x0.  Size: the stock boot.img dtb is
  // 157748 bytes and the dtbo overlays add a few KiB, so 1 MiB is generous.
  {"FDT",                0x82000000, 0x00100000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, WRITE_BACK_XN},
  // Ramdisk window.  TODO(hardware): confirm the actual cboot ramdisk load
  // address; the MMU must cover whatever /chosen reports, and this window is
  // the union of the two candidates (see header comment).  It ends at the
  // vendor audio-mem reservation.
  {"Initrd",             0x82100000, 0x05400000, AddMem,  MEM_RES, UNCACHED_UNBUFFERED_XN, Reserv, UNCACHED_UNBUFFERED_XN},

  // DDR: reserved by the vendor firmware / secure world / modem  [DT]
  {"DDRBIST",            0x80000000, 0x00001000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"RebootEscrow",       0x81FF0000, 0x00010000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"Audio Mem",          0x87500000, 0x00300000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"SIPC",               0x87800000, 0x00800000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"Sensorhub",          0x88000000, 0x01000000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"AudioDSP",           0x89000000, 0x00600000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"CP Memory",          0x89600000, 0x04900000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"IQ Memory",          0x90000000, 0x04000000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"SML",                0x94000000, 0x00040000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"TOS",                0x94040000, 0x00400000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},

  // [CB] free DRAM between tos-mem and cboot's secboot-verify buffer.  Reserved
  // as a DarwinBoot window donor: contiguous, 2 MiB aligned, 92 MiB.  TODO:
  // split into a DXE heap expansion + a Conv donor once DarwinBoot is wired.
  {"High RAM",           0x94440000, 0x05BC0000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},

  // [CB] cboot scratch after handoff.  Not mapped on purpose; listed here for
  // documentation only - keep it out of the descriptor table.
  // {"Secboot Verify",  0x9A000000, 0x03000000, ...},
  // {"BMP Buffer",      0x9D000000, 0x009E4000, ...},

  // [DT] display / boot logo buffer, still scanned out by the DPU after
  // handoff.  This is the framebuffer the GOP driver publishes.
  //
  // DRAM holes that the vendor map leaves unclaimed.  DarwinBoot needs ~50 MiB
  // for kernel.macho plus room for the boot args and XNU itself, which the
  // 19 MiB DXE heap cannot provide (AllocatePoolPages: failed to allocate
  // 12692 pages).  These are handed to the UEFI allocator as Conventional.
  //
  //
  // Everything above the vendor carve-outs is plain DRAM: U-Boot reports 1 GiB
  // starting at 0x80000000, so 0xA0000000..0xC0000000 is real memory that the
  // vendor map simply does not mention.  This is where DarwinBoot can get its
  // multi-megabyte contiguous window from.
  //
  {"Darwin E",           0xA0000000, 0x20000000, AddMem,  SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

  {"Display Reserved",   0x9E000000, 0x009E4000, AddMem,  MEM_RES, SYS_MEM_CAP,   Reserv, WRITE_THROUGH_XN},

  // [DT] ramoops: persistent log the firmware writes and Android reads back
  // through /sys/fs/pstore.
  {"Ramoops",            0xFFF80000, 0x00040000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},

  // On-chip peripherals  [DT] unless marked
  {"GICD",               0x12000000, 0x00020000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"GICR",               0x12040000, 0x00100000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"AP AHB",             0x20100000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"DPU/GSP",            0x20300000, 0x00002000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"DSI",                0x20400000, 0x00001000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  // UART0/1/2/3 live at 0x70000000 / 0x70100000 / 0x70200000 / 0x70300000
  // (stride 0x10000).  The region must span all four: SEC/DXE write the
  // debug console on UART1 (0x70100000), and the old 0x40000 length only
  // covered UART0, which caused a level-3 translation fault at 0x70100008
  // the moment the MMU came on.
  {"UART0-3",            0x70000000, 0x00400000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"eMMC SDHCI",         0x71400000, 0x00001000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"AP GPIO",            0x32070000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"EIC",                0x32000000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"PMIC SPI",           0x32100000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"AON APB",            0x327D0000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"PMU APB",            0x327E0000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount
  )
{
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
