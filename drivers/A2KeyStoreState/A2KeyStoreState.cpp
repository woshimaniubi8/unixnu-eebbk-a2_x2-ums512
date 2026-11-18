// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board lifecycle for the pinned UniXNU AppleKeyStoreState component.
// The explicitly selected ephemeral RAM Data profile uses upstream software
// wrapping keyed from actual board identity; it does not implement hardware data protection.
#include <IOKit/IOKitKeys.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>

extern "C" void proc_selfname(char *name, int size);
extern "C" int proc_selfpid(void);

// Keep the upstream selector implementation and metaclasses byte-for-byte in
// their own source file; this wrapper supplies only the board state/lifecycle.
#include "upstream/NativeKeyStoreEvents.cpp"

namespace {
bool stateEnabled;
uint8_t deviceIdentity[6] = {};
size_t deviceIdentityBytes;
volatile SInt32 calls, opened, refused;

bool ephemeralRamData()
{
    unsigned optIn = 0;
    if (!PE_parse_boot_argn("bbk-aks-state", &optIn, sizeof(optIn)) || optIn != 1) return false;
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) return false;
    auto *ephemeral = OSDynamicCast(OSData, chosen->getProperty("ephemeral-storage"));
    uint32_t flag = 0;
    if (ephemeral && ephemeral->getLength() == sizeof(flag)) memcpy(&flag, ephemeral->getBytesNoCopy(), sizeof(flag));
    IORegistryEntry *memory = chosen->childFromPath("memory-map", gIODTPlane);
    auto *ramdisk = memory ? OSDynamicCast(OSData, memory->getProperty("RAMDisk")) : nullptr;
    uint64_t range[2] = {};
    if (ramdisk && ramdisk->getLength() == sizeof(range)) memcpy(range, ramdisk->getBytesNoCopy(), sizeof(range));
    if (memory) memory->release();
    chosen->release();
    // This board ships a 32 MiB unencrypted APFS Data seed in the owned window.
    // No persistent or encrypted Data/keybag profile is represented by these answers.
    return flag == 1 && range[0] != 0 && range[1] == 0x2000000ULL && range[0] + range[1] > range[0];
}

bool readFactoryIdentity()
{
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) return false;
    OSData *identity = OSDynamicCast(OSData, chosen->getProperty("mac-address-bluetooth0"));
    uint8_t value[6] = {};
    const bool sized = identity && identity->getLength() == sizeof(value);
    if (sized) memcpy(value, identity->getBytesNoCopy(), sizeof(value));
    chosen->release();
    unsigned any = 0, notOnes = 0;
    for (unsigned i = 0; i < sizeof(value); ++i) {
        any |= value[i];
        notOnes |= (unsigned)(value[i] ^ 0xffu);
    }
    if (!sized || any == 0 || notOnes == 0) return false;
    memcpy(deviceIdentity, value, sizeof(value));
    deviceIdentityBytes = sizeof(value);
    return true;
}

bool logCount(unsigned n) { return n <= 16 || (n & (n - 1)) == 0; }
}

// Identity is captured before service publication and never printed. There is
// no fabricated identity: the software key cannot be derived without this board.
extern "C" size_t native_aks_device_id(uint8_t *out, size_t capacity)
{
    if (!stateEnabled || !out || deviceIdentityBytes != sizeof(deviceIdentity) || capacity < deviceIdentityBytes) return 0;
    memcpy(out, deviceIdentity, deviceIdentityBytes);
    return deviceIdentityBytes;
}
extern "C" bool native_aks_state_enabled(void) { return stateEnabled; }
extern "C" uint32_t native_aks_lock_state(void) { return 0; }
extern "C" void native_aks_call(uint32_t selector, int32_t result)
{
    unsigned n = (unsigned)OSIncrementAtomic(&calls) + 1;
    if (logCount(n)) IOLog("bbk aks: call selector=%u result=0x%x calls=%u\n", selector, (unsigned)result, n);
    // The original Bluetooth daemon currently exits inside readRootKeys().
    // Keep results unchanged; expose the first four calls and subsequent powers
    // of two for each selector, rather than hiding failures behind a global rate.
    static volatile SInt32 bySelector[256];
    if (selector < 256) {
        unsigned count = (unsigned)OSIncrementAtomic(&bySelector[selector]) + 1;
        if (count <= 4 || (count & (count - 1)) == 0) {
            char name[32] = {0};
            proc_selfname(name, (int)sizeof(name));
            IOLog("M144 aks: pid=%d name=%s selector=%u result=0x%x count=%u\n",
                  proc_selfpid(), name, selector, (unsigned)result, count);
        }
    }

}
extern "C" void native_aks_opened(void)
{
    unsigned n = (unsigned)OSIncrementAtomic(&opened) + 1;
    if (logCount(n)) IOLog("bbk aks: user client opened count=%u\n", n);
}
extern "C" void native_keyevents_open_refused(void)
{
    unsigned n = (unsigned)OSIncrementAtomic(&refused) + 1;
    if (logCount(n)) IOLog("bbk aks: state client refused enabled=%u count=%u\n", stateEnabled, n);
}

class NativeA2KeyStoreState final : public IOService {
    OSDeclareDefaultStructors(NativeA2KeyStoreState)
    IOService *events = nullptr;
public:
    bool start(IOService *provider) override
    {
        if (!ephemeralRamData()) {
            IOLog("bbk aks: not published; explicit ephemeral RAM Data profile required\n");
            return false;
        }
        if (!IOService::start(provider)) return false;
        if (!readFactoryIdentity()) {
            IOLog("M145 aks: actual factory identity unavailable; software key service not started\n");
            IOService::stop(provider);
            return false;
        }
        events = native_keyevents_allocate();
        if (!events || !events->init(nullptr)) {
            IOLog("bbk aks: event service allocation/init failed\n");
            OSSafeReleaseNULL(events);
            IOService::stop(provider);
            return false;
        }
        if (!events->attach(this)) {
            IOLog("bbk aks: event service attach failed\n");
            OSSafeReleaseNULL(events);
            IOService::stop(provider);
            return false;
        }
        stateEnabled = true;
        OSMemoryBarrier();
        if (!events->start(this)) {
            stateEnabled = false;
            events->detach(this);
            OSSafeReleaseNULL(events);
            IOService::stop(provider);
            IOLog("bbk aks: event service start failed\n");
            return false;
        }
        setProperty("EphemeralRAMDataOnly", true);
        registerService();
        IOLog("M145 aks: original upstream software-wrap=1 identity=factory-bluetooth bytes=%u profile=ephemeral-ram-data class=%s\n",
              (unsigned)deviceIdentityBytes, events->getMetaClass()->getClassName());
        return true;
    }
    void stop(IOService *provider) override
    {
        stateEnabled = false;
        OSMemoryBarrier();
        if (events) {
            // Synchronous termination performs stop/detach through IOKit.
            // Calling them again here would repeat the child's teardown.
            if (!events->isInactive() && !events->terminate(kIOServiceSynchronous))
                IOLog("bbk aks: child termination did not complete\n");
            OSSafeReleaseNULL(events);
        }
        IOService::stop(provider);
    }
};
OSDefineMetaClassAndStructors(NativeA2KeyStoreState, IOService)
