// SPDX-License-Identifier: GPL-2.0-only
// Board sessions reuse the upstream NativeMali ABI and own one GPU AS.
#pragma once
#include <IOKit/IOUserClient.h>
#include "mali/NativeMaliUserClient.h"
#include "mali/NativeMaliXnuSupport.h"

class NativeSprdMaliService;
struct SprdMaliSession {
    NativeMaliXnuCache cache;
    NativeMaliXnuPages pages;
    NativeMaliXnuBuffers buffers;
    NativeMaliVm vm;
    NativeMaliUserClient core;
    uint32_t as=0;
    bool allocated=false;
    bool enabled=false;
    bool opened=false;
};

class NativeSprdMaliUserClient final : public IOUserClient {
    OSDeclareDefaultStructors(NativeSprdMaliUserClient)
    NativeSprdMaliService *owner_=nullptr;
    SprdMaliSession *session_=nullptr;
    IOMemoryDescriptor *views_[NativeMaliBoTableCapacity]={};
    IOReturn shutdown();
    void releaseViews();
    static IOReturn action(OSObject *,void *,IOExternalMethodArguments *);
public:
    bool initWithTask(task_t,void *,UInt32,OSDictionary *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    IOReturn clientClose() override;
    IOReturn clientMemoryForType(UInt32,IOOptionBits *,IOMemoryDescriptor **) override;
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,
        IOExternalMethodDispatch *,OSObject *,void *) override;
};
