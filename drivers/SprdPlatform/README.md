# EEBBK 1h190 platform hardware

This is a hardware port, not a set of Apple-compatible placeholder nodes.
Factory authority is the shipped 4.14.193 kernel (SHA256
`70a55f276551f0fedbbc3c36ad1950d85eab19352b6b0d4eb6e69ae737c98104`)
and its base DTB merged with DTBO #3 (`ums512_1h190`).

## Acquisition and qualification

- ADIv3 transactions: physical slave window `0x32120000`, original address echo
  comparison `read_data[30:16] == physical_address[16:2]`; bounded timeouts.
- AP GPIO124 active-low volume down. SC2730 EIC1 active-low power and EIC4
  active-high volume up. Normal press/release/debounce HID reports; no Home key.
- I2C3 `0x70600000` and I2C4 `0x70700000`, stock AP_APB gate bits12/13;
  26MHz source, 400kHz SCL, FIFO-bounded register transfers.
- Read-only identification of Goodix@14 / FocalTech@38 and BQ2560x@6b.
- Goodix GT9x frame parser and original IOHIDDevice digitizer delivery only after
  valid ID and controller-reported geometry. A FocalTech ACK alone is not a
  qualified IC version and does not enable guessed firmware/config writes.
- SC2730 FGU efuse calibration, qualified factory boot OCV and a coherent
  split-register coulomb counter. The battery frontend subclasses the original
  IOPMPowerSource, using UniXNU-sheng's normalized capacity interface. Publication
  requires calibrated measurements, the real EIC3 battery-present signal, a
  qualified capacity origin/counter and successful charger reads. No FGU counter
  initialization, user-area persistence or charger control register is written.

GPIO/PWM/ADI clock restoration changes only bits owned by this component. I2C
register accesses are serialized on the single platform work loop. The current
implementation polls input; wake-from-suspend requires the separate GIC/EIC
interrupt and power-management integration, and is not claimed here.

## Reused implementation sources

- Linux `spi-sprd-adi.c`, `gpio-sprd.c`, `gpio-pmic-eic-sprd.c`,
  `sc27xx_fuel_gauge.c`, `ums512-clk.c`, `goodix.c` / `goodix.h`, under their
  original GPL licenses; source snapshot `android15-6.6-2025-06_r15` from
  android.googlesource.com/kernel/common. I2C register protocol is dual MIT/GPL
  in `i2c-sprd.c` (Spreadtrum Communications, 2017).
- UniXNU `ShengHidInput/NativeHidInput.cpp` and `build-sheng-hid-input.py`, BSD
  2-Clause-Patent: original HID report descriptors, class/slot ABI and delayed
  metaclass registration. No Qualcomm register/firmware backend is reused.
- Native iPadOS 21H461 IOHIDFamily-2031.120.4, verified by the source/final-image
  ABI checker. No private object-field reinterpretation is used for input.

The host test compiles the same `SprdBus.h` used on-device. It exercises real
command construction and fault handling against deterministic register models;
this is not evidence that an untested physical controller already works.

## SC2355 and Mali hardware dependencies (M116)

The driver-integration reference is https://unixnu.com/zh/porting/drivers.
The radio and GPU transport backends in its examples belong to WCN7850 and
Adreno740. This board uses SC2355 on SDIO1 at0x71200000 and Mali Gondul at
0x60000000. The eMMC host0x71400000 is excluded from this radio operation.

`SprdWcnPower.h` follows the shipped4.14 `chip_power_on`, not GPIO polarity
guesses: CHIP_EN GPIO143 goes raw low/high and RESET GPIO97 goes raw high.
The selected32k clock is actually AUX0 (DT phandle80/index2), offset0x228,
parent ext32k index0, AON gate bit4. The SC2730 descriptor supplies are
VDDSIM2=1.8V, VDDWCN=1.2V and VDDWIFIPA=3.3V. Owned rail fields, WBREQ
binding bits, GPIO bits and clock fields are saved and restored on failure
and after the identification transaction. PMIC access runs through the
existing platform owner before its input/battery timer starts.

`SprdSdio.h` implements bounded CMD0/5/3/7/52/53, CCCR/function1/CIS parsing
and the original direct-memory window. The ROM vendor/device identity0/0
is deliberate (`sdiohal_ids` in the exact shipped kernel), so a zero ID is
not discarded. CP memory addresses go through function0 SYSADDR0..3 at
0x15c..0x15f and incrementing function1 port0x0f; STBBA is a different register
set. Both copied length and transfer completion are required, including when
FIFO readiness and completion arrive together. No DMA memory is exposed by
this initial byte-mode operation. Chip ID and strap reads are real SDIO
transactions, not CPU dereferences of CP addresses.

`SprdMali.h` controls only the factory GPU domain and owned gate/mux fields,
using the real ext26m parent for identification. It checks the already-on
DCDC_GPU supply, waits for the original domain/clock startup intervals,
reads GPU_ID, issues a standard Bifrost soft reset and requires the reset
completion bit. Shader/tiler/L2/address-space/job-slot masks are then read
before restoring the hardware state. It never substitutes the CPU framebuffer
for successful GPU execution or publishes a Metal device from these reads.

Host verification: `clang++ -std=c++17 -fsanitize=address,undefined
scripts/test-sprd-radio-gpu.cpp -o /tmp/test-sprd-radio-gpu` and execute the
result. This covers ROM identity, original memory-window addressing, CMD53
read/write through private ADMA buffers, bounded errors, partial-start rollback
and GPU reset failures. M120 hardware evidence confirms chip `0x2355b001`,
strap `0`, successful CMD53 data completion, and a Mali `WRITE_VALUE` job whose
readback matched `0x4750554d313139`; it does not establish Wi-Fi or Bluetooth
services.

The M121 firmware loader embeds exactly the factory 1h190 image prefix:
`wcnmodem` partition bytes `[0, 0xe7400)`, SHA-256
`59e177b4cb195c63a0021164d286c13c5c4fc9be9d430457451db40dc8caf554`. Its
vector words are SP `0x001a0aa0` and reset `0x00100319`; the shipped loader
places it at `0x40500000`, holds/releases bit 0 at `0x40088288`, and waits for
mailbox states at `0x405e73b0`. If the optional factory TSX calibration file
or AFC flag is missing, the original loader's value is `0xffff`; M121 preserves
that fallback and records it. Firmware boot is only accepted after the vector
readback and final mailbox value `0xf0f0f0ff`. This first acceptance transaction
holds CP again and restores the SDIO host and owned rails before returning;
it does not leave an unmanaged running radio behind. M121 firmware boot,
radio calibration, Wi-Fi scan/association/traffic and Bluetooth HCI remain
separate hardware acceptance stages; no Metal device is published.

## PWM2 backlight

`SprdBacklight.h` adapts the CoreBrightness property/fade frontend from the
pinned UniXNU-sheng `package/AppleARMBacklight.cpp`, whose copyright and BSD
2-Clause-Patent license apply to the reused interface design. Its UMS512
backend follows the shipped4.14 `sprd_pwm_*` implementation. The exact factory 1h190 256-entry curve,
scale 880, is preserved in `FactoryBacklight.h`.

PWM2 uses `0x32260000 + 2*0x20`, AON gate `0x327d0008` bit27 and the existing
clock mux `0x32080240`. Takeover requires a valid firmware waveform (MOD255,
bounded duty/prescaler) and the original ext26m parent. M115 applies the factory
75000ns request: PRESCALE6, MOD255, continuous PAT_LOW/PAT_HIGHffff/ffff.
The quantized output period is68653.8ns, about14.6kHz. Store order is the exact
shipped MOD/DUTY/PAT_LOW/PAT_HIGH/PRESCALE commit, not a newer driver's
DUTY-last sequence. Other channels, divider and clock mux remain unchanged.
Every brightness write is read back; ENABLE changes only on actual off/on.
Stop restores the saved waveform including both pattern registers and only
the clock gate owned by this service. IOKit power states control the backlight,
not DSI panel rails or SoC deep sleep.

The CoreBrightness conformance bridge copies exactly 168 verified IOService
slots and qualifies the original AppleARMBacklight metaclass extent `0x370`.
Publishing a backlight service alone is not end-to-end brightness evidence;
record CoreBrightness requests, PWM readback and physical display observation.

## Touch reset and pin mux

After both factory touch addresses explicitly NACK, `SprdTouchPins.h` applies
the shipped gt9xx RESET/INT address-selection sequence once. GPIO145 RESET
is low20ms, GPIO144 INT high2ms, RESET high6ms, INT low50ms, then INT is input.
Existing responding controllers are not reset. No touchscreen firmware or
configuration table is written.

The shipped `sprd_pinctrl_probe` passes common offset `0x34`, misc offset
`0x434`; its pin table has63 global entries followed by common pins. EXTINT0,
EXTINT1,SDA3,SCL3 (224..227) therefore use `0x2b8/2bc/2c0/2c4`. Only mux
bits5:4 are changed, preserving pull, drive and sleep settings. GPIO bank9
changes only bits0/1. Stop restores those bits and the original muxes.
The original 1h190 touchscreen supply VDD28 is always-on; this code does not
guess new regulator programming. A NACK after this sequence remains a real
failure requiring the physical controller/supply evidence.

## SC2730 FGU calibration and power dependency

`SprdFuelGauge.h` reads the shipped FGU calibration cell at byteoffset6,
using SC2730 module1808 bit6 and hardware lock12. Its real clock is
`splk-eb` (AON+4 bit10), obtained after applying the factory Image's relative
relocations and including the `clk_hw_onecell_data` count/padding. The
adjacent USB PHY clock is not substituted. Only READ_START/CLR_RDDONE are
issued; programming-start, DATA_WR and timing registers are never written.
The original module, clock and lock ownership are restored on return.

Voltage/current conversion follows the shipped FGU coefficients and the
board's spec20000/real10000 uOhm ratio. Failed calibration or inactive FGU
gates leave measurements unavailable. The invalid persisted capacityFFF is
not a battery percentage; calibrated samples alone are not a SOC algorithm
or an iOS battery-service implementation. Charger status/fault reads remain
read-only.

The current pinned ArmCpu quiesce disables its timer and GIC interface and
does not implement a system-resume path. A physical power-key report works
only while its polling CPU runs. This service therefore owns a standard
CPU PM assertion, released on stop, while this acquisition path is active.
Display-off remains available. Full SoC suspend/resume, interrupt wake and
per-device power restoration remain unqualified; do not label this assertion
as hardware sleep support.

### M115 flicker correction

The earlier driver preserved cboot PRESCALE8003 and omitted both pattern
registers, although the shipped Android PWM probe selects ext26m and its
backlight requests75000ns. Disabling the channel changing8003 to0003 only
establishes a readback transition; it does not qualify the inherited period
or pattern for display. After the user reported visible backlight flicker,
the exact original config function was emulated to recover the store order
and PRESCALE6 recipe. M115 restores that factory initialization, then retains
it across level changes. Source tests check atomic commit, all256 levels,
off/on, readback refusal, unchanged sibling registers and full rollback.
The flashed M115 image reads PRESCALE8003->8006, MOD255/DUTY25, DIV0,
PAT_LOW/PAT_HIGHffff/ffff and ext26m mux1. Both patterns were alreadyffff in
cboot, so pattern omission alone is not established as the cause. Brightness
and physical power-key off/on writes pass readback. The user then confirmed
that visible flicker stopped. This qualifies the observed M115 display result;
the programmed14.6kHz estimate has not been measured with an oscilloscope.

M116 on-device Mali reset passes: GPU_ID74021000, shader mask3, tiler1, L2 mask1,
AS maskff and JS mask7. This identifies the GPU hardware path; a GPU job, PanVK
and Metal presentation have not yet been run. WCN power returns Protocol before
SDIO commands. M117 restores the omitted original regulator prerequisite:
shipped sc2730_regulator_probe atffffff800852c49c sets1bd0/6e7f before calling
regmap_write. The matching4.14 BSP names it SC2730_PWR_WR_PROT; its status bit15
is read-only and its key field is15bits. Each changed regulator field is read
back, and the previous key is restored only after rail/GPIO/clock cleanup.
Failed initialization retains the actual stage/register/mask/wanted/read value.
The header test exercises initially locked and already unlocked PMICs and
failure during unlock/each supply operation, with exact state restoration.

M117 verifies key status0->8000 and VDDSIM2 readback but stops atGPIO143.
M118 matches the shipped sprd_gpio_direction_output: DMSK request, DIR output,
INEN clear, DATA last. The previous DATA-before-DIR path also left INEN set,
so reading DATA checked an input sample. GPIO diagnostics retain the actual
pin/data/direction/INEN/DMSK snapshot. No inversion of raw CHIP_EN is added.

### Pinned Mali task implementation

M118 on-device WCN power, raw GPIO sequencing and SDIO commands0/5/3/7/52
pass: OCR91ffff00, RCA8800, CCCR43, function1ready2, MANFID0/0 as expected.
The first CMD53 exposes the SDHCR11 PIO defect: TransferComplete without
BufferReadReady, zero FIFO bytes. M120 uses the same16-byte ADMA64 descriptor
format and vendor engine/64-bit mode as the board's qualified eMMC driver.
SDIO1 receives separate prepared, physically contiguous,32-bit DMA buffers;
it never shares an eMMC descriptor or reads a DMA-owned FIFO. Both command
and transfer completion are required, DMA_END is only acknowledged. Timeout
resets both CMD/DATA lines before returning DMA buffers, or retains references
if reset never completes. Host tests exercise both confirmed and stuck reset.

The board reuses Dracen-W/Mali-G52 at2f279e1c0213c2ec33ec9e0f2f643c04c65565ec
for Bifrost v7 feature decode, LPAE tables, AS programming, WRITE_VALUE encoding
and polled job completion. Original GPL notices and per-file source hashes are
preserved inhardware/mali-g52-reuse.json. The MTK PMIC/pwrap/SPM implementation
is excluded; NativeMaliPower.h only qualifies this board's live Sprd resources.
Reset progress goes to UART, not the existing display's framebuffer.

The first workload uses a16KiB physically contiguous, prepared,32-bit-addressable
IOBufferMemoryDescriptor. GPU IOVA1000000 maps only its first8KiB; the chain
writes4750554d313139 to the second4KiB page. Table pages get separate16KiB
descriptors while exposing4KiB Mali granules. CPU cache operations use the live
CTR_EL0 line size and explicit PoC clean/invalidate. Success requires the JM
completion, the literal written value and confirmed teardown; identity alone
does not qualify acceleration. On unconfirmed DMA stop, buffer references remain
pinned and domain restoration is refused. No Metal plugin or accelerator is
published by this owned-memory test. The host codec/table tests pass; hardware
acceptance must come from the exact candidate's M119 log.

## Battery source and runtime evidence

M121 hardware acceptance: chip2355b001, exact947200-byte factory firmware,
vector readback and calibration request/ack/ready f0f0f0f1/f2/ff all passed.
The test then confirmed CP reset and restored its rails. M119's real Mali
WRITE_VALUE, literal target value and stopped-DMA cleanup passed in that boot.
This does not publish a Wi-Fi interface, Bluetooth controller or Metal device.

M122 extends that qualified transport with840-byte fixed-FIFO20 CMD53 blocks.
The actual kernel's sdiohal_hwtype_to_channel adds12 to RX subtype (not16).
Wi-Fi command TX8 and RX22 consequently use public subtypes8 and10. API main
version1 and all256 driver command versions are extracted from the shipped
sprdwl_ng.ko g_api_array, not inferred from a newer donor. A bounded GPIO96
poller receives1680-byte aggregates, checks public framing and trailer bounds,
and matches command+transaction before accepting SYNC_VERSION or GET_INFO.
No OPEN, scan or RF transmission is attempted before board calibration is
integrated. GPIO96 and slave dedicated interrupt enables are restored before
the existing CP-reset/power teardown. ASan/UBSan exercises valid/stale/error
replies, malformed lengths, RX timeout, aggregation limit and block geometry.

Through M113, the device reports efuse `775d` (voltage ADC705/current ADC5640),
POCV2940, QMAX4097 and compensated boot OCV4170000uV. The exact factory table
maps that to80%; `USER_AREA=afff` is first-power-on with invalid saved capacity,
not100%. M113 leaves that estimate unpublished. M114 publishes the original
IOPMPowerSource with80% starting capacity, voltage4169..4170mV and a live counter
progressing `fffff0da -> fffff149 -> fffff1b4`. External power and charger
termination are observed, so IsCharging remains false; termination does not
force100%. This verifies the acquired service, not a physical screen observation
or a full discharge-cycle qualification. Current watchdog fault80 reappears
after the initial latched/current80/00 pair; charge watchdog/control ownership
remains unfinished and is not described as a healthy charging policy.

`SprdBattery.h` adapts the IOPMPowerSource setters/notification frontend from
the pinned UniXNU-sheng `ShengBattery.cpp` under BSD-2-Clause-Patent. The backend
uses this board's shipped4.14 kernel and4900mAh design capacity. The original
OCV routine was emulated against10067 compiled-C++ vectors; the original counter
arithmetic was emulated against11845 vectors including every ADC calibration
value, both signs and rounding boundaries. Source tests cover torn counter
reads, wraparound, counter resets and unavailable hardware samples.

Capacity starts once from a qualified boot OCV or correctly decoded saved
capacity, then follows calibrated2Hz coulomb-count deltas. This is the factory
normal-temperature estimate; learned full capacity, temperature compensation,
aging/cycle count, time estimates and charge-current/JEITA control are not yet
qualified. They are not filled with fixed values. REG09 is read consecutively
twice to separate latched and current charger faults. Charger termination alone
does not force the factory-OCV estimate to100%.

M109-M113 verify GT9110fw5023 at the factory address and attachment of the
original IOHIDEventDriver and its user client. On2026-11-18 the user identified
four physical contacts in TL/TR/BL/BR order. Their raw samples were75/1119,
109/23,1879/1142 and1869/54. M146 converts the retained1920x1200 controller
configuration to the1200x1920 panel using x'=1199-y,y'=x before HID scaling.
Unqualified configuration dimensions are rejected rather than assigned an
assumed orientation. Contact IDs and zero-contact releases are preserved.
M112 verifies CoreBrightness
dim/off/on requests reaching PWM2 with logical1/duty1, off0/0 and on1/1, with
strict register readback. Physical display appearance is a separate observation.

M122 hardware: firmware ready and GPU regression passed, but SYNC_VERSION
timed out with GPIO96 low/no frames; later CP reset access failed. Thus packet
transport is not hardware-qualified. The original start_marlin is not merely
firmware boot: mem_pd_save_bin snapshots both regions, powers them down and
acknowledges SAVE_CP_MEM; mem_pd_mgr then powers/reloads Wi-Fi and waits for
WIFI_OPEN before the first host command. M123 ports that exact sequence with
bounded CP hardware-spinlock acquisition, checked CP ranges and original
UMW2652 memory masks. AON wake precedes metadata reads and final CP reset.
An explicit bbk-wcn-fast-clock=1 diagnostic may select divisor0400 after
enumeration and requires a repeated chip-ID read; default remains unchanged.

M123 hardware: MEM_SAVE, CP runtime ranges (Wi-Fi40520000+70000, BT405a8000+36300),
snapshot/power-down/reload/WIFI_OPEN, final wake/reset and GPU regression passed.
The remaining command parse failure is independent. M124 corrects the factory
adma-rx transaction geometry: each1680-byte packet slot is followed by a
separate840-byte DTBS block; the first receive is therefore2520 bytes/3blocks.
The original1680 normal-mode aggregate assumption is rejected by host tests.

M124 hardware acceptance now confirms SYNC_VERSION (260-byte reply) and
GET_INFO (93-byte reply): chip2355, revision0, firmwareaa51, standards7f and
capabilitiesf7f. The receive transaction is2520 bytes with valid272/remaining1
in its separate DTBS. CP reset, IRQ/GPIO/power restoration and Mali WRITE_VALUE
all passed. These are firmware-command results; no OS network interface,
association, Bluetooth HCI controller or Metal renderer is established.

M125 follows the original module's SYNC_VERSION -> DOWNLOAD_INI -> GET_INFO
sequence. Chip2355b001 is UMW2652 AB, so it uses the shipped
wifi_board_config.ini, not the AA variant. Packed wifi_conf_t3300 and section
offsets/sizes come from the exact shipped sprdwl_ng.ko DWARF and command code.
The packed input is pinned at5b91206f79606e65780da3a229584bf8c3e2beb19b7a2437345688cfe5621fff.
Sections1/2/4 contain328/1464/3 bytes, with CRC16/MODBUSffff/a001 and the
original four-byte section prefix. RF section3 is absent because rf_data_len0.
Section2 requires two840-byte TX blocks; the EOF and padding stay in the same
CMD53 transaction. OPEN station uses the privately acquired six-byte address
from /chosen/mac-address-wifi0; no address is compiled or printed. CLOSE uses
the firmware-returned context. Failure stops the sequence, then the existing
CP-reset/resource cleanup runs. Hardware M125 remains unverified until its
capture confirms every command and cleanup result.

M126 stages native HCI over the same owned SDIO transport, not a Qualcomm UART.
The original 1h190 sprdbt_tty.ko (7c167434995cc76c02100a975f408c5b68c1f1136d40ebf7aca214577bbe89ea)
confirms TX3 and H4 delivery; RX17 uses the shipped channel base12, subtype5.
Its compiled SITM padding is4 bytes, including a complete4-byte pad for aligned
commands; donor source with8-byte padding does not describe this binary.
The BT memory snapshot stays alive until BT's c00 memory bits are enabled,
the bytes are restored and BT_BIN_DOWNLOAD/BT_OPEN completes. Wi-Fi's80 DRAM
bit is preserved. The public interrupt, GPIO, host and CP/power teardown stays
with this one owner.

HCI initialization reuses the order in Otto Pflueger's btsprdsipc.c at Linux
464b3e7bf45bb300e190339977abba36c8078e1f: FCA0 PSKEY, FCA2 RF, FCA1(00,09,01),
then standard RESET. Factory160/252-byte config is used; the PSKEY address at20
is deliberately zero in source and filled from /chosen/mac-address-bluetooth0
in HCI little-endian order. Bounded H4 event reassembly matches command
completion opcode/status, then READ_LOCAL_VERSION and internal-only BDADDR
comparison provide acceptance. No factory address is logged. Disable and CP
reset follow before resources are freed. This source is not evidence for a
published iOS Bluetooth controller or pairing; its hardware results are pending.

M127 documents the original sprdwl_cmd_send_recv() contract: WIFI_CMD_OPEN
returns its allocated vif context in the public response header. Its body is
empty because the original command wrapper writes hdr->common.ctx_id directly
to the caller's vif_ctx_id pointer. The client uses that header context for
subsequent CLOSE; treating OPEN as a one-byte payload is wrong.

M128 adds the original SC2355 WIFI_CMD_SCAN request and asynchronous scan
events. The request uses channel bitmap bits 0/5/10 (channels 1/6/11), an empty
SSID and a zero 5 GHz channel count. The bounded parser accepts the command
response, WIFI_EVENT_MGMT_FRAME scan beacons/probe responses, and
WIFI_EVENT_SCAN_DONE; malformed lengths and information-element boundaries
fail the probe. SSIDs and BSSIDs remain in the temporary in-memory cache and
are not printed. This is a firmware scan probe, not yet the Apple80211/ifnet
network frontend or association/DHCP path; hardware qualification is pending.
