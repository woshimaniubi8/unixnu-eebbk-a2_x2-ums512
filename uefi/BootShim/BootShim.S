//
//  BootShim for BBK/EEBBK A2 / X2 / X2P (Unisoc UMS512, ums512_1h190, SharkL5Pro).
//
//  Chain:  BootROM -> splloader -> cboot (U-Boot 2015.07) -> boot.img kernel field
//          -> this shim at 0x80080000 -> SILICIUM_UEFI.fd at FD_BASE -> SEC.
//
//  Contract with cboot (same handoff Project-Silicium relies on for Qualcomm ABL):
//
//    * cboot loads the boot.img "kernel" field to a fixed DRAM address.  The
//      Unisoc cboot address is 0x80080000; the stock boot.bin header says
//      kernel_addr = 0x8000 (offset from base 0), and the Bootshim-sprd
//      reference documents 0x80080000 for Unisoc cboot.  TO VERIFY ON HARDWARE.
//    * x0 = physical address of the merged (boot dtb + dtbo) device tree.
//      cboot hands over the FDT at 0x82000000 on this SoC family (measured in
//      the cboot image for ums512_1h190: start_linux is entered with
//      x0 = 0x82000000).  This shim does not touch x0: it is saved in x19 and
//      restored right before the jump, so Silicium SEC records it in the
//      gSiliciumEntryArgHobGuid HOB and DtPlatformDxe / the payload driver
//      can read /chosen from it.
//    * MMU and caches: cboot is expected to hand over with them off.  We still
//      run cache maintenance (harmless when off) because LK on the UMS9621
//      reference handed over with caches ON and that cost the E5 port a day.
//
//  The shim relocates the firmware volume:
//
//    0x80080000  _Head / _Start / this code
//    0x8008xxxx  _Payload = SILICIUM_UEFI.fd appended by the boot-image packer
//         |
//         +-- copy FD_SIZE bytes to FD_BASE (0x80800000), then br there
//
//  FD_BASE = 0x80800000 keeps the destination outside the source window
//  (0x80080000 + 4 MiB = 0x80480000) because the copy below is a forward
//  16-byte ldp/stp loop and would otherwise overwrite source bytes it has not
//  read yet.  Same reasoning as the E5 port (_repos/e5-xnu/docs/uefi-bringup.md).
//
//  The first 64 bytes double as a valid AArch64 Image header:
//
//    +0  b _Start                      code0
//    +4  (part of the same branch)     code1
//    +8  FD_BASE                       text_offset
//    +16 FD_SIZE + (_Payload - _Head)  image_size (patched at link time, not
//                                      at pack time: the assembler knows both
//                                      the FD size macro and the shim length)
//    +24 flags, +32..+48 reserved
//    +56 "ARM\x64"                     magic (cboot checks this)
//
//  UniXNU's scripts/unixnu/bootimg.py:compile_shim() enforces all of it:
//    * llvm-nm must report _Payload at exactly the end of the binary image,
//    * shim[56:60] == "ARM\x64",
//    * unpack("<QQ", shim, 8) == (FD_BASE, len(shim) + FD_SIZE).
//
//  Build (as done by `./unixnu bootimg` and build-bbk-a2.sh):
//
//    clang --target=aarch64-none-elf -c \
//      -DFD_BASE=0x80800000 -DFD_SIZE=0x400000 -DREQUIRES_KERNEL_HEADER=1 \
//      BootShim.S -o BootShim.o
//    llvm-objcopy -O binary BootShim.o BootShim.bin
//
//  Optional compile-time switches (all OFF by default, see the TODO blocks):
//
//    -DBBK_KEEP_EL2        do not drop from EL2 to EL1 before entering SEC
//    -DBBK_STAGE_COLORS    paint the panel at 0x9e000000 at each stage
//    -DBBK_ARM_WATCHDOG    program the AP watchdog as a dead-man switch
//
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//

//
//  Where cboot drops the boot.img kernel field.  Must match FD_BASE + FD_SIZE
//  window and the "text_offset" the packer writes (see above).
//
.set SHIM_BASE, 0x80080000

//
//  Optional panel stage markers (BBK_STAGE_COLORS).
//
//  cboot leaves the DPU scanning the "logobuffer" reserved region at
//  0x9e000000 (U-Boot device tree: logobuffer@9e000000, size 0x9e4000) with
//  the panel already initialised, so a bare-metal write there is visible.
//  The U-Boot DTS panel node for this board is lcd_himax8279_boe_mipi_fhd_bbk_H190:
//  1200x1920, 4 DSI lanes, RGB888.
//
//  TODO(hardware): the DPU layer format (16 bpp RGB565 vs 32 bpp XRGB8888) and
//  the stride are NOT confirmed.  0x9e4000 bytes fits both 1200*1920*4
//  (0x8CA000) and 1200*1920*2 (0x465000).  Default below is 32 bpp with a
//  1200-pixel stride; if the panel shows garbage, rebuild with
//  -DBBK_FB_BPP=16 (and note 16 bpp needs 2-byte stores, the macro selects
//  them).  Garbage here is harmless: it only paints over the boot logo.
//
#ifdef BBK_STAGE_COLORS
#  ifndef BBK_FB_BASE
#    define BBK_FB_BASE 0x9e000000
#  endif
#  ifndef BBK_FB_WIDTH
#    define BBK_FB_WIDTH 1200
#  endif
#  ifndef BBK_FB_HEIGHT
#    define BBK_FB_HEIGHT 1920
#  endif
#  ifndef BBK_FB_BPP
#    define BBK_FB_BPP 32
#  endif
#  define BBK_FB_PIXELS (BBK_FB_WIDTH * BBK_FB_HEIGHT)

.macro FBPAINT colour
  ldr   x0, =BBK_FB_BASE
#  if BBK_FB_BPP == 32
  mov   w1, #(\colour)
  ldr   x2, =BBK_FB_PIXELS
991:
  str   w1, [x0], #4
  subs  x2, x2, #1
  b.ne  991b
#  else
  mov   w1, #(\colour)
  ldr   x2, =BBK_FB_PIXELS
991:
  strh  w1, [x0], #2
  subs  x2, x2, #1
  b.ne  991b
#  endif
  dsb   sy
.endm
#endif

//
//  AP watchdog dead-man switch (BBK_ARM_WATCHDOG).
//
//  Why it matters: a wedged loop never takes an exception, so cboot's panic
//  path never runs and the tablet sits there until the battery is pulled.  The
//  E5 port learned this the hard way (_repos/e5-xnu/docs/uefi-bringup.md §6).
//
//  TODO(hardware) - two independent unknowns, which is why this block is OFF
//  by default:
//
//   1. The AP watchdog block is 0x322f0000 on UMS512 (vendor DTS:
//      watchdog@322f0000, compatible "sprd,wdt-r2p0-fiq", clocks
//      <enable, rtc_enable>).  On UMS9621 it was 0x641e0000.
//   2. Its APB clocks are gated by the AON gate controller at 0x327d0000
//      (vendor DTS: aonapb-gate, compatible "sprd,sharkl5pro-aon-gate",
//      syscon = <&aon_apb_regs> = 0x327d0000).  The E5 values were
//      reg 0x08 bit 29 (ap-wdg-eb) and reg 0x18 bit 7 (ap-wdg-rtc-eb), with
//      "write the mask to base + reg + 0x1000" to enable.  The sharkl5pro
//      clock table has NOT been read for this SoC: if the bit indices are
//      wrong the write either does nothing (best case) or touches a gated
//      register, which on this family wedges the bus instead of faulting
//      (E5 measured this).  So: do not arm this until the gate bits are
//      confirmed from the vendor kernel clk/sprd_wdt source, or until cboot's
//      own watchdog is proven to reset the board.
//
//  Note: stock cboot passes androidboot.wdten=%x to the kernel, so a watchdog
//  may already be armed by cboot itself; that is the free safety net to test
//  first (flash a shim that parks, measure the reset time).
//
//
//  AArch64 Image header.  Everything up to _Start is data the packer and
//  cboot inspect; the branch in code0 skips it.
//
.balign 16
.global _Head
_Head:
  adr   x1, _Payload             // code0: x1 = appended firmware volume
  b     _Start                   // code1 (upper half of the same 8 bytes)

_StackBase:
  .quad FD_BASE                  // text_offset: where the payload wants to run
_StackSize:
  .quad FD_SIZE + (_Payload - _Head)   // image_size: shim + firmware volume

  .quad 0                        // flags
  .quad 0                        // reserved
  .quad 0                        // reserved
  .quad 0                        // reserved

  .ascii "ARM\x64"               // magic at offset 56, cboot checks it
  .long 0                        // reserved

  // Free-form identification, ignored by every loader.  Kept short so the
  // header stays inside the first 64-byte cache line pair cboot may inspect.
  .asciz "BBK-A2-UMS512"
  .balign 16

_Start:
  // Mask interrupts first: cboot may hand over with them live and its own
  // vectors still installed.
  msr   daifset, #0xf

  // x0 is the FDT handed over by cboot.  The copy loop below uses x1..x12, so
  // park it in a callee-saved register until the final jump.
  mov   x19, x0

#ifdef BBK_STAGE_COLORS
  FBPAINT 0x00001f                // blue: the shim was entered
#endif

#ifdef BBK_ARM_WATCHDOG
  // Enable the two AP watchdog APB clocks through the AON gate controller.
  // TODO(hardware): verify reg/bit pairs against the sharkl5pro clock driver.
  ldr   x0, =0x327d1008
  mov   w1, #0x20000000          // ap-wdg-eb,      reg 0x08 bit 29
  str   w1, [x0]
  ldr   x0, =0x327d1018
  mov   w1, #0x80                // ap-wdg-rtc-eb,  reg 0x18 bit 7
  str   w1, [x0]
  dsb   sy

  // sprd,wdt-r2p0-fiq register map (vendor DTS + E5 port):
  //   0x00 LOAD_LOW  0x04 LOAD_HIGH  0x08 CTRL  0x0c INT_CLR
  //   0x10 INT_RAW   0x20 LOCK (0xe551 unlocks)
  // One tick is 1/32768 s; 120 s = 0x3c0000 ticks -> LOAD_HIGH 0x3c.
  // 120 s and not 300 s: a blind first boot should come back to Android fast.
  ldr   x0, =0x322f0000
  ldr   w1, =0xe551
  str   w1, [x0, #0x20]          // unlock
  mov   w1, #0x9
  str   w1, [x0, #0x0c]          // INT_CLR

  // E5 bug to avoid: LOAD writes are silently dropped while LD_BUSY is set.
  // Poll INT_RAW bit 4 before writing LOAD_LOW/LOAD_HIGH.
  mov   w2, #1000
2:
  ldr   w1, [x0, #0x10]          // INT_RAW
  tbz   w1, #4, 3f               // LD_BUSY clear -> go
  subs  w2, w2, #1
  b.ne  2b
3:
  mov   w1, #0x3c
  str   w1, [x0, #0x04]          // LOAD_HIGH = 0x3c  (120 s)
  str   wzr, [x0, #0x00]         // LOAD_LOW  = 0
  mov   w1, #0xf                 // NEW_VER_EN | INT_EN | CNT_EN | RST_EN
  str   w1, [x0, #0x08]
  str   wzr, [x0, #0x20]         // lock
  dsb   sy
#endif

  // x4 = source (_Payload), x5 = FD_BASE, x6 = FD size.
  adr   x4, _Payload
  ldr   x5, _StackBase
  cmp   x4, x5
  beq   _Entry

  // image_size - shim length = FD_SIZE (see header above).  Deriving it here
  // instead of using the FD_SIZE macro directly keeps the header and the copy
  // length in lock-step: the packer only ever sees the header.
  ldr   x6, _StackSize
  adr   x7, _Payload
  adr   x8, _Head
  sub   x7, x7, x8
  sub   x6, x6, x7

_CopyLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _CopyLoop

#ifdef BBK_STAGE_COLORS
  FBPAINT 0x00ff00                // green: firmware volume copied to FD_BASE
#endif

_Entry:
  // Publish the copied image to the point of coherency.  With caches off this
  // is a no-op; with caches on (LK-style handoff) it is the difference between
  // executing stale bytes and a working SEC.
  mrs   x9, ctr_el0
  ubfx  x9, x9, #16, #4           // DminLine, log2 of words
  mov   x10, #4
  lsl   x9, x10, x9               // cache line size in bytes
  sub   x10, x9, #1
  ldr   x5, _StackBase
  ldr   x6, _StackSize
  adr   x7, _Payload
  adr   x8, _Head
  sub   x7, x7, x8
  sub   x6, x6, x7                // x6 = FD_SIZE again
  bic   x11, x5, x10
  add   x12, x5, x6
_CleanDst:
  dc    civac, x11
  add   x11, x11, x9
  cmp   x11, x12
  b.lo  _CleanDst
  dsb   sy
  ic    iallu
  dsb   sy
  isb

#ifndef BBK_KEEP_EL2
  // Drop EL2 -> EL1 before entering SEC.
  //
  // DarwinBoot's DarwinEnter() hands the kernel EL1 and refuses anything
  // above it, so the whole firmware must run at EL1 (E5 port, docs §6).  If
  // cboot already hands over at EL1 the check below skips the eret.  MMU and
  // caches are off here, so this is the cheapest safe point for the drop.
  //
  // HCR_EL2.RW (bit 31) MUST stay 1: it selects AArch64 for EL1.  Clearing it
  // makes the eret an illegal PSTATE return (EC=0x0e, measured on E5).
  mrs   x10, currentel
  and   w10, w10, #0xc
  cmp   w10, #0x8
  b.ne  _EnterSec

  ldr   x10, =(0x1 << 31)
  msr   hcr_el2, x10
  msr   cptr_el2, xzr
  msr   mdscr_el1, xzr
  tlbi  vmalle1
  dsb   sy
  isb
  mov   x10, #0x3c5               // SPSR: EL1h, DAIF masked
  msr   spsr_el2, x10
  adr   x10, _EnterSec
  msr   elr_el2, x10
  eret
#endif

_EnterSec:
#ifdef BBK_STAGE_COLORS
  FBPAINT 0xffffff                // white: entering SEC
#endif

  // Restore the FDT pointer and enter SEC.  Silicium's ModuleEntryPoint.S
  // stores x0 in gSecEntryArg0 and Sec.c turns it into the
  // gSiliciumEntryArgHobGuid HOB (DAEE82E6-8090-4E72-BFB9-78C13CD4E359).
  mov   x0, x19
  ldr   x5, _StackBase
  br    x5

_Dead:
  wfe
  b     _Dead

.ltorg

.text
.balign 16

//
//  _Payload must be the very last byte of the binary image: the packer appends
//  the firmware volume right behind this label and compile_shim() rejects a
//  literal pool or padding that separates them.
//
.global _Payload
_Payload:
