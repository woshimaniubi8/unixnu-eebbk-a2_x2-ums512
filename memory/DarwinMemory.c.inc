// SPDX-License-Identifier: BSD-2-Clause-Patent
#define BBK_U64 UINT64
#define BBK_U32 UINT32
#include "BbkMemoryMap.h"
#undef BBK_U64
#undef BBK_U32
STATIC_ASSERT (sizeof (BbkMemoryMap) == 576, "physical memory handoff ABI");

// This board's cboot hands its final, fixed-up FDT at82000000. Do not use
// the injected payload DTS's static /memory template in its place.
STATIC CONST VOID *mBbkFirmwareFdt = (CONST VOID *)(UINTN)0x82000000ULL;

STATIC UINT64 BbkFdtCells (CONST UINT32 *Cells, INT32 Count) {
  UINT64 Value = 0;
  for (INT32 I = 0; I < Count; ++I) { Value = (Value << 32) | Fdt32ToCpu (Cells[I]); }
  return Value;
}

STATIC EFI_STATUS BbkReadPhysicalMemory (BbkMemoryMap *Memory) {
  INT32 Node, Length, AddressCells, SizeCells;
  CONST CHAR8 *Type;
  CONST UINT32 *Reg;
  UINT64 Start, Bytes;
  UINTN Found = 0;
  ZeroMem (Memory, sizeof (*Memory));
  if ((FdtCheckHeader (mBbkFirmwareFdt) != 0) ||
      (FdtTotalSize (mBbkFirmwareFdt) > SIZE_1MB)) { return EFI_COMPROMISED_DATA; }
  AddressCells = FdtAddressCells (mBbkFirmwareFdt, 0);
  SizeCells = FdtSizeCells (mBbkFirmwareFdt, 0);
  if ((AddressCells != 2) || (SizeCells != 2)) { return EFI_UNSUPPORTED; }
  for (Node = -1; (Node = FdtNextNode (mBbkFirmwareFdt, Node, NULL)) >= 0;) {
    Type = FdtGetProp (mBbkFirmwareFdt, Node, "device_type", &Length);
    if ((Type == NULL) || (Length != 7) || (CompareMem (Type, "memory", 7) != 0)) { continue; }
    Reg = FdtGetProp (mBbkFirmwareFdt, Node, "reg", &Length);
    if ((Reg == NULL) || (Length != 16) || (++Found != 1)) { return EFI_UNSUPPORTED; }
    Start = BbkFdtCells (Reg, 2); Bytes = BbkFdtCells (Reg + 2, 2);
    if ((Start != 0x80000000ULL) || (Bytes < 0xF0000000ULL) ||
        (Bytes > SIZE_4GB) || (Bytes > MAX_UINT64 - Start)) { return EFI_COMPROMISED_DATA; }
    Memory->physical_start = Start;
    Memory->physical_end = bbkMemoryFloor (Start + Bytes);
  }
  if (Found != 1) { return EFI_NOT_FOUND; }
  Memory->magic = BBK_MEMORY_MAGIC;
  Memory->version = BBK_MEMORY_VERSION;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS BbkExcludeFirmwareReservations (BbkMemoryMap *Memory) {
  CONST UINT8 *Fdt = mBbkFirmwareFdt;
  UINT32 Total = FdtTotalSize (Fdt);
  UINT32 At = Fdt32ToCpu (ReadUnaligned32 ((CONST UINT32 *)(Fdt + 16)));
  UINT64 Start, Bytes;
  INT32 Node = -1, Depth = -1, ReservedDepth = -1, Length;
  INT32 AddressCells = 2, SizeCells = 2;
  CONST UINT32 *Reg;
  CONST CHAR8 *Name, *Status;
  // The FDT header reservation table is separate from /reserved-memory.
  for (;;) {
    if ((At > Total) || (Total - At < 16)) { return EFI_COMPROMISED_DATA; }
    Start = BbkFdtCells ((CONST UINT32 *)(Fdt + At), 2);
    Bytes = BbkFdtCells ((CONST UINT32 *)(Fdt + At + 8), 2); At += 16;
    if ((Start == 0) && (Bytes == 0)) { break; }
    if ((Bytes == 0) || (Bytes > MAX_UINT64 - Start) ||
        !bbkMemoryExclude (Memory, Start, Start + Bytes)) { return EFI_COMPROMISED_DATA; }
  }
  while ((Node = FdtNextNode (Fdt, Node, &Depth)) >= 0) {
    Name = FdtGetName (Fdt, Node, NULL);
    if ((ReservedDepth >= 0) && (Depth <= ReservedDepth)) { ReservedDepth = -1; }
    if ((Depth == 1) && (Name != NULL) && (AsciiStrCmp (Name, "reserved-memory") == 0)) {
      ReservedDepth = Depth;
      AddressCells = FdtAddressCells (Fdt, Node); SizeCells = FdtSizeCells (Fdt, Node);
      if ((AddressCells < 1) || (AddressCells > 2) || (SizeCells < 1) || (SizeCells > 2)) { return EFI_UNSUPPORTED; }
      continue;
    }
    if ((ReservedDepth < 0) || (Depth != ReservedDepth + 1)) { continue; }
    Status = FdtGetProp (Fdt, Node, "status", &Length);
    if ((Status != NULL) && (Length == 9) && (CompareMem (Status, "disabled", 9) == 0)) { continue; }
    Reg = FdtGetProp (Fdt, Node, "reg", &Length);
    if ((Reg == NULL) || (Length <= 0) || (Length % ((AddressCells + SizeCells) * 4))) { return EFI_UNSUPPORTED; }
    for (INT32 Cell = 0; Cell < Length / 4; Cell += AddressCells + SizeCells) {
      Start = BbkFdtCells (Reg + Cell, AddressCells);
      Bytes = BbkFdtCells (Reg + Cell + AddressCells, SizeCells);
      if ((Bytes == 0) || (Bytes > MAX_UINT64 - Start) ||
          !bbkMemoryExclude (Memory, Start, Start + Bytes)) { return EFI_COMPROMISED_DATA; }
    }
  }
  // These genuine1h190 Android DTBO carve-outs were absent from the old
  // UEFI memory table. Keep them reserved until their DMA owners are retired.
  if (!bbkMemoryExclude (Memory, 0xFD708000ULL, 0xFFF80000ULL)) { return EFI_OUT_OF_RESOURCES; }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS BbkBuildPhysicalMemory (BbkMemoryMap *Memory,
  EFI_MEMORY_DESCRIPTOR *Map, UINTN MapSize, UINTN Stride) {
  EFI_MEMORY_DESCRIPTOR *D;
  EFI_STATUS Status;
  UINT64 End;
  Memory->count = 0; Memory->managed_bytes = 0;
  if ((Stride < sizeof (*D)) || (MapSize % Stride)) { return EFI_COMPROMISED_DATA; }
  // BootServices/Loader/Runtime allocations remain excluded. Only the
  // separately allocated staging window is owned by this kernel.
  for (UINTN At = 0; At < MapSize; At += Stride) {
    D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + At);
    if (D->Type != EfiConventionalMemory || !D->NumberOfPages) { continue; }
    if ((D->Attribute & EFI_MEMORY_RUNTIME) ||
        (D->NumberOfPages > (MAX_UINT64 - D->PhysicalStart) / EFI_PAGE_SIZE)) { return EFI_COMPROMISED_DATA; }
    End = D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages);
    if (!bbkMemoryAdd (Memory, D->PhysicalStart, End)) { return EFI_OUT_OF_RESOURCES; }
  }
  if (!bbkMemoryAdd (Memory, Memory->staging_start, Memory->staging_end)) { return EFI_OUT_OF_RESOURCES; }
  Status = BbkExcludeFirmwareReservations (Memory);
  if (EFI_ERROR (Status)) { return Status; }
  Memory->managed_bytes = bbkMemoryCount (Memory, Memory->physical_start, Memory->physical_end);
  if (!bbkMemoryValidate (Memory)) { return EFI_COMPROMISED_DATA; }
  return EFI_SUCCESS;
}
