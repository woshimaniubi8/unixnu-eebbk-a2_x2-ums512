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
result. This covers ROM identity, original memory-window addressing, combined
PIO/completion, bounded errors, partial-start rollback and GPU reset failures.
Hardware results belong to the M116 serial receipt. Firmware download, RF
calibration, Wi-Fi scan/association/traffic, Bluetooth HCI and GPU jobs/Metal
remain separately verifiable stages; this initial operation does not claim
those stages have completed.

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

## Battery source and runtime evidence

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

M109-M113 also verify GT9110fw5023 at the factory address and attachment of the
original IOHIDEventDriver and its user client. Physical touch contacts and the
1920x1200-to-1200x1920 transform remain unobserved. M112 verifies CoreBrightness
dim/off/on requests reaching PWM2 with logical1/duty1, off0/0 and on1/1, with
strict register readback. Physical display appearance is a separate observation.
