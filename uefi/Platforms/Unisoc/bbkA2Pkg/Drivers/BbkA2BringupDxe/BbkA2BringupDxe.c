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
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/GraphicsOutput.h>

#define DEBUG_TAG "BBK-A2-BRINGUP: "

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
