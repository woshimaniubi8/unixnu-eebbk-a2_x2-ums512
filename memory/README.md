# Native physical-memory ownership on BBK A2/X2

The firmware uses three different memory descriptions. They must remain
separate:

* The runtime cboot FDT describes the physical DRAM address span. On the tested
  4 GiB board this is `0x80000000 + 0xFFFFF000`, not an end address of
  `0xFFFFF000`. Native 16 KiB page alignment produces end `0x17FFFC000`.
* DarwinBoot needs one contiguous **256 MiB staging allocation** for the kernel,
  payload data and early page tables. This allocation is not the VM's memory
  capacity. Increasing the requested contiguous allocation to 4 GiB fails in
  a fragmented firmware map even when physical DRAM is present.
* XNU receives version 1 of `unixnu,physical-memory-map`: a 576-byte,
  little-endian structure containing at most 32 owned intervals. Ownership is
  final UEFI Conventional memory plus staging, minus the FDT reservation table,
  `/reserved-memory`, and the original display buffers. Reserved memory is not
  converted to free memory merely because it has no named DT node.

`install-memory-handoff.py` installs the checked-in DarwinBoot source, header
and libfdt dependency into the upstream build. The ADT generator option belongs
under `[payload.adt]`: `physical_memory_map = true`.

## Native-page and address constraints

Free intervals round inward to 16 KiB. Reservations round outward. In
particular, the original 4 KiB debug page at `0x100000000` withholds the whole
native page through `0x100004000`. The original framebuffer and writeback
span `0xFD708000..0xFFF80000`; ramoops is also withheld.

Assembly starts with the staging boot arguments. `bbk_memory_init()` validates
the ownership ABI before expanding the physical base and span. It preserves
the kernel VA-to-PA delta and all existing boot pointers. Early bootstrap
allocations remain bounded by staging.

Two assumptions in the original VM bootstrap need adjustment when expanding
the physical base below the kernel staging address:

1. `init_ptpages()` must walk aligned L1 slots. Incrementing an unaligned start
   by one L1 size misses the final partially covered slot. On this board the
   expanded base and kernel occupy slots 6 and 7. Missing slot 7 caused an early
   `phystokv: illegal PA` panic while mapping `__PRELINK_TEXT`.
2. `pmap_virtual_region()` must return disjoint reservations. The expanded
   static aperture crosses low globals. Reserving that aperture and then low
   globals again caused `kmem_init: vm_map_enter ... error 0x3`. The native
   board path reserves the static/low-global union once, plus VREGION1.

Physical holes retain `vm_pages` array slots because the native VM derives
physical page numbers from the array index. Hole slots are excluded from free
page counts and allocation. Removing the slots would silently change the PPNs
of all subsequent pages. Physical aperture mappings and `pa_valid()` also honor
the owned interval set.

CPU access to high DRAM does not imply that peripheral DMA accepts those
addresses. Both SDHCI transfer and descriptor allocations explicitly retain a
32-bit physical mask and validate their complete allocation below 4 GiB.

## Acceptance evidence

A successful boot prints an `M100 memory:` summary, all owned intervals, native
frame-slot accounting, and `M100 dma: ... below4G=1` for both block devices.
The free-plus-wired page total must equal the reported managed byte count.
The tested map owns about 3.48 GiB; factory carve-outs and firmware allocations
account for the rest of the 4 GiB physical span. This is not a promise of 4 GiB
of free RAM.

Check actual execution of launchd, SpringBoard and backboardd, display frame
submissions, and sustained operation without memory or storage panics. A
firmware memory-map total alone is not acceptance evidence. Boot smoke tests
also do not substitute for a full physical-memory pattern stress test.
