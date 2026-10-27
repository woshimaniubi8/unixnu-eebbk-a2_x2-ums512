/** @file
  Register base addresses and board constants for the Unisoc UMS512
  (SharkL5Pro, T610 family) as used by the BBK/EEBBK A2 / X2 / X2P tablet
  (board ums512_1h190).

  Sources, in order of precedence:
    * the vendor boot device tree of this device
      (analysis/extracted/boot/boot-appended.dtb -> _scratch/boot-dtb.dts),
    * the cboot / U-Boot 2015.07 device tree (analysis/extracted/uboot/
      uboot-base.dtb) for what the bootloader itself sets up,
    * the working UMS9621 port (_repos/e5-xnu) for register offsets of IP
      blocks that are shared between the two SoCs; every such value is marked
      with its provenance and must be re-verified on hardware.

  TODO(hardware) markers below are the values this port cannot prove without
  a device.  Nothing here is a guess presented as fact.

  Copyright (c) 2026, UniXNU bbk-a2 contributors. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef UMS512_H_
#define UMS512_H_

//
// Interrupt controller: GICv3.  Vendor DT (boot-dtb.dts):
//   interrupt-controller@12000000 {
//     compatible = "arm,gic-v3";
//     redistributor-stride = <0 0x20000>;
//     #redistributor-regions = <1>;
//     reg = <0 0x12000000 0 0x20000   // GICD
//            0 0x12040000 0 0x100000>; // GICR, 8 frames
//   };
// Matches the UMS9621 port, so the shared ArmGicDxe configuration is reused.
//
#define UMS512_GICD_BASE   0x12000000
#define UMS512_GICD_SIZE   0x00020000
#define UMS512_GICR_BASE   0x12040000
#define UMS512_GICR_SIZE   0x00100000
#define UMS512_GICR_STRIDE 0x00020000

//
// Architectural timer.  Vendor DT (boot-dtb.dts /timer):
//   compatible = "arm,armv8-timer";
//   interrupts = <1 13 4>, <1 14 4>, <1 11 4>, <1 10 4>;
// i.e. secure-physical 13, non-secure-physical 14, virtual 11, hypervisor 10.
// ArmPkg defaults to 29/30/27/26, which never fire here: DXE would stall on
// the first timer event with no output (exactly what the E5 port hit).
//
// The counter frequency is read from CNTFRQ_EL0 at run time (the E5 port
// measured 26 MHz on UMS9621; UMS512 is expected to be 26 MHz too but the
// value is not hardcoded anywhere).
//
#define UMS512_TIMER_PPI_SEC   13
#define UMS512_TIMER_PPI_NSEC  14
#define UMS512_TIMER_PPI_VIRT  11
#define UMS512_TIMER_PPI_HYP   10

//
// Debug UART: vendor DT serial@70100000 (/chosen stdout-path, kernel cmdline
// console=ttyS1).  AP-APB serial block starts at 0x70000000, stride 0x10000:
//   serial0 @0x70000000, serial1 @0x70100000, serial2 @0x70200000, ...
// The stock firmware has already initialised and clocked UART1 when cboot
// jumps to us, so SprdDebugSerialPortLib only polls LSR and writes TXD; it
// does NOT reprogram the baud rate (the APB clock tree is not modelled in
// this firmware).  TODO(hardware): confirm the baud rate at handoff (U-Boot
// env baudrate=921600, boot.img cmdline says 115200, the device tree /chosen
// says 921600; probe both on a UART pad).
//
#define UMS512_UART0_BASE   0x70000000
#define UMS512_UART1_BASE   0x70100000
#define UMS512_UART_STRIDE  0x00010000
#define UMS512_UART_TXD     0x00
#define UMS512_UART_LSR     0x08
#define UMS512_UART_LSR_TX_OVER (1U << 15)

//
// Storage: eMMC on the SDHCI controller at 0x71400000.
// Vendor DT (boot-dtb.dts): sdio@71400000, compatible "sprd,sdhc-r11p1",
//   sprd,name = "sdio_emmc", no-sdio/no-sd (HS400 class part).
// Kernel cmdline also carries androidboot.boot_devices=soc/soc:ap-apb/71400000.sdio.
//
// SprdSdhciMmcHostDxe drives this controller in takeover mode (see that file):
// it resets only the CMD/DAT lines, keeps the cboot clock/power/delay setup and
// uses PIO for data.  EmbeddedPkg/Universal/MmcDxe then produces the BlockIo
// handle that PartitionDxe + Fat use to expose the payload FAT volume stored in
// the socko partition (the Android vendor kernel-module partition, unused when
// booting UniXNU/iOS).  UNVERIFIED on hardware.
//
#define UMS512_EMMC_BASE    0x71400000
#define UMS512_EMMC_SIZE    0x00001000

//
// Display.  Vendor DT (boot-dtb.dts):
//   dpu@20300000  compatible "sprd,display-processor", sprd,ip = "dpu-r4p0"
//   gsp@20300000  compatible "sprd,gsp-core"      (same 0x20300000 window)
//   dsi@20400000  compatible "sprd,dsi-host",     sprd,ip = "sprd,dsi-ctrl r3p1"
//   dphy          compatible "sprd,dsi-phy"       (same 0x20400000 window)
// The bootloader leaves the "logobuffer" reserved region at 0x9e000000
// (cboot/U-Boot device tree: logobuffer@9e000000, size 0x9e4000) being scanned
// out by the DPU, with the panel powered and the backlight on.
//
// Panel: U-Boot DTS lcd_himax8279_boe_mipi_fhd_bbk_H190, 1200x1920, 4 lanes,
// RGB888, phy-bit-clock 0xdf5de.  The DTBO partition carries alternative
// panels for other board variants (td4310/ssd2092 1080x2160, dummy 720x1280).
//
// TODO(hardware): DPU layer format (16 bpp RGB565 vs 32 bpp XRGB8888) and the
// row stride are not confirmed.  The UMS9621 E5 port read LAY0_CTRL format
// field 4 (RGB565) and PANEL_SIZE 0x01e00140; UMS512 DPU is "dpu-r4p0" and its
// register layout has NOT been read on hardware.
//
#define UMS512_DPU_BASE     0x20300000
#define UMS512_DPU_SIZE     0x00002000
#define UMS512_DSI_BASE     0x20400000
#define UMS512_DSI_SIZE     0x00001000
#define UMS512_DPU_GLB_BASE 0x20100000   // TODO(hardware): DPU GLB syscon not confirmed
#define UMS512_FB_BASE      0x9e000000
#define UMS512_FB_SIZE      0x009e4000
#define UMS512_PANEL_WIDTH  1200
#define UMS512_PANEL_HEIGHT 1920

//
// AON / PMU / GPIO.
// Vendor DT (boot-dtb.dts):
//   aon_apb_regs = "/soc/syscon@327d0000"   (aonapb-gate, "sprd,sharkl5pro-aon-gate")
//   pmu_apb_regs = "/soc/syscon@327e0000"
//   ap_ahb_regs  = "/soc/syscon@20100000"
//   ap_gpio      = "/soc/aon/gpio@32070000"
//   eic          = gpio@32000000 / 0x32000080 / 0x320000a0 / 0x320000c0
//   pmic         = spi@32100000/pmic@0 (SC2730, includes its own GPIO block)
//   watchdog@322f0000 compatible "sprd,wdt-r2p0-fiq"
//
#define UMS512_AON_APB_BASE     0x327d0000
#define UMS512_PMU_APB_BASE     0x327e0000
#define UMS512_AP_AHB_BASE      0x20100000
#define UMS512_AP_GPIO_BASE     0x32070000
#define UMS512_EIC_DEBOUNCE_BASE 0x32000000
#define UMS512_PMIC_SPI_BASE    0x32100000
#define UMS512_WDT_BASE         0x322f0000

//
// AP watchdog (sprd,wdt-r2p0-fiq).  Register map is shared with UMS9621
// (E5 port), but the two AON gate clock bits below are NOT verified for the
// sharkl5pro clock table.  Do not enable the shim watchdog until they are.
//
#define UMS512_WDT_LOAD_LOW   0x00
#define UMS512_WDT_LOAD_HIGH  0x04
#define UMS512_WDT_CTRL       0x08
#define UMS512_WDT_INT_CLR    0x0C
#define UMS512_WDT_INT_RAW    0x10
#define UMS512_WDT_LOCK       0x20
#define UMS512_WDT_UNLOCK_KEY 0xE551
#define UMS512_WDT_CTRL_INT_EN     0x1
#define UMS512_WDT_CTRL_CNT_EN     0x2
#define UMS512_WDT_CTRL_NEW_VER_EN 0x4
#define UMS512_WDT_CTRL_RST_EN     0x8
// TODO(hardware): sharkl5pro aon-gate bit indices (E5 used reg 0x08 bit 29
// for ap-wdg-eb and reg 0x18 bit 7 for ap-wdg-rtc-eb; "set" is written to
// base + reg + 0x1000).
#define UMS512_AON_GATE_SET   0x00001000
#define UMS512_AON_WDT_EB_REG 0x00000008
#define UMS512_AON_WDT_EB_BIT 29
#define UMS512_AON_WDT_RTC_EB_REG 0x00000018
#define UMS512_AON_WDT_RTC_EB_BIT 7

//
// Ramoops.  Vendor DT (boot-dtb.dts): ramoops@fff80000, size 0x40000,
// record-size 0x8000, console-size 0x8000, pmsg-size 0x8000.  The zone
// layout is the sprd persistent-RAM one; the offsets below are the ones the
// UMS9621 port measured on the same driver family:
//   dmesg   @0xfff80000 (survives Android boot, read as dmesg-ramoops-0)
//   console @0xfffb0000 (Android console zone)
// TODO(hardware): re-read the zone headers on this device before trusting the
// log read-back recipe.
//
#define UMS512_RAMOOPS_BASE      0xFFF80000
#define UMS512_RAMOOPS_SIZE      0x00040000
#define UMS512_RAMOOPS_DMESG     0xFFF80000
#define UMS512_RAMOOPS_CONSOLE   0xFFFB0000
#define UMS512_RAMOOPS_SIG       0x43474244   // "DBGC" little endian

//
// DRAM.  Vendor DT memory node: reg = <0 0x80000000 0 0x80000000> = 2 GiB.
// cboot loads the boot.img kernel field to 0x80080000 and passes the merged
// FDT in x0 at 0x82000000 (both measured in the cboot image; see
// analysis/uefi/FEASIBILITY.md).
//
#define UMS512_DRAM_BASE         0x80000000
#define UMS512_DRAM_SIZE         0x80000000
#define UMS512_KERNEL_LOAD_ADDR  0x80080000
#define UMS512_FDT_ADDR          0x82000000

//
// Firmware volume relocation target.  Must clear the cboot load window
// (0x80080000 + 4 MiB) because BootShim copies forward.
//
#define UMS512_FD_BASE           0x80800000
#define UMS512_FD_SIZE           0x00400000

#endif /* UMS512_H_ */
