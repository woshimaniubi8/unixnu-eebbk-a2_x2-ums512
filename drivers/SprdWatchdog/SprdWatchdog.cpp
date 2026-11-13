// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeSprdWatchdog: Unisoc UMS512 watchdog backend that publishes the
// original IOWatchdog service consumed by /usr/libexec/watchdogd.
//
// The original Apple IOWatchdog is an abstract class in the prelinked
// AppleARMWatchdogTimer kext.  This file declares only its public ABI: the
// protected constructor/destructor/metaclass come from the original kext, the
// five-argument newUserClient and stop overrides keep the original user client
// and lifecycle, and the three trailing pure virtuals are the hardware
// callbacks.  No original class, vtable or metaclass is redefined here.
//
// The original common init (M57 0xfffffff00563e498) publishes the service via
// registerService(0) after preparing its own state; the subclass must map MMIO
// and prepare the backend before calling it.  The M58 kernel must re-check that
// address/ABI before boot.

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryMap.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/libkern.h>

class IOWatchdog : public IOService {
	OSDeclareAbstractStructors(IOWatchdog)

public:
	virtual void stop(IOService *provider) override;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID,
	    UInt32 type, OSDictionary *properties, IOUserClient **handler) override;

	// Non-virtual original common init: this, provider, unused zero, flags.
	bool init(IOService *provider, uintptr_t unusedZero, UInt32 flags);

	// Original trailing pure virtual slots: +0x540, +0x548, +0x550.
	virtual void hwStart() = 0;
	virtual void hwStop() = 0;
	virtual void hwService() = 0;

private:
	unsigned char qualifiedState[0x70];
};

static_assert(sizeof(IOService) == 0x88, "IOService ABI mismatch");
static_assert(sizeof(IOWatchdog) == 0xF8, "IOWatchdog ABI mismatch");

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
		unsigned int size = strnlen(entry, remaining);
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

class NativeSprdWatchdog : public IOWatchdog {
	OSDeclareDefaultStructors(NativeSprdWatchdog)

public:
	virtual IOService *probe(IOService *provider, SInt32 *score) override;
	virtual bool start(IOService *provider) override;
	virtual void stop(IOService *provider) override;
	virtual void hwStart() override;
	virtual void hwStop() override;
	virtual void hwService() override;

private:
	IOMemoryMap *wdtMap = nullptr;
	IOMemoryMap *aonMap = nullptr;
	volatile uint32_t *wdt = nullptr;
	volatile uint32_t *aon = nullptr;
	uint32_t serviceCount = 0;
	bool clocksOn = false;
};

OSDefineMetaClassAndStructors(NativeSprdWatchdog, IOWatchdog)

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

	// AON gate SET registers: CLK_APCPU_WDG_EB bit 30 and
	// CLK_AC_WDG_RTC_EB bit 8.  Status is read back from the base registers.
	aon[0x1008 / 4] = (1u << 30);
	aon[0x1018 / 4] = (1u << 8);
	clocksOn = true;

	IOLog("watchdog: provider=%s compatible=sprd,wdt-r2p0-fiq "
	    "wdt=%p aon=%p ctrl=0x%08x load=0x%08x gate=0x%08x rtc=0x%08x\n",
	    provider->getName(), (void *)wdt, (void *)aon,
	    wdt[0], wdt[1], aon[0x8 / 4], aon[0x18 / 4]);

	// The original common init calls registerService(0), the +0x540 hook and
	// IOService::start(provider); all backend resources are ready here.
	bool ok = init(provider, 0, 0);
	if (!ok) {
		IOLog("watchdog: original common init failed\n");
		return false;
	}
	IOLog("watchdog: service published\n");
	return true;
}

void
NativeSprdWatchdog::stop(IOService *provider)
{
	if (clocksOn && aon != nullptr) {
		aon[0x1000 / 4] = (1u << 30);
		aon[0x1010 / 4] = (1u << 8);
		clocksOn = false;
	}
	if (wdtMap != nullptr) {
		wdtMap->release();
		wdtMap = nullptr;
	}
	if (aonMap != nullptr) {
		aonMap->release();
		aonMap = nullptr;
	}
	IOWatchdog::stop(provider);
}

void
NativeSprdWatchdog::hwStart()
{
	IOLog("watchdog: hwStart\n");
}

void
NativeSprdWatchdog::hwStop()
{
	IOLog("watchdog: hwStop\n");
}

void
NativeSprdWatchdog::hwService()
{
	serviceCount++;
	if ((serviceCount & 0x1f) == 0) {
		IOLog("watchdog: hwService count=%u ctrl=0x%08x\n",
		    serviceCount, wdt != nullptr ? wdt[0] : 0);
	}
}
