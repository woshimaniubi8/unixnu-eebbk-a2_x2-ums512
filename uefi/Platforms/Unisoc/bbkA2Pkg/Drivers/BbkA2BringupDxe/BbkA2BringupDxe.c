/** @file
  Bring-up probe for the BBK/EEBBK A2 / X2 / X2P (Unisoc UMS512).

  At the end of DXE dispatch (gEfiEndOfDxeEventGroupGuid) this driver prints
  what the firmware actually sees:

    * the GOP mode that SprdGopDxe published (resolution, format, framebuffer
      base/size) - this is what the UniXNU menu and DarwinBoot will inherit;
    * the full UEFI memory map (type, start, end, attributes) plus the count of
      conventional descriptors and the free MiB - the UMS9621 port found its
      first DXE crash from exactly this dump;
    * a marker line "BBK-A2-BRINGUP: end of DXE dump complete" that proves the
      dump itself finished, which distinguishes "the firmware died before
      EndOfDxe" from "the firmware parked in BDS".

  Then, if PcdBbkA2ResetAtEndOfDxe is TRUE (the bring-up default), it issues
  PSCI SYSTEM_RESET through ResetSystemRuntimeDxe.  This is deliberate: a blind
  firmware that falls into BDS waits for a key press forever, which is
  indistinguishable from a hang and costs a physical power cycle to escape.
  Resetting brings the tablet back into Android on its own, where the ramoops
  log (and /sys/fs/pstore) can be read.

  Set PcdBbkA2ResetAtEndOfDxe to FALSE in bbkA2.dsc once a boot target is
  confirmed to come up.

  This file is a reduced rewrite of the UMS9621 port's E5BringupDxe
  (_repos/e5-xnu, BSD-2-Clause-Patent); the heavy E5-specific probing
  (ring-resume snapshots, Shell launch hooks, GST scrub) is intentionally left
  out of the skeleton.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/FirmwareVolume2.h>

#define DEBUG_TAG "BBK-A2-BRINGUP: "

STATIC VOID RestoreConfigTableInfo (VOID);
STATIC VOID SaveConfigTableInfo (VOID);

//
// Upper bound for the memory-map dump.  The UMS9621 port saw 41 descriptors
// with a 73 MiB heap; the UMS512 map in MemoryMapLib is ~40 entries, so 64 is
// a safe display cap (the summary counters still cover all of them).
//
#define MAX_DUMP_DESCRIPTORS  64

STATIC
VOID
DumpGop (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, DEBUG_TAG "no GOP (%r); the menu will be serial-only\n", Status));
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    DEBUG_TAG "GOP mode %u/%u %ux%u format %u frame buffer 0x%lx + 0x%lx\n",
    Gop->Mode->Mode,
    Gop->Mode->MaxMode,
    Gop->Mode->Info->HorizontalResolution,
    Gop->Mode->Info->VerticalResolution,
    Gop->Mode->Info->PixelFormat,
    Gop->Mode->FrameBufferBase,
    Gop->Mode->FrameBufferSize
    ));
}

STATIC
VOID
DumpMemoryMap (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  UINTN                  Count;
  UINTN                  Index;
  UINTN                  Conventional;
  UINT64                 FreePages;
  CONST CHAR8            *TypeName;

  MapSize = 0;
  Status  = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "GetMemoryMap sizing failed: %r\n", Status));
    return;
  }

  //
  // GetMemoryMap may need more room after the allocation itself changes the
  // map, so add slack and retry once.
  //
  MapSize += 8 * DescriptorSize;
  Map = AllocatePool (MapSize);
  if (Map == NULL) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "out of memory for the memory map\n"));
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "GetMemoryMap failed: %r\n", Status));
    FreePool (Map);
    return;
  }

  Count        = MapSize / DescriptorSize;
  Conventional = 0;
  FreePages    = 0;

  for (Index = 0; Index < Count; Index++) {
    EFI_MEMORY_DESCRIPTOR  *Entry;

    Entry = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescriptorSize);

    switch (Entry->Type) {
      case EfiConventionalMemory:
        TypeName = "Conventional";
        Conventional++;
        FreePages += Entry->NumberOfPages;
        break;
      case EfiBootServicesCode:
        TypeName = "BsCode";
        break;
      case EfiBootServicesData:
        TypeName = "BsData";
        break;
      case EfiRuntimeServicesCode:
        TypeName = "RtCode";
        break;
      case EfiRuntimeServicesData:
        TypeName = "RtData";
        break;
      case EfiLoaderCode:
        TypeName = "LdrCode";
        break;
      case EfiLoaderData:
        TypeName = "LdrData";
        break;
      case EfiACPIReclaimMemory:
        TypeName = "ACPIReclaim";
        break;
      case EfiACPIMemoryNVS:
        TypeName = "ACPINVS";
        break;
      case EfiReservedMemoryType:
        TypeName = "Reserved";
        break;
      default:
        TypeName = "Other";
        break;
    }

    if (Index < MAX_DUMP_DESCRIPTORS) {
      DEBUG ((
        DEBUG_INFO,
        DEBUG_TAG "map[%02u] %a 0x%016lx-0x%016lx attr 0x%lx\n",
        (UINT32)Index,
        TypeName,
        Entry->PhysicalStart,
        Entry->PhysicalStart + EFI_PAGES_TO_SIZE (Entry->NumberOfPages),
        Entry->Attribute
        ));
    }
  }

  DEBUG ((
    DEBUG_INFO,
    DEBUG_TAG "%u descriptors, %u conventional, %lu MiB free\n",
    (UINT32)Count,
    (UINT32)Conventional,
    (FreePages * EFI_PAGE_SIZE) / (1024 * 1024)
    ));

  FreePool (Map);
}


//
// UniXNU.efi file GUID (package/UniXNUPkg/Application/UniXNU/UniXNU.inf).
//
STATIC CHAR16  mFirmwareVendor[] = L"BBK";

STATIC CONST EFI_GUID  mUnixnuFileGuid = {
  0x9B665204, 0xA133, 0x47A2, { 0x93, 0x96, 0x9E, 0x2B, 0x37, 0x0A, 0x4E, 0x25 }
};

/**
  Load UniXNU.efi from the firmware volume and run it.

  The BDS Boot#### path crashes on this firmware (BDS dereferences a bad
  pointer while processing the boot option), so the bring-up driver starts the
  application itself at EndOfDxe.  When UniXNU returns, BDS continues with its
  normal (empty) boot order.
**/
STATIC
VOID
LaunchUnixnu (
  VOID
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *Handles;
  UINTN                          HandleCount;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *Fv;
  UINT32                         AuthStatus;
  VOID                           *FileBuffer;
  UINTN                          FileSize;
  EFI_HANDLE                     ImageHandle;
  UINTN                          ExitDataSize;
  CHAR16                         *ExitData;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_DEVICE_PATH_PROTOCOL       *FvPath;
  EFI_DEVICE_PATH_PROTOCOL       *Path;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiFirmwareVolume2ProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "UniXNU: no firmware volumes (%r)\n", Status));
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv);
    if (EFI_ERROR (Status)) {
      continue;
    }

    FileBuffer = NULL;
    FileSize   = 0;
    //
    // ReadSection(PE32) instead of ReadFile: ReadFile returns the FFS file
    // data with the section header in front, which LoadImage rejects as an
    // unsupported image.  ReadSection gives the raw PE32 image.
    //
    Status = Fv->ReadSection (
                   Fv,
                   &mUnixnuFileGuid,
                   EFI_SECTION_PE32,
                   0,
                   &FileBuffer,
                   &FileSize,
                   &AuthStatus
                   );
    if (EFI_ERROR (Status) || (FileBuffer == NULL)) {
      continue;
    }

    DEBUG ((DEBUG_INFO, DEBUG_TAG "UniXNU: found %lu bytes in FV %u, starting it\n", (UINT64)FileSize, (UINT32)Index));

    //
    // LoadImage with a source buffer still needs a valid device path; build
    // "<FV device path>/FvFile(UniXNU.efi)".  If the FV handle has no device
    // path, fall back to a standalone FvFile node plus end node.
    //
    EfiInitializeFwVolDevicepathNode (&FileNode, &mUnixnuFileGuid);
    FvPath = DevicePathFromHandle (Handles[Index]);
    if (FvPath != NULL) {
      Path = AppendDevicePathNode (FvPath, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
    } else {
      Path = AllocatePool (sizeof (FileNode) + END_DEVICE_PATH_LENGTH);
      if (Path != NULL) {
        CopyMem (Path, &FileNode, sizeof (FileNode));
        SetDevicePathEndNode ((EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Path + sizeof (FileNode)));
      }
    }

    if (Path == NULL) {
      DEBUG ((DEBUG_ERROR, DEBUG_TAG "UniXNU: cannot build device path\n"));
      FreePool (FileBuffer);
      break;
    }

    ImageHandle  = NULL;
    ExitDataSize = 0;
    ExitData     = NULL;
    Status       = gBS->LoadImage (TRUE, gImageHandle, Path, FileBuffer, FileSize, &ImageHandle);
    if (!EFI_ERROR (Status)) {
      Status = gBS->StartImage (ImageHandle, &ExitDataSize, &ExitData);
      DEBUG ((DEBUG_INFO, DEBUG_TAG "UniXNU: returned %r\n", Status));
    } else {
      DEBUG ((DEBUG_ERROR, DEBUG_TAG "UniXNU: LoadImage failed %r\n", Status));
    }

    FreePool (Path);
    FreePool (FileBuffer);
    break;
  }

  if (Index == HandleCount) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "UniXNU: application file not found in any FV\n"));
  }

  FreePool (Handles);
}


/**
  gST->FirmwareVendor is observed to be a garbage value on this firmware
  (0x6C617470 in the first UniXNU run), which makes UniXNU's very first log
  line dereference it through "%s".  Repair it (and the table CRC) before
  starting anything that may print it.
**/
STATIC
VOID
FixupSystemTable (
  VOID
  )
{
  UINTN  Ptr;

  RestoreConfigTableInfo ();

  Ptr = (UINTN)gST->FirmwareVendor;
  DEBUG ((DEBUG_ERROR, "BBK-A2-DBG: gST->FirmwareVendor=0x%lx\n", (UINT64)Ptr));
  if ((Ptr < 0x80000000ULL) || (Ptr >= 0xFFFF0000ULL)) {
    gST->FirmwareVendor = mFirmwareVendor;
    gST->Hdr.CRC32      = 0;
    gBS->CalculateCrc32 ((VOID *)gST, sizeof (EFI_SYSTEM_TABLE), &gST->Hdr.CRC32);
    DEBUG ((DEBUG_ERROR, "BBK-A2-DBG: gST->FirmwareVendor repaired to 0x%lx\n",
            (UINT64)(UINTN)gST->FirmwareVendor));
  }
}


/**
  Connect every controller handle.

  Our payload RAM disk is published during DXE dispatch, but its FAT driver
  only binds when the controller is connected.  BDS normally does this in
  EfiBootManagerConnectAll() *after* the EndOfDxe event, and we start UniXNU
  from the EndOfDxe callback, so do the connect pass ourselves first.
**/

STATIC CONST EFI_GUID  mPayloadVendorGuid = {
  0x5D3E8A17, 0x42C6, 0x4B0F, { 0x9D, 0x85, 0x6A, 0x1C, 0x7E, 0x93, 0xB2, 0x04 }
};

/**
  Connect only the payload RAM-disk handle (identified by the vendor device
  path node BbkA2PayloadDxe installs), so the FAT driver binds to it and its
  SimpleFileSystem becomes visible to UniXNU.  Connecting *all* handles here
  pulls in image-verification drivers and makes the later LoadImage of
  UniXNU.efi fault inside DxeCore.
**/
STATIC
VOID
ConnectPayload (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     Index;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  VENDOR_DEVICE_PATH        *Vendor;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "no block io handles (%r)\n", Status));
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    DevicePath = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&DevicePath)) ||
        (DevicePath == NULL) ||
        (DevicePathType (DevicePath) != HARDWARE_DEVICE_PATH) ||
        (DevicePathSubType (DevicePath) != HW_VENDOR_DP))
    {
      continue;
    }

    Vendor = (VENDOR_DEVICE_PATH *)DevicePath;
    if (!CompareGuid (&Vendor->Guid, &mPayloadVendorGuid)) {
      continue;
    }

    DEBUG ((DEBUG_ERROR, DEBUG_TAG "connecting payload block device %u\n", (UINT32)Index));
    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
  }

  FreePool (Handles);
}


STATIC
VOID
DumpPcds (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, DEBUG_TAG "PCD: EmuVarReserved=0x%lx VarStoreSize=0x%x MaxVarSize=0x%x FwRev=0x%x\n",
          (UINT64)PcdGet64 (PcdEmuVariableNvStoreReserved),
          (UINT32)PcdGet32 (PcdVariableStoreSize),
          (UINT32)PcdGet32 (PcdMaxVariableSize),
          (UINT32)PcdGet32 (PcdFirmwareRevision)));
}
STATIC
VOID
ProbeHighRam (VOID) {
  STATIC CONST UINT64 Addrs[] = { 0xA0000000ULL, 0xA4000000ULL, 0xA8000000ULL, 0xB0000000ULL, 0xBFF00000ULL };
  UINTN  Index;
  UINT64 Ok = 0, Bad = 0;
  for (Index = 0; Index < ARRAY_SIZE (Addrs); Index++) {
    volatile UINT64 *P = (volatile UINT64 *)(UINTN)Addrs[Index];
    *P = 0x55AA112233445566ULL;
    if (*P == 0x55AA112233445566ULL) { Ok++; } else { Bad++; }
  }
  DEBUG ((DEBUG_ERROR, DEBUG_TAG "high DRAM probe: ok=%lu bad=%lu\n", Ok, Bad));
}

STATIC UINTN  mSavedTableEntries;
STATIC VOID   *mSavedTable;

STATIC
VOID
SaveConfigTableInfo (
  VOID
  )
{
  mSavedTableEntries = gST->NumberOfTableEntries;
  mSavedTable        = gST->ConfigurationTable;
}

STATIC
VOID
RestoreConfigTableInfo (
  VOID
  )
{
  if ((mSavedTableEntries != 0) && (mSavedTable != NULL) &&
      ((gST->NumberOfTableEntries != mSavedTableEntries) || (gST->ConfigurationTable != mSavedTable)))
  {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "restoring gST config table info: entries %u -> %u, table 0x%lx -> 0x%lx\n",
            (UINT32)gST->NumberOfTableEntries, (UINT32)mSavedTableEntries,
            (UINT64)(UINTN)gST->ConfigurationTable, (UINT64)(UINTN)mSavedTable));
    gST->NumberOfTableEntries = mSavedTableEntries;
    gST->ConfigurationTable   = mSavedTable;
  }
}

STATIC
VOID
DumpConfigTables (
  VOID
  )
{
  UINTN  Index;

  DEBUG ((DEBUG_ERROR, DEBUG_TAG "gST: entries=%u table=0x%lx vendor=0x%lx rev=0x%x\n",
          (UINT32)gST->NumberOfTableEntries,
          (UINT64)(UINTN)gST->ConfigurationTable,
          (UINT64)(UINTN)gST->FirmwareVendor,
          gST->FirmwareRevision));

  for (Index = 0; (Index < gST->NumberOfTableEntries) && (Index < 32); Index++) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "  cfg[%u] guid=0x%lx table=0x%lx\n",
            (UINT32)Index,
            (UINT64)(UINTN)&gST->ConfigurationTable[Index].VendorGuid,
            (UINT64)(UINTN)gST->ConfigurationTable[Index].VendorTable));
  }
}

STATIC
VOID
EFIAPI
BbkA2LaunchOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DEBUG ((DEBUG_ERROR, DEBUG_TAG "ReadyToBoot: launching UniXNU\n"));
  DumpConfigTables ();

  //
  // The ReadyToBoot notify runs at TPL_CALLBACK.  Drop to TPL_APPLICATION for
  // the driver-model connect pass (ConnectController) and for starting the
  // application, then restore the callback TPL before returning.
  //
  ConnectPayload ();
  LaunchUnixnu ();
}

STATIC
VOID
EFIAPI
BbkA2OnEndOfDxe (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DEBUG ((DEBUG_INFO, DEBUG_TAG "end of DXE reached\n"));

  DumpGop ();
  DumpMemoryMap ();

  DEBUG ((DEBUG_INFO, DEBUG_TAG "===== end of DXE dump complete =====\n"));

  FixupSystemTable ();
  {
    EFI_EVENT  ReadyEvent;

    ReadyEvent = NULL;
    if (!EFI_ERROR (gBS->CreateEventEx (
                           EVT_NOTIFY_SIGNAL,
                           TPL_CALLBACK,
                           BbkA2LaunchOnReadyToBoot,
                           NULL,
                           &gEfiEventReadyToBootGuid,
                           &ReadyEvent
                           )))
    {
      DEBUG ((DEBUG_ERROR, DEBUG_TAG "UniXNU launch deferred to ReadyToBoot\n"));
    }
  }

  if (FixedPcdGetBool (PcdBbkA2ResetAtEndOfDxe)) {
    DEBUG ((DEBUG_INFO, DEBUG_TAG "resetting to hand control back to cboot\n"));
    //
    // EfiResetWarm maps to PSCI SYSTEM_RESET through ArmPsciResetSystemLib.
    // cboot then consumes one boot attempt and falls back to Android, which
    // is what makes the ramoops log readable without touching the tablet.
    //
    gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);
  }
}

EFI_STATUS
EFIAPI
BbkA2BringupInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   EndOfDxeEvent;

  DEBUG ((DEBUG_INFO, DEBUG_TAG "driver loaded\n"));
  SaveConfigTableInfo ();
  DumpPcds ();
  ProbeHighRam ();
  DumpConfigTables ();
  FixupSystemTable ();

  //
  // Print the GOP early as well: if the firmware dies between here and
  // EndOfDxe, the early line still tells us a display was published.
  //
  DumpGop ();

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  BbkA2OnEndOfDxe,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &EndOfDxeEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "CreateEventEx(EndOfDxe) failed: %r\n", Status));
  }

  return EFI_SUCCESS;
}
