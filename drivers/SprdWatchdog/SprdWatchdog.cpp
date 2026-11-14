// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeSprdWatchdog: Unisoc UMS512 watchdog backend that publishes the
// original IOWatchdog service consumed by /usr/libexec/watchdogd.
//
// The original IOWatchdog C++ symbols are local to the prelinked
// AppleARMWatchdogTimer kext and cannot be linked from a separate component
// (the first direct-subclass build failed on IOWatchdog::gMetaClass, C2/D2,
// init, newUserClient and vtable).  This driver therefore uses the original
// concrete AppleARMWatchdogTimer metaclass through OSMetaClass::
// allocClassWithName(), copies its vtable, replaces the three trailing
// hardware slots (+0x540/+0x548/+0x550) with this backend, and calls the
// original IOWatchdog common init by its verified address.  The resulting
// object is a real IOWatchdog subclass, so IOServiceMatching("IOWatchdog")
// matches it; no class, metaclass or original function is redefined.

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSKextLib.h>
#include <libkern/libkern.h>

// Verified against the M57 kernel; the final M58 kernel may shift the
// prelinked text, so locate the common init by its unique prologue words.
static constexpr uintptr_t kPrelinkedTextBase = 0xfffffff004920000ULL;
static constexpr size_t kPrelinkedTextSize = 0x1380000;
static constexpr size_t kWatchdogVtableBytes = 0x558;
static constexpr size_t kWatchdogHwStartSlot = 0x540 / sizeof(void *);
static constexpr size_t kWatchdogHwStopSlot = 0x548 / sizeof(void *);
static constexpr size_t kWatchdogHwServiceSlot = 0x550 / sizeof(void *);

static uintptr_t
findWatchdogInit()
{
	static const uint32_t pattern[] = {
		0xd10203ff, 0xa9045ff8, 0xa90557f6, 0xa9064ff4,
		0xa9077bfd, 0x9101c3fd, 0xaa0303f5, 0xaa0103f3,
	};
	const uint32_t *base = reinterpret_cast<const uint32_t *>(kPrelinkedTextBase);
	size_t words = kPrelinkedTextSize / sizeof(uint32_t);
	for (size_t i = 0; i + sizeof(pattern) / sizeof(pattern[0]) < words; i++) {
		if (memcmp(&base[i], pattern, sizeof(pattern)) == 0) {
			return kPrelinkedTextBase + i * sizeof(uint32_t);
		}
	}
	return 0;
}

class NativeSprdWatchdog : public IOService {
	OSDeclareDefaultStructors(NativeSprdWatchdog)

public:
	virtual IOService *probe(IOService *provider, SInt32 *score) override;
	virtual bool start(IOService *provider) override;
	virtual void stop(IOService *provider) override;

	void backendStart();
	void backendStop();
	void backendService();
	void releaseBackend();

private:
	IOMemoryMap *wdtMap = nullptr;
	IOMemoryMap *aonMap = nullptr;
	volatile uint32_t *wdt = nullptr;
	volatile uint32_t *aon = nullptr;
	IOService *watchdogService = nullptr;
	uint32_t serviceCount = 0;
	bool clocksOn = false;
	bool enabledGate = false;
	bool enabledRtc = false;
};

OSDefineMetaClassAndStructors(NativeSprdWatchdog, IOService)

static NativeSprdWatchdog *gWatchdog;
static void *gPatchedVtable[kWatchdogVtableBytes / sizeof(void *)];

static void
watchdogHwStart(IOService *self)
{
	if (gWatchdog != nullptr) {
		gWatchdog->backendStart();
	}
}

static void
watchdogHwStop(IOService *self)
{
	if (gWatchdog != nullptr) {
		gWatchdog->backendStop();
	}
}

static void
watchdogHwService(IOService *self)
{
	if (gWatchdog != nullptr) {
		gWatchdog->backendService();
	}
}

static bool
hasCompatible(IOService *provider, const char *want)
{
	OSData *data;

	if (provider == nullptr) {
		return false;
	}
	data = OSDynamicCast(OSData, provider->getProperty("compatible"));
	if (data == nullptr || data->getLength() == 0) {
		return false;
	}
	const char *base = static_cast<const char *>(data->getBytesNoCopy());
	unsigned int length = data->getLength();
	unsigned int offset = 0;
	while (offset < length) {
		const char *entry = base + offset;
		unsigned int remaining = length - offset;
		unsigned int size = (unsigned int)strnlen(entry, remaining);
		if (size == remaining) {
			break;
		}
		if (strcmp(entry, want) == 0) {
			return true;
		}
		offset += size + 1;
	}
	return false;
}

IOService *
NativeSprdWatchdog::probe(IOService *provider, SInt32 *score)
{
	if (!hasCompatible(provider, "sprd,wdt-r2p0-fiq")) {
		return nullptr;
	}
	if (score != nullptr) {
		*score = 1000;
	}
	return this;
}

bool
NativeSprdWatchdog::start(IOService *provider)
{
	if (provider == nullptr || !hasCompatible(provider, "sprd,wdt-r2p0-fiq")) {
		return false;
	}

	wdtMap = provider->mapDeviceMemoryWithIndex(0);
	aonMap = provider->mapDeviceMemoryWithIndex(1);
	if (wdtMap == nullptr || aonMap == nullptr) {
		IOLog("watchdog: MMIO map failed wdt=%p aon=%p\n", wdtMap, aonMap);
		return false;
	}
	wdt = reinterpret_cast<volatile uint32_t *>(wdtMap->getVirtualAddress());
	aon = reinterpret_cast<volatile uint32_t *>(aonMap->getVirtualAddress());

	uint32_t gateBefore = aon[0x8 / 4];
	uint32_t rtcBefore = aon[0x18 / 4];
	if ((gateBefore & (1u << 30)) == 0) {
		aon[0x1008 / 4] = (1u << 30);
		enabledGate = true;
	}
	if ((rtcBefore & (1u << 8)) == 0) {
		aon[0x1018 / 4] = (1u << 8);
		enabledRtc = true;
	}
	clocksOn = true;
	uint64_t loaded = (uint64_t)wdt[0] | ((uint64_t)wdt[1] << 32);
	IOLog("watchdog: provider=%s compatible=sprd,wdt-r2p0-fiq "
	    "wdt=%p aon=%p ctrl=0x%08x load=0x%016llx int_raw=0x%08x "
	    "gate=0x%08x rtc=0x%08x\n",
	    provider->getName(), (void *)wdt, (void *)aon,
	    wdt[2], loaded, wdt[3], aon[0x8 / 4], aon[0x18 / 4]);

	OSReturn loadStatus = OSKextLoadKextWithIdentifier(
	    "com.apple.driver.AppleARMWatchdogTimer");
	IOLog("watchdog: original kext load status=0x%x\n",
	    static_cast<unsigned>(loadStatus));
	if (loadStatus != kOSReturnSuccess) {
		releaseBackend();
		return false;
	}
	const OSMetaClass *baseMeta = nullptr;
	const OSSymbol *baseName = OSSymbol::withCString("IOWatchdog");
	if (baseName != nullptr) {
		baseMeta = OSMetaClass::getMetaClassWithName(baseName);
		baseName->release();
	}
	IOLog("watchdog: IOWatchdog metaclass=%p classSize=%u\n",
	    (const void *)baseMeta,
	    baseMeta != nullptr ? baseMeta->getClassSize() : 0);

	IOService *wd = static_cast<IOService *>(
	    OSMetaClass::allocClassWithName("AppleARMWatchdogTimer"));
	if (wd == nullptr) {
		IOLog("watchdog: AppleARMWatchdogTimer allocation failed\n");
		releaseBackend();
		return false;
	}
	if (!wd->init(nullptr)) {
		IOLog("watchdog: IOService::init(nullptr) failed\n");
		wd->release();
		releaseBackend();
		return false;
	}
	memcpy(gPatchedVtable, *reinterpret_cast<void **>(wd), kWatchdogVtableBytes);
	gPatchedVtable[kWatchdogHwStartSlot] = reinterpret_cast<void *>(&watchdogHwStart);
	gPatchedVtable[kWatchdogHwStopSlot] = reinterpret_cast<void *>(&watchdogHwStop);
	gPatchedVtable[kWatchdogHwServiceSlot] = reinterpret_cast<void *>(&watchdogHwService);
	*reinterpret_cast<void **>(wd) = gPatchedVtable;

	if (!wd->attach(provider)) {
		IOLog("watchdog: attach failed\n");
		wd->release();
		releaseBackend();
		return false;
	}
	gWatchdog = this;
	using InitFn = bool (*)(IOService *, IOService *, uintptr_t, UInt32);
	uintptr_t initAddress = findWatchdogInit();
	if (initAddress == 0) {
		IOLog("watchdog: IOWatchdog common init pattern not found\n");
		gWatchdog = nullptr;
		wd->detach(provider);
		wd->release();
		releaseBackend();
		return false;
	}
	InitFn init = reinterpret_cast<InitFn>(initAddress);
	bool ok = init(wd, provider, 0, 0);
	if (!ok) {
		IOLog("watchdog: original common init failed\n");
		gWatchdog = nullptr;
		wd->detach(provider);
		wd->release();
		releaseBackend();
		return false;
	}
	watchdogService = wd;
	IOLog("watchdog: service published\n");
	return true;
}

void
NativeSprdWatchdog::releaseBackend()
{
	if (clocksOn && aon != nullptr) {
		if (enabledGate) {
			aon[0x2008 / 4] = (1u << 30);
		}
		if (enabledRtc) {
			aon[0x2018 / 4] = (1u << 8);
		}
		clocksOn = false;
		enabledGate = false;
		enabledRtc = false;
	}
	if (wdtMap != nullptr) {
		wdtMap->release();
		wdtMap = nullptr;
	}
	if (aonMap != nullptr) {
		aonMap->release();
		aonMap = nullptr;
	}
}

void
NativeSprdWatchdog::stop(IOService *provider)
{
	if (watchdogService != nullptr) {
		watchdogService->stop(provider);
		watchdogService->release();
		watchdogService = nullptr;
	}
	gWatchdog = nullptr;
	releaseBackend();
	IOService::stop(provider);
}

void
NativeSprdWatchdog::backendStart()
{
	if (wdt != nullptr) {
		uint64_t loaded = (uint64_t)wdt[0] | ((uint64_t)wdt[1] << 32);
		IOLog("watchdog: hwStart ctrl=0x%08x load=0x%016llx int_raw=0x%08x\n",
		    wdt[2], loaded, wdt[3]);
	}
}

void
NativeSprdWatchdog::backendStop()
{
	if (wdt != nullptr) {
		IOLog("watchdog: hwStop ctrl=0x%08x int_raw=0x%08x\n",
		    wdt[2], wdt[3]);
	}
}

void
NativeSprdWatchdog::backendService()
{
	serviceCount++;
	if (wdt != nullptr && (serviceCount & 0x1f) == 0) {
		IOLog("watchdog: hwService count=%u ctrl=0x%08x int_raw=0x%08x\n",
		    serviceCount, wdt[2], wdt[3]);
	}
}
