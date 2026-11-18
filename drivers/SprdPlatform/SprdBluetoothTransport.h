/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SPRD_BT_TRANSPORT_H
#define SPRD_BT_TRANSPORT_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct sprd_bt_transport_ops {
    void *context;
    void (*wake)(void *);
    /* Queue power transition on the shared WCN worker; never call back inline. */
    int (*power)(void *, bool);
};
extern volatile uint32_t sprd_bt_transport_activity[4];
int sprd_bt_transport_init(const struct sprd_bt_transport_ops *);
void sprd_bt_transport_boot_result(int error, const char *firmware, uint32_t baud);
void sprd_bt_transport_power_result(bool enabled, int error, const char *firmware, uint32_t baud);
uint32_t sprd_bt_transport_receive(const uint8_t *, uint32_t);
uint32_t sprd_bt_transport_tx_pending(void);
uint32_t sprd_bt_transport_tx_take(uint8_t *, uint32_t);
bool sprd_bt_transport_active(void);
#ifdef __cplusplus
}
#endif
#endif
