# 1h190 peripheral adaptation

`1h190-peripherals.json` records the shipped base DTB merged with DTBO3 and
the current implementation boundary. Candidate nodes do not prove which
optional chip is populated. All firmware digests below were measured from
read-only partition extraction; firmware binaries remain local.

| Subsystem | Factory hardware | Remaining functional boundary |
|---|---|---|
| USB | MUSB5fff0000, SharkL5Pro HS PHY, SC2730 Type-C | MUSB endpoint/DMA backend, cable/role lifecycle, iOS USB device controller integration |
| Wi-Fi/BT | Marlin3 Lite/SC2355 on SDIO | WCN boot/calibration/SDIO channels, Wi-Fi interface, BT HCI transport |
| Audio | VBCv4, SC2730 codec, AUDCP DSP | Original firmware boot/SIPC/PCM DMA, codec routes and iOS audio interface |
| GPU | Mali-gondul60000000 | GPU power/job/MMU backend and iOS IOGPU/Metal user interface; existing CPU framebuffer scanout is independent |
| Camera | Factory I2C sensor candidates, CSI/DCAM/ISP | Actual sensor IDs/power sequence, capture buffers, ISP pipeline and iOS camera interface |
| Sensors | SharkL5Pro CM4 sensorhub, SIPC/mailbox | Firmware/shared-memory handshake, frame CRC/timestamps/calibration and CoreMotion/HID delivery |
| Brightness | PWM2, 256-entry factory curve | Implemented source plus host tests; still requires build and physical/CoreBrightness verification |
| Power | SC2730 ADI, FGU, regulators, CPU PSCI | Raw reads verified; calibrated SOC/charge management and per-device suspend/resume remain |

## Inputs and reusable implementations

- Factory Linux4.14.193 Image SHA256
  `70a55f276551f0fedbbc3c36ad1950d85eab19352b6b0d4eb6e69ae737c98104`.
- Factory merged DTB SHA256
  `a275230ce98dae2c0cc95f36e6ddd09f2d7e06e7bdfac5a6d74dc55dac799874`.
- UMS512 BSP reference:
  `marohinmark/kernel_ums512_5.4@ead7c4e34de9a1465cb9badd7e0cf56691800b16`.
  The BSP covers sibling devices; compare its register protocol with this
  board's shipped driver and wiring before enabling a device.
- CoreBrightness/HID/battery frontend reference: pinned UniXNU-sheng sources.
  Qualcomm hardware, firmware and bus drivers do not match the Unisoc board.
- Mali compiler/protocol reference: Mesa Panfrost. A Linux GLES/Vulkan driver
  is not a ready-made iOS Metal/IOGPU adapter.

## Runtime evidence through M103-local

- Original IOHIDEventDriver attached to physical-key HID device.
- GPIO124 volume-down and SC2730 EIC1 power both produced debounced down/up
  reports, with original HID handleReport returning success.
- Explicit factory INT_EN fixed I2C completion: BQ device register0B reads2C.
- Both factory touch candidates return NACK. The factory pinmux/reset path is
  implemented separately and must prove a chip ID before reports are enabled.
- SC2730 IDs B000/2730 and raw FGU voltage/current reads succeed. Persisted
  capacity fieldFFF is invalid; no fabricated battery percentage is published.
- One M102 run hit SpringBoard userspace watchdog timeout. Preserve that
  independent UI failure; do not disable the watchdog to claim stability.

## Firmware extracted from this tablet

| Partition | Bytes | Observed identity |
|---|---:|---|
| wcnmodem | 10485760 | sc2355_marlin3_lite; MARLIN3_20A_W22.36.3 |
| gnssmodem | 1048576 | Original GNSS firmware |
| l_agdsp | 6291456 | SharkL5_AUDCP_2022Y_VER_2009 |
| pm_sys | 1048576 | SharkL5Pro CM4 plus sensorhub code |

Validation requires hardware identity, successful transactions, OS consumer
delivery and power lifecycle evidence. A registered service, successful build
or node in the ADT alone does not meet that criterion.
