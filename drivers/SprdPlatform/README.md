# EEBBK 1h190 platform hardware

This is a hardware port, not a set of Apple-compatible placeholder nodes.
Factory authority is the shipped 4.14.193 kernel (SHA256
`70a55f276551f0fedbbc3c36ad1950d85eab19352b6b0d4eb6e69ae737c98104`)
and its base DTB merged with DTBO #3 (`ums512_1h190`).

## Implemented in this source, not yet hardware-qualified

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
- FGU raw diagnostics only. No fabricated battery percentage, calibration,
  current limit or charging state is published.

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

## PWM2 backlight

`SprdBacklight.h` adapts the CoreBrightness property/fade frontend from the
pinned UniXNU-sheng `package/AppleARMBacklight.cpp`, whose copyright and BSD
2-Clause-Patent license apply to the reused interface design. Its UMS512
backend follows the GPL Spreadtrum `pwm-sprd.c` register contract and the
shipped `sprd_pwm_*` implementation. The exact factory 1h190 256-entry curve,
scale 880, is preserved in `FactoryBacklight.h`.

PWM2 uses `0x32260000 + 2*0x20`, AON gate `0x327d0008` bit27 and the existing
clock mux `0x32080240`. Takeover requires a valid firmware waveform (MOD255,
bounded duty/prescaler); it preserves the source, period and sibling channels.
Every brightness write is read back. Stop restores the saved waveform and only
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
