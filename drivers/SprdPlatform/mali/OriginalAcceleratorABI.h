// SPDX-License-Identifier: MIT
#pragma once
// The exact iOS 17.7.11 IOAcceleratorES base audited in
// OriginalAcceleratorABI.json. No operational methods or private state are
// added to this original 136-byte base.
#include <IOKit/IOService.h>
class IOAcceleratorES : public IOService {
    OSDeclareAbstractStructors(IOAcceleratorES)
};
static_assert(sizeof(IOService)==136 && sizeof(IOAcceleratorES)==136,
    "Qualified original IOAcceleratorES instance extent");
