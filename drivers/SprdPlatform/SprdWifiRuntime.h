// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "SprdWcnScan.h"
#include <IOKit/IOService.h>

class SprdWifiHardware {
public:
    virtual ~SprdWifiHardware()=default;
    virtual SprdBus::Result scan(const uint8_t *,size_t,SprdWcn::WifiScanReport &)=0;
    virtual SprdBus::Result power(bool)=0;
    virtual SprdBus::Result drain(unsigned &)=0;
};
bool sprdWifiRuntimeStart(IOService *,SprdWifiHardware *,const uint8_t *,
                         const SprdWcn::WifiScanReport *);
void sprdWifiRuntimeStop();
