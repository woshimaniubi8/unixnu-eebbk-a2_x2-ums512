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
