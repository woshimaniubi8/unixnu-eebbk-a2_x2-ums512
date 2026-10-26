/*
#
  Debug serial port for the BbkA2 (Unisoc UMS512).

  Three sinks.  Only the panel is readable today; the other two are kept
  because they cost nothing once they are disabled (see below) and because
  they are the channels we will use once their wiring is understood.

   1. UART1 at 0x70100000 -- the SoC console UART (vendor DTS serial@70100000,
      /chosen stdout-path, kernel cmdline console=ttyS1).  Stock firmware runs it at
      921600 8n1, mainline Linux at 115200.  The physical test points are not
      identified yet, so this sink is currently write-only and invisible.

      The UART APB clock is never enabled, so the transmitter status bit
      (UART_LSR.TX_OVER) never asserts and the original implementation burned
      the full 256000 read timeout on *every* character.  With the MMU still
      off in SEC each of those reads is a Device-memory access, so one
      character of console output cost ~256000 uncached loads (~8 ms at
      32 MHz APB, ~3 ms even at 200 MHz) and a single 80 column trace line
      could cost the better part of a second.

      Since the sink is invisible, that timeout buys nothing.  UartPutc now
      runs a one-shot probe: the first character that does not see TX_OVER
      within a short budget latches mUartDead and the UART is never polled
      again for the rest of the boot.  If the UART does answer, behaviour is
      exactly what it always was (poll for TX_OVER, then write TXD).

   2. The ramoops console zone at 0xFFFB0000 -- a persistent-RAM buffer that
      survives a warm reset.  Android is supposed to mount it as
      /sys/fs/pstore/console-ramoops-0, but pstore currently recovers nothing
      from it, so it is written and not read.

   3. The panel, which is the channel that actually works (see below).
#

*/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/SerialPortLib.h>

#include "SprdFont8x16.h"

#define UART1_BASE           0x70100000ULL
#define UART_TXD             0x00
#define UART_LSR             0x08
#define UART_LSR_TX_OVER     (1U << 15)
//
// One-shot probe budget.  At 32 MHz APB a 921600 baud character takes ~35 us,
// so 20000 reads of a Device-memory register is several character times and is
// plenty to see a transmitter that is actually clocked.  It is also only ~1/13
// of the old per-character cost, and it is paid exactly once per boot.
//
#define UART_PROBE_TIMEOUT   20000U

#define PRZ_SIG              0x43474244U
#define PRZ_HDR              12U
#define CONSOLE_ZONE         0xFFFB0000ULL
//
// The kernel console rewrites the console zone on every Android boot, so the
// same log is mirrored into dmesg zone 0, which is created without ZAP_OLD and
// survives as dmesg-ramoops-0.  pstore only recovers a dmesg record whose
// payload opens with the kernel message header, so the first DMESG_PREFIX_LEN
// payload bytes are reserved for it and the mirrored ring starts after them.
//
#define CONSOLE_ZONE_DMESG   0xFFF80000ULL
#define DMESG_PREFIX         "====0.000000-0\n"
#define DMESG_PREFIX_LEN     ((UINT32)(sizeof (DMESG_PREFIX) - 1))
//
// One capacity for both zones: the kernel zaps a zone whose published size
// exceeds 0x8000 - PRZ_HDR, so the ring gives up the prefix bytes.
// 12 + DMESG_PREFIX_LEN + CONSOLE_CAP == 0x8000, i.e. 12 + 15 + 32741.
//
#define CONSOLE_CAP          (0x8000U - PRZ_HDR - DMESG_PREFIX_LEN)

STATIC UINT32   mConsolePos;
STATIC BOOLEAN  mConsoleWrapped;

//
// Set by the first character the UART fails to acknowledge; from then on the
// UART is skipped entirely.  See the file header.
//
STATIC BOOLEAN  mUartDead;

//
// Framebuffer console.
//
// The panel is the only output channel that needs no reboot to read.  LK
// leaves the DPU scanning out 0x9e000000 (logobuffer) at 1200x1920.  The pixel
// format is TODO(hardware): this file assumes RGB565/2-byte pixels; if the DPU
// scans 32 bpp the text below is garbled but harmless.
// pitch, and the backlight is already on when it jumps to us, so text written
// there is visible while the firmware is still running.  A hang then shows up
// as the last line on the screen instead of costing a power cycle.
//
// Geometry and pixel format measured from bare metal; see the M0 report.
//
// While SEC runs with the MMU off every framebuffer access is Device memory,
// so the number of loads and stores here is what decides whether a debug trace
// finishes before the 60 s AP watchdog expires.  Two rules follow:
//
//   * never touch a pixel the glyph does not cover, and
//   * move whole rows with the widest store the alignment allows.
//
// Neither changes a single visible pixel.
//

#define FB_BASE     0x9e000000ULL
#define FB_WIDTH    1200U
#define FB_HEIGHT   1920U
#define FB_STRIDE   (FB_WIDTH * 2U)
#define FB_COLS     (FB_WIDTH / 8U)
#define FB_ROWS     (FB_HEIGHT / 16U)
#define FB_CELL_W   8U
#define FB_CELL_H   16U
#define FB_BG       0x0000U
#define FB_FG       0xffffU
//
// Both FB_BASE (0x9e000000) and FB_STRIDE (2400) are multiples of 8, so every
// row starts on an 8 byte boundary and a whole row can be moved or filled a
// UINT64 at a time.
//
#define FB_ROW_WORDS  (FB_STRIDE / 8U)

STATIC UINT32   mFbRow;
STATIC UINT32   mFbCol;
STATIC BOOLEAN  mFbReady;

STATIC
VOID
FbFillSpan (
  IN UINT32  X,
  IN UINT32  Y,
  IN UINT32  Width,
  IN UINT16  Color
  )
{
  volatile UINT16  *Pixel;
  volatile UINT64  *Word;
  UINT64           Value;
  UINT32           Index;

  Pixel = (volatile UINT16 *)(UINTN)(FB_BASE + (UINT64)Y * FB_STRIDE + (UINT64)X * 2U);

  if ((X == 0) && (Width == FB_WIDTH)) {
    //
    // Whole row, 8 byte aligned: fill it with 64 bit stores instead of 320
    // 16 bit ones.  Same bytes on the panel.
    //
    Word  = (volatile UINT64 *)(VOID *)Pixel;
    Value = ((UINT64)Color << 48) | ((UINT64)Color << 32) |
            ((UINT64)Color << 16) | (UINT64)Color;
    for (Index = 0; Index < FB_ROW_WORDS; Index++) {
      Word[Index] = Value;
    }

    return;
  }

  for (Index = 0; Index < Width; Index++) {
    Pixel[Index] = Color;
  }
}

STATIC
VOID
FbClear (
  VOID
  )
{
  UINT32  Y;

  for (Y = 0; Y < FB_HEIGHT; Y++) {
    FbFillSpan (0, Y, FB_WIDTH, FB_BG);
  }
}

/**
  Scroll the panel up by one text row.

  This has to produce exactly the pixels the original per-pixel loop produced:
  every pixel of the screen moves up by FB_CELL_H scanlines and the bottom
  FB_CELL_H scanlines become background.  The original cleared a full glyph
  cell width of columns (FB_COLS * FB_CELL_W == FB_WIDTH pixels) but only for
  the FB_CELL_W columns it had just written -- the rest of those rows was
  already background, because nothing but FbGlyph ever sets a pixel to
  foreground and FbGlyph always writes a complete cell.  So copying whole rows
  and blanking whole rows is pixel-identical, and it lets each row move as
  FB_ROW_WORDS 64 bit loads/stores instead of FB_WIDTH 16 bit pairs.

  Cost: 464 rows * 80 word moves = 37120 load+store pairs, versus the original
  464 * 320 = 148480 load+store pairs, and no separate clear pass over 16 rows.
**/
STATIC
VOID
FbScroll (
  VOID
  )
{
  volatile UINT64  *Dst;
  volatile UINT64  *Src;
  UINT32           Row;
  UINT32           Index;

  for (Row = FB_CELL_H; Row < FB_HEIGHT; Row++) {
    Dst = (volatile UINT64 *)(UINTN)(FB_BASE + (UINT64)(Row - FB_CELL_H) * FB_STRIDE);
    Src = (volatile UINT64 *)(UINTN)(FB_BASE + (UINT64)Row * FB_STRIDE);
    for (Index = 0; Index < FB_ROW_WORDS; Index++) {
      Dst[Index] = Src[Index];
    }
  }

  for (Row = FB_HEIGHT - FB_CELL_H; Row < FB_HEIGHT; Row++) {
    FbFillSpan (0, Row, FB_WIDTH, FB_BG);
  }
}

STATIC
VOID
FbGlyph (
  IN UINT32  Col,
  IN UINT32  Row,
  IN UINT8   Ch,
  IN UINT16  Fg
  )
{
  CONST UINT8     *Bits;
  volatile UINT16 *Line;
  UINT32          X;
  UINT32          Y;

  //
  // A cell that is not on the panel is not drawn.  Without this check a wrap
  // that happened to be latched before a scroll would write past the end of
  // the framebuffer; the original code had no bound at all.
  //
  if ((Col >= FB_COLS) || (Row >= FB_ROWS)) {
    return;
  }

  if ((Ch < FONT_FIRST) || (Ch > FONT_LAST)) {
    Ch = (UINT8)0x3f;
  }

  Bits = &font8x16[(Ch - FONT_FIRST) * 16];

  //
  // The destination address advances by FB_STRIDE per scanline and by 2 per
  // column, so it can be carried in a pointer instead of being recomputed from
  // scratch for each of the 128 pixels.
  //
  Line = (volatile UINT16 *)(UINTN)(FB_BASE +
                                    (UINT64)(Row * FB_CELL_H) * FB_STRIDE +
                                    (UINT64)(Col * FB_CELL_W) * 2U);
  for (Y = 0; Y < FB_CELL_H; Y++) {
    for (X = 0; X < FB_CELL_W; X++) {
      if ((Bits[Y] & (UINT8)(0x80 >> X)) != 0) {
        Line[X] = Fg;
      } else {
        Line[X] = FB_BG;
      }
    }

    Line = (volatile UINT16 *)(UINTN)((UINTN)Line + FB_STRIDE);
  }
}

STATIC
VOID
FbPutc (
  IN CHAR8  C
  )
{
  UINT8  Ch;

  Ch = (UINT8)C;

  if ((Ch == 0x0a) || (Ch == 0x0d)) {
    mFbCol = 0;
    if (Ch == 0x0a) {
      mFbRow++;
    }
  } else if (Ch == 0x09) {
    mFbCol = (mFbCol + 4U) & ~3U;
  } else if ((Ch >= 0x20) && (Ch < 0x7f)) {
    FbGlyph (mFbCol, mFbRow, Ch, FB_FG);
    mFbCol++;
  } else {
    return;
  }

  if (mFbCol >= FB_COLS) {
    mFbCol = 0;
    mFbRow++;
  }

  while (mFbRow >= FB_ROWS) {
    FbScroll ();
    mFbRow--;
  }
}

STATIC
VOID
FbConWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  if (mFbReady == 0) {
    FbClear ();
    mFbRow   = 0;
    mFbCol   = 0;
    mFbReady = TRUE;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    FbPutc ((CHAR8)Buffer[Index]);
  }
}

STATIC
VOID
UartPutc (
  IN CHAR8  C
  )
{
  UINT32  Timeout;

  //
  // One-shot probe.  mUartDead is latched the first time the transmitter never
  // reports TX_OVER, and from then on this function is a single flag test --
  // the UART is not polled again for the rest of the boot.
  //
  if (!mUartDead) {
    for (Timeout = 0; Timeout < UART_PROBE_TIMEOUT; Timeout++) {
      if ((*(volatile UINT32 *)(UINTN)(UART1_BASE + UART_LSR) & UART_LSR_TX_OVER) != 0) {
        break;
      }
    }

    if (Timeout == UART_PROBE_TIMEOUT) {
      mUartDead = TRUE;
    } else {
      *(volatile UINT32 *)(UINTN)(UART1_BASE + UART_TXD) = (UINT32)(UINT8)C;
      return;
    }
  }

  //
  // Dead sink: nothing to write.  The byte is still kept in the ramoops zone
  // and on the panel by the caller.
  //
}

STATIC
VOID
ConsolePublish (
  VOID
  )
{
  volatile UINT32  *Header;
  volatile UINT32  *HeaderDmesg;
  volatile UINT8   *Prefix;
  UINT32           Index;

  Header      = (volatile UINT32 *)(UINTN)CONSOLE_ZONE;
  HeaderDmesg = (volatile UINT32 *)(UINTN)CONSOLE_ZONE_DMESG;

  //
  // Re-written every publish: the prefix lives outside the ring, so this only
  // ever stores the same bytes and costs nothing worth counting.
  //
  Prefix = (volatile UINT8 *)(UINTN)(CONSOLE_ZONE_DMESG + PRZ_HDR);
  for (Index = 0; Index < DMESG_PREFIX_LEN; Index++) {
    Prefix[Index] = (UINT8)DMESG_PREFIX[Index];
  }

  Header[0] = PRZ_SIG;
  Header[1] = mConsoleWrapped ? mConsolePos : 0;
  Header[2] = mConsoleWrapped ? CONSOLE_CAP : mConsolePos;

  //
  // The kernel de-wraps from the start offset and parses the message header
  // there, so start stays 0 and the record is always prefix + ring.  After a
  // wrap that reports only the newest bytes, which keeps both contiguous.
  //
  HeaderDmesg[0] = PRZ_SIG;
  HeaderDmesg[1] = 0;
  HeaderDmesg[2] = DMESG_PREFIX_LEN + Header[2];
}

STATIC
VOID
ConsolePutc (
  IN CHAR8  C
  )
{
  volatile UINT8  *Data;
  volatile UINT8  *DataDmesg;

  if (mConsolePos >= CONSOLE_CAP) {
    mConsolePos      = 0;
    mConsoleWrapped  = TRUE;
  }

  Data      = (volatile UINT8 *)(UINTN)(CONSOLE_ZONE + PRZ_HDR);
  DataDmesg = (volatile UINT8 *)(UINTN)(CONSOLE_ZONE_DMESG + PRZ_HDR + DMESG_PREFIX_LEN);
  Data[mConsolePos]      = (UINT8)C;
  DataDmesg[mConsolePos] = (UINT8)C;
  mConsolePos++;
}

//
//
// Called from the library constructor: snapshot what the previous boot
// (firmware or the BbkA2 test kernel stub) left in both RAM rings BEFORE this
// boot writes anything.  Kept separate from the report so the report can be
// printed late (BbkA2BringupDxe EndOfDxe, via DEBUG) and survive ring wrap.
STATIC UINT32  mResumeDmesgSig;
STATIC UINT32  mResumeDmesgStart;
STATIC UINT32  mResumeDmesgSize;
STATIC UINT32  mResumeConsoleSig;
STATIC UINT32  mResumeConsoleStart;
STATIC UINT32  mResumeConsoleSize;
STATIC UINT32  mResumeConsolePos;
STATIC UINT8   mResumeTail[65];
STATIC UINT8   mResumeStub[128];
STATIC UINT32  mResumeStubLen = 0;
STATIC UINT32  mResumeRaw[4];
STATIC UINT32  mResumeTrace[8];
STATIC BOOLEAN mResumeCaptured = FALSE;

VOID
BbkA2RingCapture (
  VOID
  )
{
  volatile UINT32  *HeaderDmesg;
  volatile UINT32  *HeaderConsole;
  volatile UINT32  *Scratch;
  volatile UINT8   *DataDmesg;
  UINTN             Index;
  UINTN             Start;
  UINT32            Length;

  if (mResumeCaptured) {
    return;
  }

  mResumeCaptured  = TRUE;
  HeaderDmesg      = (volatile UINT32 *)(UINTN)CONSOLE_ZONE_DMESG;
  HeaderConsole    = (volatile UINT32 *)(UINTN)CONSOLE_ZONE;
  mResumeDmesgSig     = HeaderDmesg[0];
  mResumeDmesgStart   = HeaderDmesg[1];
  mResumeDmesgSize    = HeaderDmesg[2];
  mResumeConsoleSig   = HeaderConsole[0];
  mResumeConsoleStart = HeaderConsole[1];
  mResumeConsoleSize  = HeaderConsole[2];
  mResumeConsolePos   = (UINT32)mConsolePos;

  DataDmesg = (volatile UINT8 *)(UINTN)(CONSOLE_ZONE_DMESG + PRZ_HDR + DMESG_PREFIX_LEN);
  Start     = (mConsolePos > 64) ? (mConsolePos - 64) : 0;
  for (Index = 0; Index < (mConsolePos - Start); Index++) {
    mResumeTail[Index] = DataDmesg[Start + Index];
    if ((mResumeTail[Index] < 0x20) || (mResumeTail[Index] > 0x7e)) {
      mResumeTail[Index] = '.';
    }
  }

  mResumeTail[Index] = 0;

  // The BbkA2 test kernel stub (v11) leaves its banner in scratch DRAM at
  // 0xC6000000 (initrd reserve top): "BbkA2ST" magic, u32 length, banner bytes.  The stub never
  // touches the prz zones, so the sprd pstore group stays mountable and this
  // line is how stub output gets read out.
  Scratch = (volatile UINT32 *)(UINTN)0xC6000000;
  mResumeRaw[0] = Scratch[0];
  mResumeRaw[1] = Scratch[1];
  mResumeRaw[2] = Scratch[2];
  mResumeRaw[3] = Scratch[3];

  {
    volatile UINT32  *TraceZone;
    UINTN             Tdx;

    TraceZone = (volatile UINT32 *)(UINTN)0xC6002000;
    for (Tdx = 0; Tdx < 8; Tdx++) {
      mResumeTrace[Tdx] = TraceZone[Tdx];
    }
  }
  if (Scratch[0] == 0x54533545) {
    Length = Scratch[1];
    if (Length > 127) {
      Length = 127;
    }

    for (Index = 0; Index < Length; Index++) {
      mResumeStub[Index] = ((volatile UINT8 *)(UINTN)0xC6000008)[Index];
      if ((mResumeStub[Index] < 0x20) || (mResumeStub[Index] > 0x7e)) {
        mResumeStub[Index] = '.';
      }
    }

    mResumeStub[Index] = 0;
    mResumeStubLen = (UINT32)Index;

    // Clear the magic so the cookie describes exactly one follow-up boot:
    // if it survived, the third boot of a cycle (or the first boot of the
    // next cycle) would wrongly skip the chain as well.
    Scratch[0] = 0;
    Scratch[1] = 0;
  }
}

// Hand the snapshotted previous-boot ring state to a caller that owns a
// DebugLib instance (BbkA2BringupDxe).  Printing from inside SerialPortLib via
// ConsolePutc is useless: every module links a private copy of this library
// with its own mConsolePos, so bytes written here are immediately overwritten
// by the DxeCore copy writing at the same ring offset.  DEBUG() from the
// caller goes through DxeCore's one true copy and survives.
typedef struct {
  UINT32  DmesgSig;
  UINT32  DmesgStart;
  UINT32  DmesgSize;
  UINT32  ConsoleSig;
  UINT32  ConsoleStart;
  UINT32  ConsoleSize;
  UINT32  ConsolePos;
  CHAR8   Tail[65];
  UINT32  StubLen;
  CHAR8   StubBanner[128];
  UINT32  RawScratch[4];
  UINT32  Trace[8];
} BBK_A2_RING_RESUME_SNAPSHOT;

RETURN_STATUS
EFIAPI
BbkA2RingResumeGetSnapshot (
  OUT BBK_A2_RING_RESUME_SNAPSHOT  *Out
  )
{
  UINTN  Index;
  UINTN  Tdx;

  if ((0 == mResumeCaptured) || (NULL == Out)) {
    return RETURN_NOT_READY;
  }

  Out->DmesgSig      = mResumeDmesgSig;
  Out->DmesgStart    = mResumeDmesgStart;
  Out->DmesgSize     = mResumeDmesgSize;
  Out->ConsoleSig    = mResumeConsoleSig;
  Out->ConsoleStart  = mResumeConsoleStart;
  Out->ConsoleSize   = mResumeConsoleSize;
  Out->ConsolePos    = mResumeConsolePos;
  for (Index = 0; Index < sizeof (Out->Tail); Index++) {
    Out->Tail[Index] = mResumeTail[Index];
  }

  Out->StubLen = mResumeStubLen;
  for (Index = 0; Index < sizeof (Out->StubBanner); Index++) {
    Out->StubBanner[Index] = mResumeStub[Index];
  }

  Out->RawScratch[0] = mResumeRaw[0];
  Out->RawScratch[1] = mResumeRaw[1];
  Out->RawScratch[2] = mResumeRaw[2];
  Out->RawScratch[3] = mResumeRaw[3];
  for (Tdx = 0; Tdx < 8; Tdx++) {
    Out->Trace[Tdx] = mResumeTrace[Tdx];
  }

  return RETURN_SUCCESS;
}



/**
  Take over the log already in the dmesg zone instead of starting over.

  Nothing clears the zone except a cold boot: within one boot every stage that
  maps this library (SEC first, then each DXE module) constructs it again and
  lands here with the previous stage's bytes still in place.  Reading the
  published size back is all it takes to continue writing behind them, so SEC
  and DXE output end up in one record instead of DXE replacing SEC.

  The dmesg record is always prefix followed by the ring, so its start field
  stays 0 and the ring length is simply size - DMESG_PREFIX_LEN.  That size
  only pins the position until the ring wraps; from then on it is the constant
  capacity, and the moving position is read from the plain zone's start.
**/
STATIC
VOID
ConsoleAdopt (
  VOID
  )
{
  volatile UINT32  *Header;
  volatile UINT32  *HeaderPlain;
  UINT32           Sig;
  UINT32           Size;
  UINT32           Pos;

  Header      = (volatile UINT32 *)(UINTN)CONSOLE_ZONE_DMESG;
  HeaderPlain = (volatile UINT32 *)(UINTN)CONSOLE_ZONE;
  Sig         = Header[0];
  Size        = Header[2];

  if ((Sig != PRZ_SIG) || (Size <= DMESG_PREFIX_LEN)) {
    //
    // Nothing recoverable -- a cold boot, or a zone the kernel zapped.
    //
    mConsolePos     = 0;
    mConsoleWrapped = FALSE;
    return;
  }

  //
  // The previous writer stopped exactly where the published size says.  A ring
  // that reached the capacity had wrapped, which is the state ConsolePutc
  // would have left mConsoleWrapped in.
  //
  // A wrapped ring publishes the capacity, a constant, so the size stops
  // changing and the write position can only come from the plain zone, which
  // keeps it in start.  That zone is rewritten by the kernel on every Android
  // boot, so a start that is not ours leaves the position unknown and the
  // capacity is the safe answer: ConsolePutc then restarts the ring.
  //
  Pos = Size - DMESG_PREFIX_LEN;
  if (Pos >= CONSOLE_CAP) {
    mConsolePos = CONSOLE_CAP;
    if (HeaderPlain[0] == PRZ_SIG) {
      mConsolePos = HeaderPlain[1];
      if (mConsolePos > CONSOLE_CAP) {
        mConsolePos = CONSOLE_CAP;
      }
    }

    mConsoleWrapped = TRUE;
  } else {
    mConsolePos     = Pos;
    mConsoleWrapped = FALSE;
  }
}

/**
  Publish the persistent RAM header at construction time.

  The position comes from ConsoleAdopt, not from zero: within one boot every
  stage that maps this library (SEC first, then each DXE module) constructs it
  again, and a reset here would overwrite the previous stage's bytes from
  offset 0.  On a cold boot the zone holds no valid record and ConsoleAdopt
  starts at zero, which is the same state a reset would have produced.
**/
RETURN_STATUS
EFIAPI
SprdDebugSerialPortLibConstructor (
  VOID
  )
{
  ConsoleAdopt ();
  ConsolePublish ();
  // v22b: scratch/trace live in the initrd reserve (0xC6000000/0xC6002000),
  // which is NOT mapped in the early DxeCore page tables.  Capturing there
  // from this constructor faults (Synchronous Exception in DxeCore+0x25B4),
  // so BbkA2BringupDxe calls BbkA2RingCapture() late instead, where the platform
  // memory map (initrd included) is fully mapped.
  return RETURN_SUCCESS;
}

/**
  Initialize the serial device hardware.
**/
RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  ConsoleAdopt ();
  ConsolePublish ();
  return RETURN_SUCCESS;
}

/**
  Write data to the serial device.
**/
UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  if (Buffer == NULL) {
    return 0;
  }

  //
  // 每次写前从已发布的头部重新对齐游标：mConsolePos 只是本镜像的私有副本，
  // 构造之后其它镜像仍在往 ring 里写，沿用陈旧游标会覆盖别人已写好的内容。
  //
  ConsoleAdopt ();

  for (Index = 0; Index < NumberOfBytes; Index++) {
    UartPutc ((CHAR8)Buffer[Index]);
    ConsolePutc ((CHAR8)Buffer[Index]);
  }

  FbConWrite (Buffer, NumberOfBytes);
  ConsolePublish ();
  return NumberOfBytes;
}

/**
  Read data from the serial device.  Not supported: this port is write-only.
**/
UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  return 0;
}

/**
  Poll the serial device for input.  Always FALSE: this port is write-only.
**/
BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return FALSE;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  if (Control == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  *Control = 0;
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  return RETURN_UNSUPPORTED;
}
