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

M131's guarded source-kernel build passed. Boot 5a5004ae retained the original
station through Bluetooth initialization and published en0 and the runtime
frontend; the initial scan returned 13 frames and nine BSS with no malformed
frames. No system Apple80211 scan call was observed in the first two minutes.

The exact iPadOS 17.7.11 (21H461) com.apple.wifid launch job has
_LimitLoadToDeviceTree = [wlan, marconi-wifi]. M131's actual ADT contained
neither node. M132 exports the real SC2355 WLAN function as arm-io/wlan;
SprdPlatform retains sole ownership of SDIO1 and radio power. This is a
device-tree-only change. M132's hardware log recorded the network stack
opening and one Apple80211 card-capabilities query (type 12, length 16), which
the initial adapter rejected with EOPNOTSUPP. M133 returns the versioned,
empty implemented-feature bitmap; it does not advertise unimplemented crypto
or QoS offload. The layout is checked against OpenIntelWireless/itlwm's
apple80211_capability_data and the actual 21H461 request length.

Both M131 and M132 later hit a 320-second userspace watchdog timeout involving
backboardd and other services. Frontend publication is verified, sustained
OS boot is not. M133 adds sparse worker counters and an existing native
stackshot at round 4 (before this timeout); it preserves watchdog policy.
