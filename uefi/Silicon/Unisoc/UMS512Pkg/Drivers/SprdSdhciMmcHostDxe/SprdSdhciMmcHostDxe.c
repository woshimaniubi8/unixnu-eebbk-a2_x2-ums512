/** @file
  Unisoc UMS512 (SharkL5Pro, T610 family) SDHCI eMMC host driver.

  The controller is the SDHCI-compatible "sprd,sdhc-r11p1" block at
  0x71400000 (vendor DT: sdio@71400000, 8-bit non-removable eMMC, HS400ES,
  IRQ 10).  cboot/U-Boot has already identified the card and clocked the
  controller before it jumps to the boot image, so this driver runs in
  "takeover" mode:

    * it does NOT reprogram the Unisoc clock gates, PMIC rails or the
      HS400ES DLL delay registers (that would need the AP/AON clock trees
      and the SPI PMIC, none of which is modelled in this firmware yet);
    * it resets only the CMD/DAT lines, then programs the standard SDHCI
      clock divider from the CAP base clock, runs the card at legacy/26 MHz
      8-bit mode and transfers data with PIO (no ADMA, no SMMU).

  It implements EFI_MMC_HOST_PROTOCOL (EmbeddedPkg/Include/Protocol/MmcHost.h);
  EmbeddedPkg/Universal/MmcDxe then performs card identification and produces
  the BlockIo handle that PartitionDxe + Fat consume.  With that in place
  UniXNU can read the payload FAT volume written to the socko partition
  (`fastboot flash socko dist/payload.img`).

  Unverified on hardware.  Known risks, in order:
    * HS400ES -> legacy timing without re-running the Unisoc delay tuning;
    * cboot may leave the card powered in 1.8 V I/O (CMD6 HS400ES), which the
      legacy 3.3 V init sequence below does not switch back;
    * SDHCI base clock reported by CAP may not be the real SoC clock.
  The driver therefore falls back to "do not touch the clock" when CAP looks
  implausible, and every failure returns an EFI error instead of hanging.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/MmcHost.h>

#include <Ums512.h>

//
// SDHCI register offsets (SD Host Controller Simplified Specification 3.0).
//
#define SDHCI_BLOCK_SIZE          0x04
#define SDHCI_BLOCK_COUNT         0x06
#define SDHCI_ARGUMENT            0x08
#define SDHCI_TRANSFER_MODE       0x0C
#define SDHCI_COMMAND             0x0E
#define SDHCI_RESPONSE            0x10
#define SDHCI_BUFFER              0x20
#define SDHCI_PRESENT_STATE       0x24
#define SDHCI_HOST_CONTROL        0x28
#define SDHCI_POWER_CONTROL       0x29
#define SDHCI_CLOCK_CONTROL       0x2C
#define SDHCI_TIMEOUT_CONTROL     0x2E
#define SDHCI_SOFTWARE_RESET      0x2F
#define SDHCI_NORMAL_INT_STATUS   0x30
#define SDHCI_ERROR_INT_STATUS    0x32
#define SDHCI_NORMAL_INT_ENABLE   0x34
#define SDHCI_ERROR_INT_ENABLE    0x36
#define SDHCI_NORMAL_INT_SIGNAL   0x38
#define SDHCI_ERROR_INT_SIGNAL    0x3A
#define SDHCI_HOST_CONTROL2       0x3E
#define SDHCI_CAPABILITIES        0x40

//
// Present state bits.
//
#define SDHCI_PS_CMD_INHIBIT      (1U << 0)
#define SDHCI_PS_DAT_INHIBIT      (1U << 1)
#define SDHCI_PS_DAT_ACTIVE       (1U << 2)
#define SDHCI_PS_BUF_WR_ENABLE    (1U << 10)
#define SDHCI_PS_BUF_RD_ENABLE    (1U << 11)

//
// Normal interrupt bits.
//
#define SDHCI_INT_CMD_COMPLETE    (1U << 0)
#define SDHCI_INT_TRANSFER_COMPLETE (1U << 1)
#define SDHCI_INT_BUF_WR_READY    (1U << 4)
#define SDHCI_INT_BUF_RD_READY    (1U << 5)
#define SDHCI_INT_ERROR           (1U << 15)
#define SDHCI_INT_ALL             0xFFFFU

//
// Host control 1 bits.
//
#define SDHCI_HC1_LED             (1U << 0)
#define SDHCI_HC1_4BIT            (1U << 1)
#define SDHCI_HC1_HIGH_SPEED      (1U << 2)
#define SDHCI_HC1_8BIT            (1U << 5)

//
// Clock control bits.
//
#define SDHCI_CLK_INTERNAL_EN     (1U << 0)
#define SDHCI_CLK_INTERNAL_STABLE (1U << 1)
#define SDHCI_CLK_SD_EN           (1U << 2)

//
// Software reset bits.
//
#define SDHCI_SR_ALL              (1U << 0)
#define SDHCI_SR_CMD              (1U << 1)
#define SDHCI_SR_DAT              (1U << 2)

//
// Command register fields.
//
#define SDHCI_CMD_RESP_NONE       0x0000
#define SDHCI_CMD_RESP_136        0x0001
#define SDHCI_CMD_RESP_48         0x0002
#define SDHCI_CMD_RESP_48_BUSY    0x0003
#define SDHCI_CMD_CRC_CHECK       (1U << 3)
#define SDHCI_CMD_INDEX_CHECK     (1U << 4)
#define SDHCI_CMD_DATA_PRESENT    (1U << 5)

//
// Transfer mode fields.
//
#define SDHCI_TM_BLOCK_COUNT_EN   (1U << 1)
#define SDHCI_TM_DATA_READ        (1U << 4)
#define SDHCI_TM_MULTI_BLOCK      (1U << 5)

#define SDHCI_TIMEOUT_MS          2000
#define SDHCI_POLL_STALL_US       10
#define SDHCI_MAX_BLOCK_SIZE      512

typedef struct {
  EFI_MMC_HOST_PROTOCOL    Host;
  UINTN                    Base;
  UINT32                   Capabilities;
  UINT32                   BaseClockMhz;
  UINT32                   PendingIndex;
  UINT32                   PendingArgument;
  UINT32                   PendingFlags;
  UINT32                   Response[4];
  BOOLEAN                  DataPending;
  BOOLEAN                  Initialized;
} SPRD_SDHCI_MMC_HOST;

STATIC
EFI_STATUS
SdhciWaitPresentState (
  IN SPRD_SDHCI_MMC_HOST  *Host,
  IN UINT32                Mask,
  IN BOOLEAN               Set,
  IN UINT32                TimeoutMs
  )
{
  UINT32  Waited;

  for (Waited = 0; Waited < TimeoutMs; Waited++) {
    UINT32  State = MmioRead32 (Host->Base + SDHCI_PRESENT_STATE);
    if (((State & Mask) != 0) == Set) {
      return EFI_SUCCESS;
    }

    gBS->Stall (1000);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SdhciResetLines (
  IN SPRD_SDHCI_MMC_HOST  *Host,
  IN UINT8                 Mask
  )
{
  UINT32  Waited;

  MmioWrite8 (Host->Base + SDHCI_SOFTWARE_RESET, Mask);
  for (Waited = 0; Waited < SDHCI_TIMEOUT_MS; Waited++) {
    if ((MmioRead8 (Host->Base + SDHCI_SOFTWARE_RESET) & Mask) == 0) {
      return EFI_SUCCESS;
    }

    gBS->Stall (1000);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SdhciSetClock (
  IN SPRD_SDHCI_MMC_HOST  *Host,
  IN UINT32                FrequencyKhz
  )
{
  UINT32  BaseKhz;
  UINT32  Divisor;
  UINT16  ClockControl;
  UINT32  Waited;

  if (FrequencyKhz == 0) {
    FrequencyKhz = 400;
  }

  BaseKhz = Host->BaseClockMhz * 1000;
  if (BaseKhz == 0) {
    // CAP did not report a plausible base clock: leave the controller alone,
    // cboot has already clocked the card and the legacy card clock is < 52 MHz.
    return EFI_SUCCESS;
  }

  Divisor = (BaseKhz + 2 * FrequencyKhz - 1) / (2 * FrequencyKhz);
  if (Divisor == 0) {
    Divisor = 1;
  }

  if (Divisor > 0xFF) {
    Divisor = 0xFF;
  }

  // Stop the card clock, program the divider, wait for the internal clock.
  MmioWrite16 (Host->Base + SDHCI_CLOCK_CONTROL, 0);
  ClockControl = (UINT16)(SDHCI_CLK_INTERNAL_EN | ((Divisor & 0xFF) << 8));
  MmioWrite16 (Host->Base + SDHCI_CLOCK_CONTROL, ClockControl);
  for (Waited = 0; Waited < SDHCI_TIMEOUT_MS; Waited++) {
    if ((MmioRead16 (Host->Base + SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INTERNAL_STABLE) != 0) {
      break;
    }

    gBS->Stall (1000);
  }

  if (Waited >= SDHCI_TIMEOUT_MS) {
    return EFI_TIMEOUT;
  }

  MmioWrite16 (Host->Base + SDHCI_CLOCK_CONTROL, (UINT16)(ClockControl | SDHCI_CLK_SD_EN));
  gBS->Stall (1000);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SdhciControllerInit (
  IN SPRD_SDHCI_MMC_HOST  *Host
  )
{
  UINT8  Power;

  Host->Capabilities = MmioRead32 (Host->Base + SDHCI_CAPABILITIES);
  Host->BaseClockMhz = (Host->Capabilities >> 8) & 0xFF;
  if ((Host->BaseClockMhz < 10) || (Host->BaseClockMhz > 400)) {
    // Implausible CAP value: keep the bootloader clock, see SdhciSetClock().
    Host->BaseClockMhz = 0;
  }

  // Only the CMD/DAT lines are reset; a full controller reset would also clear
  // the vendor delay registers cboot programmed for HS400ES.
  if (EFI_ERROR (SdhciResetLines (Host, SDHCI_SR_CMD | SDHCI_SR_DAT))) {
    DEBUG ((DEBUG_ERROR, "SprdSdhci: CMD/DAT line reset timed out\n"));
    return EFI_DEVICE_ERROR;
  }

  // Keep the card powered: only switch the bus power on if cboot left it off.
  Power = MmioRead8 (Host->Base + SDHCI_POWER_CONTROL);
  if ((Power & 0x0F) == 0) {
    MmioWrite8 (Host->Base + SDHCI_POWER_CONTROL, 0x0E); // 3.3 V
    gBS->Stall (1000);
    MmioWrite8 (Host->Base + SDHCI_POWER_CONTROL, 0x0F); // bus power on
    gBS->Stall (10000);
  }

  MmioWrite8 (Host->Base + SDHCI_TIMEOUT_CONTROL, 0x0E);
  MmioWrite16 (Host->Base + SDHCI_NORMAL_INT_STATUS, SDHCI_INT_ALL);
  MmioWrite16 (Host->Base + SDHCI_ERROR_INT_STATUS, SDHCI_INT_ALL);
  MmioWrite16 (Host->Base + SDHCI_NORMAL_INT_ENABLE, SDHCI_INT_ALL);
  MmioWrite16 (Host->Base + SDHCI_ERROR_INT_ENABLE, SDHCI_INT_ALL);
  // Polling only: never route the controller interrupt to the GIC.
  MmioWrite16 (Host->Base + SDHCI_NORMAL_INT_SIGNAL, 0);
  MmioWrite16 (Host->Base + SDHCI_ERROR_INT_SIGNAL, 0);

  // 1-bit bus for identification; SetIos() widens it later.
  MmioWrite8 (Host->Base + SDHCI_HOST_CONTROL, 0);
  if (EFI_ERROR (SdhciSetClock (Host, 400))) {
    DEBUG ((DEBUG_ERROR, "SprdSdhci: 400 kHz clock setup failed\n"));
    return EFI_DEVICE_ERROR;
  }

  Host->Initialized = TRUE;
  DEBUG ((DEBUG_INFO, "SprdSdhci: CAP=%08x baseClock=%u MHz\n", Host->Capabilities, Host->BaseClockMhz));

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
SdhciIsDataCommand (
  IN UINT32  Index
  )
{
  switch (Index) {
    case 6:   // SWITCH (read 512-byte status)
    case 17:  // READ_SINGLE_BLOCK
    case 18:  // READ_MULTIPLE_BLOCK
    case 24:  // WRITE_BLOCK
    case 25:  // WRITE_MULTIPLE_BLOCK
    case 51:  // SEND_SCR (via ACMD51)
      return TRUE;
    default:
      return FALSE;
  }
}

STATIC
EFI_STATUS
SdhciSendCommandNow (
  IN SPRD_SDHCI_MMC_HOST  *Host,
  IN UINT32                Cmd,
  IN UINT32                Argument,
  IN UINT32                DataLength,
  IN BOOLEAN               IsWrite
  )
{
  UINT32  Index;
  UINT32  BlockSize;
  UINT32  BlockCount;
  UINT16  Command;
  UINT16  TransferMode;
  UINT16  Status;
  UINT16  Error;
  UINT32  Waited;
  UINT32  I;

  Index = MMC_GET_INDX (Cmd);

  if (EFI_ERROR (SdhciWaitPresentState (Host, SDHCI_PS_CMD_INHIBIT, FALSE, SDHCI_TIMEOUT_MS))) {
    DEBUG ((DEBUG_ERROR, "SprdSdhci: CMD inhibit stuck (cmd %u)\n", Index));
    return EFI_TIMEOUT;
  }

  if (DataLength > 0) {
    if (EFI_ERROR (SdhciWaitPresentState (Host, SDHCI_PS_DAT_INHIBIT, FALSE, SDHCI_TIMEOUT_MS))) {
      DEBUG ((DEBUG_ERROR, "SprdSdhci: DAT inhibit stuck (cmd %u)\n", Index));
      return EFI_TIMEOUT;
    }

    BlockSize = (DataLength >= SDHCI_MAX_BLOCK_SIZE) ? SDHCI_MAX_BLOCK_SIZE : DataLength;
    if (BlockSize == 0) {
      BlockSize = 1;
    }

    BlockCount = (DataLength + BlockSize - 1) / BlockSize;
    MmioWrite16 (Host->Base + SDHCI_BLOCK_SIZE, (UINT16)BlockSize);
    MmioWrite16 (Host->Base + SDHCI_BLOCK_COUNT, (UINT16)BlockCount);
    TransferMode = (UINT16)(IsWrite ? 0 : SDHCI_TM_DATA_READ);
    if (BlockCount > 1) {
      TransferMode |= SDHCI_TM_MULTI_BLOCK | SDHCI_TM_BLOCK_COUNT_EN;
    }

    MmioWrite16 (Host->Base + SDHCI_TRANSFER_MODE, TransferMode);
  }

  MmioWrite32 (Host->Base + SDHCI_ARGUMENT, Argument);

  Command = (UINT16)(Index << 8);
  if ((Cmd & MMC_CMD_WAIT_RESPONSE) != 0) {
    if ((Cmd & MMC_CMD_LONG_RESPONSE) != 0) {
      Command |= SDHCI_CMD_RESP_136;
    } else if ((Index == 7) || (Index == 12)) {
      // R1b: wait for DAT0 busy to clear before command complete.
      Command |= SDHCI_CMD_RESP_48_BUSY;
    } else {
      Command |= SDHCI_CMD_RESP_48;
    }

    if ((Cmd & MMC_CMD_NO_CRC_RESPONSE) == 0) {
      Command |= SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK;
    }
  }

  if (DataLength > 0) {
    Command |= SDHCI_CMD_DATA_PRESENT;
  }

  MmioWrite16 (Host->Base + SDHCI_NORMAL_INT_STATUS, SDHCI_INT_ALL);
  MmioWrite16 (Host->Base + SDHCI_ERROR_INT_STATUS, SDHCI_INT_ALL);
  MmioWrite16 (Host->Base + SDHCI_COMMAND, Command);

  for (Waited = 0; Waited < SDHCI_TIMEOUT_MS; Waited++) {
    Status = MmioRead16 (Host->Base + SDHCI_NORMAL_INT_STATUS);
    Error  = MmioRead16 (Host->Base + SDHCI_ERROR_INT_STATUS);
    if ((Error != 0) || ((Status & SDHCI_INT_ERROR) != 0)) {
      DEBUG ((DEBUG_ERROR, "SprdSdhci: cmd %u error: normal=%04x error=%04x\n", Index, Status, Error));
      MmioWrite16 (Host->Base + SDHCI_NORMAL_INT_STATUS, SDHCI_INT_ALL);
      MmioWrite16 (Host->Base + SDHCI_ERROR_INT_STATUS, SDHCI_INT_ALL);
      return EFI_DEVICE_ERROR;
    }

    if ((Status & SDHCI_INT_CMD_COMPLETE) != 0) {
      break;
    }

    gBS->Stall (1000);
  }

  if (Waited >= SDHCI_TIMEOUT_MS) {
    DEBUG ((DEBUG_ERROR, "SprdSdhci: cmd %u completion timeout\n", Index));
    return EFI_TIMEOUT;
  }

  for (I = 0; I < 4; I++) {
    Host->Response[I] = MmioRead32 (Host->Base + SDHCI_RESPONSE + I * 4);
  }

  MmioWrite16 (Host->Base + SDHCI_NORMAL_INT_STATUS, SDHCI_INT_CMD_COMPLETE);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SprdSdhciSendCommand (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_CMD                 Cmd,
  IN UINT32                  Argument
  )
{
  SPRD_SDHCI_MMC_HOST  *Host;
  UINT32                Index;

  Host  = (SPRD_SDHCI_MMC_HOST *)This;
  Index = MMC_GET_INDX (Cmd);

  if (!Host->Initialized) {
    if (EFI_ERROR (SdhciControllerInit (Host))) {
      return EFI_DEVICE_ERROR;
    }
  }

  if (SdhciIsDataCommand (Index)) {
    // ReadBlockData()/WriteBlockData() are called immediately after this and
    // carry the transfer length, so the command is issued there.
    Host->PendingIndex    = Index;
    Host->PendingArgument = Argument;
    Host->PendingFlags    = Cmd;
    Host->DataPending     = TRUE;
    return EFI_SUCCESS;
  }

  Host->DataPending = FALSE;
  return SdhciSendCommandNow (Host, Cmd, Argument, 0, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SprdSdhciReceiveResponse (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_RESPONSE_TYPE       Type,
  IN UINT32                 *Buffer
  )
{
  SPRD_SDHCI_MMC_HOST  *Host;

  Host = (SPRD_SDHCI_MMC_HOST *)This;
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Type == MMC_RESPONSE_TYPE_R2) {
    CopyMem (Buffer, Host->Response, sizeof (Host->Response));
  } else {
    Buffer[0] = Host->Response[0];
  }


  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SdhciTransferComplete (
  IN SPRD_SDHCI_MMC_HOST  *Host
  )
{
  UINT32  Waited;
  UINT16  Status;
  UINT16  Error;

  for (Waited = 0; Waited < SDHCI_TIMEOUT_MS; Waited++) {
    Status = MmioRead16 (Host->Base + SDHCI_NORMAL_INT_STATUS);
    Error  = MmioRead16 (Host->Base + SDHCI_ERROR_INT_STATUS);
    if ((Error != 0) || ((Status & SDHCI_INT_ERROR) != 0)) {
      DEBUG ((DEBUG_ERROR, "SprdSdhci: transfer error: normal=%04x error=%04x\n", Status, Error));
      MmioWrite16 (Host->Base + SDHCI_NORMAL_INT_STATUS, SDHCI_INT_ALL);
      MmioWrite16 (Host->Base + SDHCI_ERROR_INT_STATUS, SDHCI_INT_ALL);
      return EFI_DEVICE_ERROR;
    }

    if ((Status & SDHCI_INT_TRANSFER_COMPLETE) != 0) {
      break;
    }

    gBS->Stall (1000);
  }

  if (Waited >= SDHCI_TIMEOUT_MS) {
    DEBUG ((DEBUG_ERROR, "SprdSdhci: transfer completion timeout\n"));
    return EFI_TIMEOUT;
  }

  MmioWrite16 (Host->Base + SDHCI_NORMAL_INT_STATUS, SDHCI_INT_ALL);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SprdSdhciReadBlockData (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN EFI_LBA                 Lba,
  IN UINTN                   Length,
  OUT UINT32                *Buffer
  )
{
  SPRD_SDHCI_MMC_HOST  *Host;
  UINT32                BlockSize;
  UINT32                BlockCount;
  UINT32                Remaining;
  UINT8                *Destination;
  UINT32                Block;
  UINT32                Word;
  EFI_STATUS            Status;

  Host = (SPRD_SDHCI_MMC_HOST *)This;
  if (!Host->DataPending || (Buffer == NULL) || (Length == 0)) {
    return EFI_DEVICE_ERROR;
  }

  BlockSize  = (Length >= SDHCI_MAX_BLOCK_SIZE) ? SDHCI_MAX_BLOCK_SIZE : (UINT32)Length;
  BlockCount = ((UINT32)Length + BlockSize - 1) / BlockSize;
  Remaining  = (UINT32)Length;
  Destination = (UINT8 *)Buffer;

  Status = SdhciSendCommandNow (Host, Host->PendingFlags, Host->PendingArgument, (UINT32)Length, FALSE);
  Host->DataPending = FALSE;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Block = 0; Block < BlockCount; Block++) {
    UINT32  Words = BlockSize / 4;

    if (EFI_ERROR (SdhciWaitPresentState (Host, SDHCI_PS_BUF_RD_ENABLE, TRUE, SDHCI_TIMEOUT_MS))) {
      DEBUG ((DEBUG_ERROR, "SprdSdhci: buffer read enable timeout (block %u)\n", Block));
      return EFI_TIMEOUT;
    }

    for (Word = 0; Word < Words; Word++) {
      UINT32  Data = MmioRead32 (Host->Base + SDHCI_BUFFER);
      UINT32  Chunk = (Remaining < 4) ? Remaining : 4;

      if (Chunk == 0) {
        break;
      }

      CopyMem (Destination, &Data, Chunk);
      Destination += Chunk;
      Remaining   -= Chunk;
    }
  }

  return SdhciTransferComplete (Host);
}

STATIC
EFI_STATUS
EFIAPI
SprdSdhciWriteBlockData (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN EFI_LBA                 Lba,
  IN UINTN                   Length,
  IN UINT32                 *Buffer
  )
{
  SPRD_SDHCI_MMC_HOST  *Host;
  UINT32                BlockSize;
  UINT32                BlockCount;
  UINT32                Remaining;
  UINT8                *Source;
  UINT32                Block;
  UINT32                Word;
  EFI_STATUS            Status;

  Host = (SPRD_SDHCI_MMC_HOST *)This;
  if (!Host->DataPending || (Buffer == NULL) || (Length == 0)) {
    return EFI_DEVICE_ERROR;
  }

  BlockSize  = (Length >= SDHCI_MAX_BLOCK_SIZE) ? SDHCI_MAX_BLOCK_SIZE : (UINT32)Length;
  BlockCount = ((UINT32)Length + BlockSize - 1) / BlockSize;
  Remaining  = (UINT32)Length;
  Source     = (UINT8 *)Buffer;

  Status = SdhciSendCommandNow (Host, Host->PendingFlags, Host->PendingArgument, (UINT32)Length, TRUE);
  Host->DataPending = FALSE;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Block = 0; Block < BlockCount; Block++) {
    UINT32  Words = BlockSize / 4;

    if (EFI_ERROR (SdhciWaitPresentState (Host, SDHCI_PS_BUF_WR_ENABLE, TRUE, SDHCI_TIMEOUT_MS))) {
      DEBUG ((DEBUG_ERROR, "SprdSdhci: buffer write enable timeout (block %u)\n", Block));
      return EFI_TIMEOUT;
    }

    for (Word = 0; Word < Words; Word++) {
      UINT32  Data = 0;
      UINT32  Chunk = (Remaining < 4) ? Remaining : 4;

      if (Chunk == 0) {
        break;
      }

      CopyMem (&Data, Source, Chunk);
      MmioWrite32 (Host->Base + SDHCI_BUFFER, Data);
      Source    += Chunk;
      Remaining -= Chunk;
    }
  }

  return SdhciTransferComplete (Host);
}

STATIC
EFI_STATUS
EFIAPI
SprdSdhciSetIos (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN UINT32                  BusClockFreq,
  IN UINT32                  BusWidth,
  IN UINT32                  TimingMode
  )
{
  SPRD_SDHCI_MMC_HOST  *Host;
  UINT8                 HostControl;

  Host        = (SPRD_SDHCI_MMC_HOST *)This;
  HostControl = MmioRead8 (Host->Base + SDHCI_HOST_CONTROL);

  switch (BusWidth) {
    case 8:
      HostControl |= SDHCI_HC1_8BIT | SDHCI_HC1_4BIT;
      break;
    case 4:
      HostControl |= SDHCI_HC1_4BIT;
      HostControl &= (UINT8)~SDHCI_HC1_8BIT;
      break;
    default:
      HostControl &= (UINT8)~(SDHCI_HC1_8BIT | SDHCI_HC1_4BIT);
      break;
  }

  if ((TimingMode == EMMCHS26) || (TimingMode == EMMCHS52) || (TimingMode == EMMCHS52DDR1V8)) {
    HostControl |= SDHCI_HC1_HIGH_SPEED;
  } else {
    HostControl &= (UINT8)~SDHCI_HC1_HIGH_SPEED;
  }

  MmioWrite8 (Host->Base + SDHCI_HOST_CONTROL, HostControl);

  if (BusClockFreq != 0) {
    // Cap at 52 MHz: higher modes (HS200/HS400) need the Unisoc delay tuning
    // and 1.8 V I/O that this takeover driver does not program.
    if (BusClockFreq > 52000000) {
      BusClockFreq = 52000000;
    }

    if (EFI_ERROR (SdhciSetClock (Host, BusClockFreq / 1000))) {
      DEBUG ((DEBUG_ERROR, "SprdSdhci: clock %u Hz setup failed\n", BusClockFreq));
      return EFI_DEVICE_ERROR;
    }
  }


  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
SprdSdhciIsCardPresent (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  //
  // TODO(hardware): the Unisoc SDHCI block reports CAP base clock = 0, so the
  // standard SDHCI clock divider cannot be programmed and the card does not
  // answer after cboot hands over (the first hardware log showed CMD5/CMD8/
  // CMD1/CMD55 timeouts, then MmcDxe's identification retry loop stalled DXE
  // dispatch).  Until the Unisoc clock-gate/clock-source setup ("emmc-eb",
  // "emmc-2x-clk", ap-emmc-rtc-eb in the U-Boot clock table) is implemented,
  // report no card so the MMC stack stays idle and the rest of the firmware
  // can boot.  Flip back to TRUE together with the clock implementation.
  //
  return FALSE;
}

STATIC
BOOLEAN
EFIAPI
SprdSdhciIsReadOnly (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
SprdSdhciBuildDevicePath (
  IN EFI_MMC_HOST_PROTOCOL      *This,
  OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath
  )
{
  SPRD_SDHCI_MMC_HOST  *Host;
  MEMMAP_DEVICE_PATH   *Node;

  Host = (SPRD_SDHCI_MMC_HOST *)This;
  Node = AllocatePool (sizeof (MEMMAP_DEVICE_PATH));
  if (Node == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Node->Header.Type       = HARDWARE_DEVICE_PATH;
  Node->Header.SubType    = HW_MEMMAP_DP;
  Node->Header.Length[0]  = (UINT8)sizeof (MEMMAP_DEVICE_PATH);
  Node->Header.Length[1]  = (UINT8)(sizeof (MEMMAP_DEVICE_PATH) >> 8);
  Node->MemoryType        = EfiMemoryMappedIO;
  Node->StartingAddress   = Host->Base;
  Node->EndingAddress     = Host->Base + UMS512_EMMC_SIZE - 1;

  *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)Node;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SprdSdhciNotifyState (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_STATE               State
  )
{
  SPRD_SDHCI_MMC_HOST  *Host;

  Host = (SPRD_SDHCI_MMC_HOST *)This;
  if (State == MmcHwInitializationState) {
    return SdhciControllerInit (Host);
  }


  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
SprdSdhciIsMultiBlock (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  return TRUE;
}

STATIC
EFI_STATUS
SprdSdhciMmcHostInit (
  IN OUT SPRD_SDHCI_MMC_HOST  *Host
  )
{
  Host->Base          = UMS512_EMMC_BASE;
  Host->Capabilities  = MmioRead32 (Host->Base + SDHCI_CAPABILITIES);
  Host->BaseClockMhz  = (Host->Capabilities >> 8) & 0xFF;
  Host->Initialized   = FALSE;
  Host->DataPending   = FALSE;

  Host->Host.Revision      = MMC_HOST_PROTOCOL_REVISION;
  Host->Host.IsCardPresent = SprdSdhciIsCardPresent;
  Host->Host.IsReadOnly    = SprdSdhciIsReadOnly;
  Host->Host.BuildDevicePath = SprdSdhciBuildDevicePath;
  Host->Host.NotifyState   = SprdSdhciNotifyState;
  Host->Host.SendCommand   = SprdSdhciSendCommand;
  Host->Host.ReceiveResponse = SprdSdhciReceiveResponse;
  Host->Host.ReadBlockData = SprdSdhciReadBlockData;
  Host->Host.WriteBlockData = SprdSdhciWriteBlockData;
  Host->Host.SetIos        = SprdSdhciSetIos;
  Host->Host.IsMultiBlock  = SprdSdhciIsMultiBlock;

  DEBUG ((
    DEBUG_INFO,
    "SprdSdhci: eMMC host at 0x%lx CAP=0x%08x base=%u MHz\n",
    (UINT64)Host->Base,
    Host->Capabilities,
    Host->BaseClockMhz
    ));

  return EFI_SUCCESS;
}

STATIC VOID DbgGst (IN CONST CHAR8 *W) {
  DEBUG ((DEBUG_ERROR, "GSTDBG-sdhci[%a]: entries=%u table=0x%lx sig=0x%x\n", W,
          (UINT32)gST->NumberOfTableEntries, (UINT64)(UINTN)gST->ConfigurationTable, gST->Hdr.Signature));
}

EFI_STATUS
EFIAPI
SprdSdhciMmcHostEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  EFI_HANDLE           Handle;
  SPRD_SDHCI_MMC_HOST  *Host;

  DbgGst ("enter");
  Host = AllocateZeroPool (sizeof (SPRD_SDHCI_MMC_HOST));
  if (Host == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = SprdSdhciMmcHostInit (Host);
  if (EFI_ERROR (Status)) {
    FreePool (Host);
    return Status;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEmbeddedMmcHostProtocolGuid,
                  &Host->Host,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SprdSdhci: protocol install failed: %r\n", Status));
    FreePool (Host);
    return Status;
  }


  DbgGst ("exit");
  return EFI_SUCCESS;
}
