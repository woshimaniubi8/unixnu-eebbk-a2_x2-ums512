// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "SprdBus.h"

namespace SprdBus {
constexpr unsigned kA2TouchRawWidth=1920,kA2TouchRawHeight=1200;
constexpr unsigned kA2PanelWidth=1200,kA2PanelHeight=1920;

// GT9110 firmware5023 retained by cboot reports landscape coordinates.
// Physical TL/TR/BL/BR samples establish x'=1199-y, y'=x for this panel.
inline Result a2GoodixPanelFrame(const Frame &raw,unsigned width,unsigned height,Frame &out) {
    if(width!=kA2TouchRawWidth || height!=kA2TouchRawHeight || raw.count>10)return Invalid;
    Frame panel={};panel.count=raw.count;
    for(unsigned i=0;i<raw.count;i++) {
        const auto &c=raw.contacts[i];
        if(c.x>=width || c.y>=height)return Protocol;
        panel.contacts[i]={c.id,(uint16_t)(height-1-c.y),c.x};
    }
    out=panel;return Ok;
}
}
