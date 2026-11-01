# SdhciBlock — Unisoc UMS512 SDHCR11 eMMC transport

XNU kernel component for the UMS512 (SharkL5Pro / T610 family) eMMC controller.
It attaches to the device-tree node, inherits the controller state cboot left
behind, proves the eMMC read path from the kernel (EXT_CSD and the GPT) and
publishes the userdata root layout as a read-only IOKit block device.

* controller: SDHCR11 at `0x71400000`, GIC SPI 10, 8-bit non-removable eMMC
* transport: ADMA2, polled (the controller interrupt is never enabled)
* runtime class: `NativeSdhciBlock` (catalog id `SdhciBlock`)
* milestone 1: register file + eMMC read path, userdata window recorded
* milestone 2: publish the userdata window as an `IOBlockStorageDevice`
* milestone 3: publish the SHNGROOT `OS` window (GPT + APFS disk) instead of the
  raw partition, falling back to the whole partition for images without the
  manifest

## Why it inherits the bootloader state

cboot/U-Boot leaves the controller initialised:

| register | value |
| --- | --- |
| `0x2C` clock control | `0x0207` |
| `0x28` host control 1 | `0x20` |
| `0x3E` host control 2 | `0x3000` |
| `0x204` vendor clock | `0x30303060` |
| `0x40` capabilities | `0x1E6D0080` (base clock reads 0) |

The capabilities register reports a zero base clock, so the standard SDHCI
divider cannot be programmed and reprogramming timing/width is impossible. The
UEFI takeover driver (`SprdSdhciMmcHostDxe`, `Silicon/Unisoc/UMS512Pkg`) proved
on hardware that the inherited state plus the vendor command path below reads
the GPT (45 partitions) successfully, so this driver does the same.

## Command path

```
0x04 (16-bit) = (512 & 0xFFF) | 0x7000      block size, 4 KiB SDMA boundary
0x00 (32-bit) = 1                           SDHCR1.1 vendor block count
0x58 / 0x5C   = ADMA2 descriptor table      low / high 32 bits
0x08          = argument                    CMD17: sector LBA (not a byte offset)
0x0C (32-bit) = (index << 24) | 0x003A0000 | 0x0011
                command | RESP_48/CRC/index/data-present, DMA + data read
descriptor    = [ (length << 16) | 0x23 ][address low][address high][0]
completion    = 0x30 & 0x3 == 0x3 (command + transfer complete)
errors        = 0x32 (error interrupt), 0x54 (ADMA error)
```

`0x34`/`0x36` (status enable) are set to `0xFFFFFFFF` so the poll loop observes
command/transfer complete; `0x38`/`0x3A` (signal enable) stay `0` because XNU
has no interrupt handler for this controller.  A 32-bit write at `0x3A` would
also hit vendor register `0x3C`, so only the `0x34` and `0x38` windows are
written.

All GPT LBAs are 512-byte units: `userdata` is `first=0x132F800`,
`last=0xE8F7FDE`, i.e. `0xD5C87DF` sectors (114,773,966,336 bytes), and the
partition starts at byte offset `0x132F800 * 512 = 0x265F00000`.

## Boot log

On hardware the driver prints, in this order (values from the 62.log bring-up
boot):

```
NativeSdhciBlock: probe node=sdhci compatible=unixnu,sdhci-block sprd,sdhc-r11p1
NativeSdhciBlock: probe matched node sdhci
NativeSdhciBlock: start node=sdhci
NativeSdhciBlock: SDHCR11 phys=0x71400000 CAP=0x1e6d0080 version=0x0004 clk=0x0207 hc=0x30 hc2=0x3800 vendor204=0x30303060 ps=0x01f000f0
NativeSdhciBlock: EXT_CSD rev=0x08 sec_count=0xe8f8000 (119280 MiB)
NativeSdhciBlock: GPT first=0x22 last=0xe8f7fde alt=0xe8f7fff entries@0x2 count=128 size=128
NativeSdhciBlock: GPT [44] userdata first=0x132f800 last=0xe8f7fde blocks=0xd5c87df (109456 MiB)
NativeSdhciBlock: userdata first=0x132f800 last=0xe8f7fde sectors=0xd5c87df bytes=114773966336
NativeSdhciBlock: root layout SHNGROOT v1 entries=2 image_bytes=9382658048
NativeSdhciBlock: root layout entry 0 OS offset=0x100000 size=6077580800 block=512 flags=0x1
NativeSdhciBlock: window OS offset=0x100000 size=6077580800 blocks=0xb52043 -> base_lba=0x1330000
NativeSdhciBlock: block device online, 0xb52043 blocks of 512 bytes (OS window)
```

The SHNGROOT manifest lives at the start of the userdata partition: `"SHNGROOT"`,
`u32 version=1`, `u32 count`, `u64 image_bytes`, then 128-byte entries
`{ char name[32]; u64 offset; u64 size; u32 block_size; u32 flags; u8 sha256[32];
40 reserved }`.  The `OS` window is the GPT + APFS disk whose volume carries the
`root-matching` UUID, so the block device exposes that window
(`base_lba = userdata_first_lba + offset/512`, `blocks = size/512`).  An image
without the manifest (or without an `OS` entry) keeps publishing the whole
userdata partition and says so in the log.

## Milestone 2: publishing the block device

`IOBlockStorageDevice` and `IOMedia` live in IOStorageFamily, which is **not**
part of the XNU kernel link.  UniXNU binds those imports from the qualified
original kernelcache (`scripts/build-virtio-mmio-bindings.py` plus
`scripts/audit-storage-interface.py`, both keyed to the pinned iPadOS 17.7.11
kernelcache), and the driver archive is then force-loaded.  Adding
`IOBlockStorageDevice` as the base class before that binding exists fails the
kernel link with `undefined symbols ... IOBlockStorageDevice` (observed in CI run
41271591408).

The binding used here is derived from the pinned kernelcache instead of the
qualified audit (the audit additionally needs a live-device memory snapshot and
the KDK AES kext, which a CI runner cannot provide):

* `build/derive-sdhciblock-bindings2.py` (host tool, not shipped) decompresses
  the pinned iPadOS 17.7.11 kernelcache (sha256 `f18b4df8…`, the
  `[kernel.prelink].kernelcache_sha256` pin), locates IOStorageFamily's kext
  segments through `__PRELINK_INFO`, finds the 190-slot dispatch table in its
  `__DATA_CONST` extent and derives the vtable, metaclass, protected
  constructor and base destructor of `IOBlockStorageDevice`.  The result is
  committed as `storage-bindings.json` (provenance: kernelcache sha, prelink
  slide, prelinked-kernel sha).
* `scripts/build-sdhci-block.py` (device-owned, copied into the build tree)
  compiles this driver with the kernel's own compile record, reads the class
  vtable relocations to learn which `IOBlockStorageDevice` symbol sits in which
  slot, emits `.set` absolute bindings for every undefined IOStorageFamily
  import, compiles them into `original-bindings.o` and archives both objects as
  `libSdhciBlock.a`.  The defconfig force-loads that archive.
* The addresses stay valid because the archive is force-loaded at the end of
  `LD_KERNEL_ARCHIVES` (earlier kernel symbols keep their addresses) and the
  IOStorageFamily kext is placed by the prelink step at `original_va + slide`.

## Integration

* `components.toml` catalog entry `SdhciBlock` (build recipe
  `build-sdhci-block.py SdhciBlock`, output `build/sdhciblock/libSdhciBlock.a`);
  the device repository carries it as `components/SdhciBlock.toml` and the macOS
  workflow appends it to the UniXNU checkout.
* `board.toml [components] include` adds `SdhciBlock`, and `[kernel] archives`
  lists `component:SdhciBlock` (the arch profile lists its component archives
  explicitly, so the automatic plan does not add it).
* `patches/xnu/sdhci-block-personality.patch` registers the `NativeSdhciBlock`
  personality with `IOProviderClass = IOPlatformDevice` and no name filter:
  the first hardware boot (62.log) showed the driver was never instantiated with
  `IONameMatch = 'unixnu,sdhci-block'`, so the match is done in `probe()` on the
  ADT `compatible` list instead, and `probe()`/`start()` log the node name and
  the compatible list they were offered.  `IOProbeScore` is 1000.
* `dts/bbk-a2.dts` adds `mmc@71400000` with
  `compatible = "unixnu,sdhci-block", "sprd,sdhc-r11p1"`,
  `unixnu,adt-name = "sdhci"`, `reg = <0 0x71400000 0 0x1000>`,
  `interrupts = <0 10 4>`.
