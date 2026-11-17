# SC2355 network frontend

The BSD ifnet, Apple80211 request/scan-result ABI, events and IOKit discovery
code are adapted from UniXNU-sheng 08cde20edc3254ed6f7df8d0f5384fb887786e8f,
package/WifiNet.cpp (BSD-2-Clause-Patent; LICENSE.WifiNet).

The board's original WCN firmware, SDIO channel geometry, calibrated radio,
factory identity and OPEN context are the hardware backend. One retained
resource owner controls the shared WCN transport; system requests queue to a
separate work loop. Closing the driver first synchronizes that loop, then
closes the station, holds CP reset, restores IRQ/GPIO, host and rails.

M128 hardware scan on channels 1/6/11 returned nine scan frames and six BSS,
with response/status/done accepted and zero malformed frames. Bluetooth HCI
and the Mali owned-memory WRITE_VALUE regression passed in the same boot.

This frontend is initially scan-only. Association, key installation and
Ethernet TX/RX are still unsupported. It returns the real unsupported/error
result, never a successful association or fabricated network/rate/noise.
Only the three tested channels are currently reported. No SSID, BSSID,
MAC address, private packet contents or keys are printed.

Firmware scan acceptance does not establish Apple80211 discovery or system
scan operation. Those require a successful source-kernel build and on-device
frontend/runtime logs from this revision.
