//
// BbkA2 platform specific SEC assembly hook.
//
// Called from ModuleEntryPoint right after the data cache is cleaned and after
// the boot loader's x0 (the vendor FDT) has been stored in gSecEntryArg0, so
// x0-x18 are scratch here.
//
// The panel is painted cyan and held for two seconds. If cyan is what stays on
// screen, SEC entry code ran and the failure is after this point. The hold
// matters: without it the colour is gone long before a human can look.
//
// Copyright (c) 2026, e5-xnu project. SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

// 0x9E000000 is the U-Boot logobuffer (vendor DTS logobuffer@9e000000); the
// panel is 1200x1920, so 2304000 halfwords are painted starting there.  The
// pixel format is TODO(hardware): this hook writes RGB565 halfwords.
#define BBK_A2_FB_MOVZ_HI   0x9E00
#define BBK_A2_FB_PIXELS_LO 0x2800
#define BBK_A2_FB_PIXELS_HI 0x0232
#define BBK_A2_RGB565_CYAN  0x07FF

// Two seconds of the 26 MHz architectural counter, split for movz/movk.
#define BBK_A2_HOLD_LO      0x7500
#define BBK_A2_HOLD_HI      0x0319

ASM_FUNC (PlatformAssemblyInitialize)
  movz  x0, #BBK_A2_FB_MOVZ_HI, lsl #16
  movz  w1, #BBK_A2_RGB565_CYAN
  movz  w2, #BBK_A2_FB_PIXELS_LO
  movk  w2, #BBK_A2_FB_PIXELS_HI, lsl #16
1:
  strh  w1, [x0], #2
  subs  x2, x2, #1
  b.ne  1b

  // Busy wait two seconds so a human can see the colour.
  movz  w4, #BBK_A2_HOLD_LO
  movk  w4, #BBK_A2_HOLD_HI, lsl #16
  mrs   x3, cntvct_el0
  add   x4, x3, x4
2:
  mrs   x3, cntvct_el0
  cmp   x3, x4
  b.lo  2b
  ret
