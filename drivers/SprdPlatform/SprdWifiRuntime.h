// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "SprdWcnStation.h"
#include <IOKit/IOService.h>

class SprdWifiHardware {
public:
    virtual ~SprdWifiHardware()=default;
    virtual SprdBus::Result scan(const uint8_t *,size_t,SprdWcn::WifiScanReport &)=0;
    virtual SprdBus::Result power(bool)=0;
    virtual SprdBus::Result drain(unsigned &)=0;
    virtual void callbacks(SprdWcn::StationCallbacks)=0;
    virtual SprdBus::Result connect(const SprdWcn::StationJoin &)=0;
    virtual SprdBus::Result disconnect(unsigned)=0;
    virtual SprdBus::Result key(const uint8_t *,size_t)=0;
    virtual SprdBus::Result transmit(const uint8_t *,size_t)=0;
};
bool sprdWifiRuntimeStart(IOService *,SprdWifiHardware *,const uint8_t *,
                         const SprdWcn::WifiScanReport *);
void sprdWifiRuntimeStop();
