# SdhciBlock — Unisoc UMS512 SDHCR11 eMMC block transport

XNU kernel component that publishes the Android **userdata** GPT partition of
the UMS512 (SharkL5Pro / T610 family) eMMC as a read-only IOKit block device.

* controller: SDHCR11 at `0x71400000`, GIC SPI 10, 8-bit non-removable eMMC
* transport: ADMA2, polled (the controller interrupt is never enabled)
* published device: GPT partition `userdata`, 512-byte blocks, read-only
* runtime class: `NativeSdhciBlock` (catalog id `SdhciBlock`)

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
has no interrupt handler for this controller.

## Boot log

On hardware the driver prints, in this order:

```
NativeSdhciBlock: SDHCR11 phys=0x71400000 CAP=0x1e6d0080 version=0x... clk=0x0207 hc=0x20 hc2=0x3000 vendor204=0x30303060 ps=0x...
NativeSdhciBlock: EXT_CSD rev=0x08 sec_count=0xe8f8000 (119280 MiB)
NativeSdhciBlock: GPT first=0x22 last=0xe8f7fde alt=0xe8f7fff entries@0x2 count=128 size=128
NativeSdhciBlock: GPT [44] userdata first=0x132f800 last=0xe8f7fde blocks=0xd5c87df (109456 MiB)
NativeSdhciBlock: userdata first=0x132f800 last=0xe8f7fde sectors=0xd5c87df bytes=...
NativeSdhciBlock: block device online, 0xd5c87df blocks of 512 bytes
```

## Integration

* `components.toml` catalog entry `SdhciBlock` (`build-arm-driver.py SdhciBlock`,
  output `build/sdhciblock/libSdhciBlock.a`); the device repository carries it as
  `components/SdhciBlock.toml` and the macOS workflow appends it to the UniXNU
  checkout.
* `board.toml [components] include` adds `SdhciBlock`.
* `patches/xnu/sdhci-block-personality.patch` registers the `NativeSdhciBlock`
  personality (`IOProviderClass = IOPlatformDevice`,
  `IONameMatch = 'unixnu,sdhci-block'`).
* `dts/bbk-a2.dts` adds `mmc@71400000` with
  `compatible = "unixnu,sdhci-block", "sprd,sdhc-r11p1"`,
  `unixnu,adt-name = "sdhci"`, `reg = <0 0x71400000 0 0x1000>`,
  `interrupts = <0 10 4>`.

## Read-only

Writes are not implemented: `reportWriteProtection` returns true and
`doAsyncReadWrite` rejects anything that is not a read. The userdata root layout
the OS image carries is written from the host, not by the kernel.
