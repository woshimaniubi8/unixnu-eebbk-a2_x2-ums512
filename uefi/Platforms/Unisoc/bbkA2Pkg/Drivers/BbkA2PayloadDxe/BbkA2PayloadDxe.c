/** @file
  Publish the Android boot image ramdisk as a read-only block device.

  cboot (U-Boot 2015.07) places the boot image ramdisk in DRAM and is expected
  to record both ends in the device tree it hands over in x0
  (/chosen linux,initrd-start and linux,initrd-end).  TODO(hardware): confirm
  cboot sets these properties; if it does not, the appended-payload fallback
  below (which needs a BootShim rescue that is not implemented yet) is the
  alternative.
  The UniXNU payload is a FAT image inside that ramdisk, so publishing a block
  device over it is what turns the payload into a normal UEFI volume that
  DarwinBoot can read the kernel and its .adt from.

  The ramdisk load address on UMS512 is not measured yet.  cboot's boot code
  loads the kernel to 0x80080000 and passes x0 = 0x82000000 (the merged FDT);
  the ramdisk address has to be confirmed on hardware.  The MemoryMapLib entry
  named "Initrd" reserves the window 0x82100000..0x87500000 so the MMU covers
  both the FDT-adjacent and the higher candidate; the exact bounds always come
  from /chosen at run time, never from this file.

  NOT WIRED IN YET: bbkA2.dsc and bbkA2.fdf gain this driver only after the DXE core
  is confirmed running, because a fault during dispatch would hide the
  EndOfDxe marker that BbkA2BringupDxe prints.
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/BlockIo.h>

//
// EDK2's LzmaCustomDecompressLib exposes these two entry points but ships no
// public header for them; declare them here and pull the library in through a
// NULL library class in the INF.
//
extern
RETURN_STATUS
EFIAPI
LzmaUefiDecompressGetInfo (
  IN  CONST VOID  *Source,
  IN  UINT32      SourceSize,
  OUT UINT32      *DestinationSize,
  OUT UINT32      *ScratchSize
  );

extern
RETURN_STATUS
EFIAPI
LzmaUefiDecompress (
  IN CONST VOID  *Source,
  IN UINTN       SourceSize,
  IN OUT VOID    *Destination,
  IN OUT VOID    *Scratch
  );
#include <Protocol/DevicePath.h>

#define RAM_BLOCK_SIZE  512
#define DEBUG_TAG       "BBKA2PAYLOAD: "

STATIC VOID DumpGst (IN CONST CHAR8 *Where);

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0x5D3E8A17, 0x42C6, 0x4B0F, { 0x9D, 0x85, 0x6A, 0x1C, 0x7E, 0x93, 0xB2, 0x04 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8               *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA   mMedia;

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % RAM_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

STATIC
VOID
PublishRamDisk (
  IN UINT8  *Base,
  IN UINTN  Size
  );

STATIC
VOID
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  INT32        Chosen;
  CONST VOID   *StartProp;
  CONST VOID   *EndProp;
  INT32        StartLength;
  INT32        EndLength;
  UINT64       Start;
  UINT64       End;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "no /chosen in the handed over tree\n"));
    return;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "no linux,initrd-start/-end; the boot image carried no ramdisk\n"));
    return;
  }

  Start = ReadCells (StartProp, (UINT32)(StartLength / 4));
  End   = ReadCells (EndProp, (UINT32)(EndLength / 4));
  DEBUG ((DEBUG_ERROR, DEBUG_TAG "initrd 0x%lx-0x%lx (%lu bytes)\n", Start, End, End - Start));

  if ((End <= Start) || ((End - Start) < RAM_BLOCK_SIZE)) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "initrd is smaller than one sector\n"));
    return;
  }

  if (((End - Start) % RAM_BLOCK_SIZE) != 0) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "initrd tail %u bytes beyond the last whole sector, rounded down\n",
            (UINT32)((End - Start) % RAM_BLOCK_SIZE)));
  }

  //
  // The boot image can carry an LZMA-alone compressed payload image instead of
  // the stock Android ramdisk.  The uncompressed payload (64 MiB FAT image) is
  // expanded into the reserved Initrd window at 0x83000000 and published as a
  // RAM disk, which the FAT driver then mounts for UniXNU.
  //
  if (((End - Start) > 0x100000) &&
      (((UINT8 *)(UINTN)Start)[0] == 0x5D) &&
      (((UINT8 *)(UINTN)Start)[1] == 0x00))
  {
    EFI_STATUS  Status;
    UINT32      DestSize;
    UINT32      ScratchSize;
    VOID        *Scratch;

    DestSize    = 0;
    ScratchSize = 0;
    Status = LzmaUefiDecompressGetInfo (
               (CONST VOID *)(UINTN)Start,
               (UINT32)(End - Start),
               &DestSize,
               &ScratchSize
               );
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "LZMA payload: size %u scratch %u (%r)\n", DestSize, ScratchSize, Status));
    if (!EFI_ERROR (Status) && (DestSize != 0) && (DestSize <= 0x05000000)) {
      Scratch = AllocatePool (ScratchSize);
      if (Scratch != NULL) {
        Status = LzmaUefiDecompress (
                   (CONST VOID *)(UINTN)Start,
                   (UINTN)(End - Start),
                   (VOID *)(UINTN)0x83000000,
                   Scratch
                   );
        FreePool (Scratch);
        DEBUG ((DEBUG_ERROR, DEBUG_TAG "LZMA payload decompressed: %r\n", Status));
        if (!EFI_ERROR (Status)) {
          UINT8  *P = (UINT8 *)(UINTN)0x83000000;

          {
            CHAR8  Label[12];

            CopyMem (Label, P + 71, 11);
            Label[11] = 0;
            DEBUG ((DEBUG_ERROR, DEBUG_TAG "payload head %02x %02x %02x %02x %02x %02x %02x %02x, label '%a'\n",
                    P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7], Label));
          }
          DEBUG ((DEBUG_ERROR, DEBUG_TAG "payload bps=%u spc=%u fatsz=%u rootclus=%u total=%u\n",
                  (UINT32)(P[11] | (P[12] << 8)), (UINT32)P[13],
                  (UINT32)(P[36] | (P[37] << 8) | (P[38] << 16) | (P[39] << 24)),
                  (UINT32)(P[44] | (P[45] << 8) | (P[46] << 16) | (P[47] << 24)),
                  (UINT32)(P[32] | (P[33] << 8) | (P[34] << 16) | (P[35] << 24))));
          PublishRamDisk (P, DestSize);
          DEBUG ((DEBUG_ERROR, DEBUG_TAG "published media block=%u last=%lu\n",
                  (UINT32)mMedia.BlockSize, (UINT64)mMedia.LastBlock));
          DumpGst ("payload-done");
          return;
        }
      }
    }
  }

  PublishRamDisk ((UINT8 *)(UINTN)Start, (UINTN)(End - Start));
}

STATIC
VOID
PublishRamDisk (
  IN UINT8  *Base,
  IN UINTN  Size
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  mRamBase                = Base;
  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = Size / RAM_BLOCK_SIZE - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  DEBUG ((DEBUG_ERROR, DEBUG_TAG "ramdisk block device published: %r\n", Status));
}

STATIC
VOID
DumpGst (
  IN CONST CHAR8  *Where
  )
{
  DEBUG ((DEBUG_ERROR, DEBUG_TAG "gST[%a]: entries=%u table=0x%lx sig=0x%x\n",
          Where, (UINT32)gST->NumberOfTableEntries,
          (UINT64)(UINTN)gST->ConfigurationTable, gST->Hdr.Signature));
}

EFI_STATUS
EFIAPI
BbkA2PayloadInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  UINT64             Fdt;

  Hob = GetFirstGuidHob (&gSiliciumEntryArgHobGuid);
  if (Hob == NULL) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "SEC left no entry-argument HOB\n"));
    return EFI_SUCCESS;
  }

  Fdt = *(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if ((Fdt == 0) || ((Fdt & 3) != 0) || (FdtCheckHeader ((VOID *)(UINTN)Fdt) != 0)) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "x0 0x%lx is not a device tree\n", Fdt));
    return EFI_SUCCESS;
  }

  DumpGst ("payload-enter");
  DEBUG ((DEBUG_ERROR, DEBUG_TAG "device tree at 0x%lx, %u bytes\n", Fdt, (UINT32)FdtTotalSize ((VOID *)(UINTN)Fdt)));

  //
  // Fallback source: a bare FAT volume that mk-boot-payload.py appended to
  // the boot blob behind a "BBK2" magic header.  TODO: BootShim does not rescue
  // this blob yet, so with the current shim the scan below finds nothing and
  // the /chosen initrd path is the only working one.  When the rescue is added
  // it should stage the blob at 0x95000000 (MemoryMapLib maps that window).
  //
  {
    UINT8   *Scan;
    BOOLEAN Found;

    Found = FALSE;
    for (Scan = (UINT8 *)0x95000000; Scan < (UINT8 *)0x9A000000; Scan += 4096) {
      if ((Scan[0] == 'B') && (Scan[1] == 'B') && (Scan[2] == 'K') && (Scan[3] == '2')) {
        UINT64  Size = ReadUnaligned64 ((UINT64 *)(Scan + 4));

        if ((Size >= RAM_BLOCK_SIZE) && (Size <= (0x05000000 - 12)) &&
            (((UINTN)Scan + 12 + Size) <= 0x9A000000))
        {
          DEBUG ((DEBUG_ERROR, DEBUG_TAG "BBK2 ramdisk at 0x%lx, %lu bytes\n", (UINT64)(UINTN)Scan, Size));
          PublishRamDisk (Scan + 12, (UINTN)Size);
          Found = TRUE;
          break;
        }
      }
    }

    if (Found) {
      return EFI_SUCCESS;
    }
  }

  //
  // Diagnostic: dump the raw bytes at the expected payload-tail magic
  // address.  If LK never loaded the 62.9 MB payload tail, this shows
  // stale DRAM instead of "BBK2".
  //
  {
    UINT64  *Mailbox = (UINT64 *)(UINTN)0x80470000;
    UINT64   P0 = Mailbox[0];
    UINT64   P1 = Mailbox[1];
    UINT64   P2 = Mailbox[2];
    UINT64   P3 = Mailbox[3];

    DEBUG ((DEBUG_ERROR, DEBUG_TAG "mailbox: tail=%016lx %016lx run=%016lx tag=%lu\n", P0, P1, P2, P3));
  }

  PublishInitrd ((VOID *)(UINTN)Fdt);
  return EFI_SUCCESS;
}
