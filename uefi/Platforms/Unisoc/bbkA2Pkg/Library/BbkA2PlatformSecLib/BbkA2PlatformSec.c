/**
  BbkA2 platform specific SEC hooks.

  These are deliberately bare metal: at this point in the boot nothing is
  initialised, so no library other than the ones already linked into SEC may be
  used. The panel and UART1 are both already live when cboot hands over, so they
  are written to directly.

  The C hook paints the panel magenta and holds it for two seconds. Each
  colour on screen is a stage marker: cyan means the assembly hook ran,
  magenta means C code ran, and the text console that follows means SEC reached
  SerialPortInitialize.

  Copyright (c) 2026, e5-xnu project. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/PlatformSecLib.h>

#define BBK_A2_FB_BASE      0x9E000000U   // U-Boot logobuffer (vendor DTS)
#define BBK_A2_FB_PIXELS    (1200U * 1920U)   // panel from U-Boot DTS lcd_himax8279_boe

#define BBK_A2_UART1_BASE   0x70100000U   // vendor DTS serial@70100000
#define BBK_A2_UART_TXD     0x00U
#define BBK_A2_UART_LSR     0x08U
#define BBK_A2_UART_LSR_TX_OVER  (1U << 15)

#define BBK_A2_HOLD_TICKS   (26000000ULL * 2ULL)   // 2 s at the 26 MHz counter

/**
  Fill the whole panel with one RGB565 colour.

  @param[in] Colour  RGB565 pixel value.
**/
STATIC
VOID
BbkA2Paint (
  IN UINT16  Colour
  )
{
  volatile UINT16  *Pixel;
  UINT32           Index;

  Pixel = (volatile UINT16 *)(UINTN)BBK_A2_FB_BASE;
  for (Index = 0; Index < BBK_A2_FB_PIXELS; Index++) {
    Pixel[Index] = Colour;
  }
}

/**
  Busy wait until the architectural counter has advanced by the given ticks.
**/
STATIC
VOID
BbkA2Hold (
  IN UINT64  Ticks
  )
{
  UINT64  Start;
  UINT64  Now;

  __asm__ volatile ("isb" ::: "memory");
  __asm__ volatile ("mrs %0, cntvct_el0" : "=r" (Start));
  do {
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r" (Now));
  } while ((Now - Start) < Ticks);
}

/**
  Poll the transmitter and write one byte to UART1.
**/
STATIC
VOID
BbkA2Putc (
  IN CHAR8  Char
  )
{
  volatile UINT32  *Uart;
  UINT32           Timeout;

  Uart    = (volatile UINT32 *)(UINTN)BBK_A2_UART1_BASE;
  Timeout = 256000;
  while ((Timeout-- != 0) && ((Uart[BBK_A2_UART_LSR / 4] & BBK_A2_UART_LSR_TX_OVER) == 0)) {
  }

  *(volatile UINT8 *)(UINTN)(BBK_A2_UART1_BASE + BBK_A2_UART_TXD) = (UINT8)Char;
}

/**
  Platform specific C initialisation, called from SecMain before the serial
  port is brought up.
**/
VOID
PlatformInitialize (
  VOID
  )
{
  STATIC CONST CHAR8  Banner[] = "BbkA2SEC: platform C code reached\r\n";
  UINTN               Index;

  BbkA2Paint (0xF81F);   // magenta
  for (Index = 0; Banner[Index] != 0; Index++) {
    BbkA2Putc (Banner[Index]);
  }

  BbkA2Hold (BBK_A2_HOLD_TICKS);
}
