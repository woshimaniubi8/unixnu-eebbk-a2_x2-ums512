// SPDX-License-Identifier: BSD-2-Clause-Patent
/* The boundary between the board code and the WiFi module: what the board provides, and the calls into the module. Plain C types only. */
#ifndef SPRD_WIFI_NET_ABI_H
#define SPRD_WIFI_NET_ABI_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct nw_wifi_stats { uint32_t stage, scans, frames, beacons, bss_seen; int last_err; };
/* ---- station interface, as the board's network code uses it ----
 * Calls may come from any board thread; each only queues a request (or copies a snapshot) and returns. The work runs, and the events below
 * are called, from whichever thread calls nw_wifi_step, so event handlers must not block. */
struct nw_bss_info {
  uint8_t bssid[6]; uint8_t ssid_len; uint8_t ssid[32];
  uint32_t freq_mhz; int8_t signal_dbm; uint16_t caps, beacon_int; uint32_t age_ms;
  uint16_t ie_len; uint8_t ie[384];                  /* the information elements as the access point sent them */
};
enum { NW_STA_OFF, NW_STA_IDLE, NW_STA_SCANNING, NW_STA_AUTH, NW_STA_ASSOC, NW_STA_KEYS, NW_STA_UP };
enum { NW_RES_OK = 0, NW_RES_NOT_FOUND = 1, NW_RES_TIMEOUT = 2, NW_RES_REJECTED = 3, NW_RES_BAD_KEY = 4, NW_RES_UNSUPPORTED = 5, NW_RES_FAILED = 6 };
struct nw_wifi_status { uint32_t state; int32_t result; uint8_t bssid[6]; uint8_t ssid_len; uint8_t ssid[32]; uint32_t freq_mhz; int8_t signal_dbm; uint8_t mac[6]; uint32_t channels; };
struct nw_wifi_events {
  void (*scan_done)(void);
  void (*connect_done)(int result);                  /* NW_RES_* */
  void (*link_lost)(int reason);
  void (*rx)(const uint8_t *eth, size_t len);        /* a frame for the host network stack: Ethernet header and payload */
};
void nw_wifi_set_events(const struct nw_wifi_events *ev);
int nw_wifi_scan(const uint8_t *ssid, size_t len);                       /* ssid may be NULL (all networks) */
int nw_wifi_bss_count(void);
int nw_wifi_bss_get(int i, struct nw_bss_info *out);                     /* 0 on success */
int nw_wifi_connect(const uint8_t *ssid, size_t len, const uint8_t *bssid, const char *pass, size_t plen);   /* pass NULL: open network */
int nw_wifi_disconnect(void);
int nw_wifi_key(uint8_t index,int pairwise,const uint8_t *address,
                const uint8_t *key,size_t length,const uint8_t *sequence,size_t sequenceLength);
int nw_wifi_tx(const uint8_t *eth, size_t len);                          /* 0 queued */
void nw_wifi_status_get(struct nw_wifi_status *st);
void nw_wifi_tx_reasons(char *text, int size);                          /* the driver's transmit completion counts, as text */
int nw_wifi_power(int on);
int nw_wifi_bss_clear(void);
#ifdef __cplusplus
}
#endif
#endif
