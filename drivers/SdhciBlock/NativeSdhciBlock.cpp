// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeSdhciBlock - Unisoc UMS512 / SharkL5Pro (T610 family) SDHCR11 eMMC
// transport for the Android "userdata" GPT partition.
//
// Milestone 1 (this driver): attach to the device-tree node, prove that the
// controller register file is reachable from XNU, inherit the state cboot left
// behind and prove the eMMC read path end to end (EXT_CSD + the GPT), then
// record the userdata window.  Milestone 2 publishes that window as an IOKit
// block device; it is not in this build because IOStorageFamily is not part of
// the XNU kernel link (UniXNU binds those symbols from the qualified original
// kernelcache through scripts/build-virtio-mmio-bindings.py and the storage
// interface audit), so the transport below is written to be the final read path.
//
// The controller at 0x71400000 is already initialised by cboot/U-Boot and this
// driver deliberately inherits that state (clock control 0x0207, host control
// 0x20, host control 2 0x3000, vendor clock register 0x204 = 0x30303060): the
// UEFI takeover driver proved on hardware that no timing, clock or bus-width
// reprogramming is needed or possible (CAP base clock reads 0).  Only the
// command/data path below is used.
//
// Command path (replicated from the stock Android sdhcr11 driver through the
// UEFI SprdSdhciMmcHostDxe takeover driver, both of which read LBA 1 and the
// whole 45-entry GPT on this exact tablet):
//   0x04 (16-bit) = (512 & 0xFFF) | 0x7000     block size, 4 KiB SDMA boundary
//   0x00 (32-bit) = blocks                     SDHCR1.1 vendor block count
//   0x58 / 0x5C   = ADMA2 descriptor table address (low / high)
//   0x0C (32-bit) = (index << 24) | 0x003A0000 | 0x0011
//                   command | response/CRC/index/data-present, DMA + data read
//   descriptor    = [ (length << 16) | 0x23 ][address low][address high][0]
//   done when 0x30 & 0x3 == 0x3; error 0x32, ADMA error 0x54.
//
// The interrupt line is never enabled (0x38 stays 0) because this kernel has no
// interrupt handler for the controller: the driver polls 0x30, which is what the
// UEFI takeover driver does as well.

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <kern/task.h>

extern "C" {
// Observation counters for the CI / UART bring-up checks: the values are read
// from the linked kernel image (nm) and from the boot log.
volatile uint32_t native_sdhci_ready = 0;
volatile uint32_t native_sdhci_gpt_userdata = 0;
volatile uint64_t native_sdhci_userdata_first = 0;
volatile uint64_t native_sdhci_userdata_last = 0;
volatile uint32_t native_sdhci_ext_csd_ok = 0;
volatile uint32_t native_sdhci_ext_csd_sectors = 0;
volatile uint32_t native_sdhci_read_requests = 0;
volatile uint32_t native_sdhci_read_failures = 0;
volatile uint64_t native_sdhci_sectors = 0;
volatile uint64_t native_sdhci_mmio_physical = 0;
}

namespace {

// SDHCI / SDHCR1.1 register offsets (16-bit registers are accessed as 16-bit).
constexpr uint32_t kBlockCount        = 0x00;   // vendor: 32-bit block count
constexpr uint32_t kBlockSize         = 0x04;   // 16-bit
constexpr uint32_t kArgument          = 0x08;
constexpr uint32_t kTransferMode      = 0x0C;   // 32-bit command + transfer mode
constexpr uint32_t kResponse          = 0x10;
constexpr uint32_t kBufferPort        = 0x20;
constexpr uint32_t kPresentState      = 0x24;
constexpr uint32_t kHostControl1      = 0x28;
constexpr uint32_t kClockControl      = 0x2C;
constexpr uint32_t kTimeoutControl    = 0x2E;
constexpr uint32_t kNormalIntStatus   = 0x30;
constexpr uint32_t kErrorIntStatus    = 0x32;
constexpr uint32_t kNormalIntEnable   = 0x34;   // 32-bit write covers 0x34/0x36
constexpr uint32_t kNormalIntSignal   = 0x38;   // 32-bit write covers 0x38/0x3A
constexpr uint32_t kHostControl2      = 0x3E;
constexpr uint32_t kCapabilities      = 0x40;
constexpr uint32_t kAdmaErrorStatus   = 0x54;
constexpr uint32_t kAdmaAddressLow    = 0x58;
constexpr uint32_t kAdmaAddressHigh   = 0x5C;
constexpr uint32_t kVersion           = 0xFE;
constexpr uint32_t kVendorClock       = 0x204;

constexpr uint32_t kPresentCmdInhibit = 1U << 0;
constexpr uint32_t kPresentDatInhibit = 1U << 1;
constexpr uint32_t kIntBufferReadReady = 1U << 5;
constexpr uint32_t kIntError           = 1U << 15;
constexpr uint32_t kIntCommandComplete = 1U << 0;
constexpr uint32_t kIntTransferComplete = 1U << 1;

constexpr uint32_t kBlockBytes         = 512;
constexpr uint32_t kCommandDataPresent = 0x003A0000U;  // RESP_48 | CRC | index | data
constexpr uint32_t kTransferModeRead   = 0x0011U;      // DMA enable + data read
constexpr uint32_t kDescriptorValid    = 0x23U;        // Valid | Tran | End

constexpr uintptr_t kCacheLineBytes = 64;

inline void dmaBarrier()
{
    __asm__ volatile("dmb osh" ::: "memory");
}

inline void cacheClean(const void *address, size_t bytes)
{
    uintptr_t line = reinterpret_cast<uintptr_t>(address) & ~(kCacheLineBytes - 1);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(address) + bytes + kCacheLineBytes - 1) &
                          ~(kCacheLineBytes - 1);
    for (; line < end; line += kCacheLineBytes) {
        __asm__ volatile("dc cvac, %0" : : "r"(line) : "memory");
    }
    __asm__ volatile("dsb sy" : : : "memory");
}

inline void cacheInvalidate(const void *address, size_t bytes)
{
    uintptr_t line = reinterpret_cast<uintptr_t>(address) & ~(kCacheLineBytes - 1);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(address) + bytes + kCacheLineBytes - 1) &
                          ~(kCacheLineBytes - 1);
    for (; line < end; line += kCacheLineBytes) {
        __asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
    }
    __asm__ volatile("dsb sy" : : : "memory");
}

} // namespace

class NativeSdhciBlock : public IOService {
    OSDeclareDefaultStructors(NativeSdhciBlock)

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    IOMemoryMap *mapping = nullptr;
    IOBufferMemoryDescriptor *descriptorMemory = nullptr;
    IOBufferMemoryDescriptor *transferMemory = nullptr;
    IOLock *gate = nullptr;
    uintptr_t registers = 0;
    uint64_t descriptorPhysical = 0;
    uint64_t transferPhysical = 0;
    uint64_t capacity = 0;             // sectors of the userdata window
    uint64_t partitionFirstLba = 0;    // eMMC sector of window sector 0
    bool online = false;
    bool ownsDevice = false;
    uint32_t lastStatus = 0;
    uint32_t lastPresentState = 0;
    uint16_t lastError = 0;
    uint8_t lastAdmaError = 0;

    uint32_t read32(uint32_t offset) const
    {
        return *reinterpret_cast<volatile uint32_t *>(registers + offset);
    }
    uint16_t read16(uint32_t offset) const
    {
        return *reinterpret_cast<volatile uint16_t *>(registers + offset);
    }
    uint8_t read8(uint32_t offset) const
    {
        return *reinterpret_cast<volatile uint8_t *>(registers + offset);
    }
    void write32(uint32_t offset, uint32_t value)
    {
        *reinterpret_cast<volatile uint32_t *>(registers + offset) = value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    void write16(uint32_t offset, uint16_t value)
    {
        *reinterpret_cast<volatile uint16_t *>(registers + offset) = value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    void write8(uint32_t offset, uint8_t value)
    {
        *reinterpret_cast<volatile uint8_t *>(registers + offset) = value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }

    bool waitCommandIdle(uint32_t milliseconds);
    bool dataCommand(uint32_t commandIndex, uint64_t argument);
    bool readSector(uint64_t emmcLba);
    bool readExtCsd();
    bool readGptHeader();
    bool findUserdataPartition();
    const uint8_t *sectorData() const
    {
        return static_cast<const uint8_t *>(transferMemory->getBytesNoCopy());
    }
};

OSDefineMetaClassAndStructors(NativeSdhciBlock, IOService)

IOService *NativeSdhciBlock::probe(IOService *provider, SInt32 *score)
{
    if (!provider) {
        return nullptr;
    }
    // The personality matches the ADT compatible string; accept only a node that
    // really is this controller, so a renamed device tree cannot attach the
    // driver to an unrelated platform device.
    OSData *compatible = OSDynamicCast(OSData, provider->getProperty("compatible"));
    bool matched = false;
    if (compatible) {
        const char *bytes = static_cast<const char *>(compatible->getBytesNoCopy());
        size_t remaining = compatible->getLength();
        while (bytes && remaining) {
            size_t length = 0;
            while (length < remaining && bytes[length] != '\0') {
                ++length;
            }
            if ((length == sizeof("unixnu,sdhci-block") - 1 &&
                 memcmp(bytes, "unixnu,sdhci-block", length) == 0) ||
                (length == sizeof("sprd,sdhc-r11p1") - 1 &&
                 memcmp(bytes, "sprd,sdhc-r11p1", length) == 0)) {
                matched = true;
                break;
            }
            if (length >= remaining) {
                break;
            }
            bytes += length + 1;
            remaining -= length + 1;
        }
    }
    if (!matched) {
        return nullptr;
    }
    return IOService::probe(provider, score);
}

bool NativeSdhciBlock::waitCommandIdle(uint32_t milliseconds)
{
    for (uint32_t waited = 0; waited < milliseconds; ++waited) {
        if ((read32(kPresentState) & (kPresentCmdInhibit | kPresentDatInhibit)) == 0) {
            return true;
        }
        IODelay(1000);
    }
    return false;
}

bool NativeSdhciBlock::dataCommand(uint32_t commandIndex, uint64_t argument)
{
    if (!ownsDevice || !registers) {
        return false;
    }
    uint32_t *descriptor = static_cast<uint32_t *>(descriptorMemory->getBytesNoCopy());
    uint8_t *data = static_cast<uint8_t *>(transferMemory->getBytesNoCopy());

    descriptor[0] = (kBlockBytes << 16) | kDescriptorValid;
    descriptor[1] = static_cast<uint32_t>(transferPhysical);
    descriptor[2] = static_cast<uint32_t>(transferPhysical >> 32);
    descriptor[3] = 0;
    cacheClean(descriptor, 4 * sizeof(uint32_t));
    cacheInvalidate(data, kBlockBytes);

    // Stock sdhcr11_prepare_data: select the vendor ADMA engine, ADMA2 with
    // 64-bit descriptors, and clear the vendor bit that forces 32-bit mode.
    write8(kHostControl1, static_cast<uint8_t>((read8(kHostControl1) & ~0x18U) | 0x10U));
    write16(kHostControl2, static_cast<uint16_t>((read16(kHostControl2) & ~0x0400U) |
                                                 0x0800U | 0x2000U));

    // 0x34/0x36 (status enable) and 0x38/0x3A (signal enable) are written as
    // 32-bit pairs: a 32-bit write at 0x3A would also hit vendor register 0x3C,
    // so only the 0x34 and 0x38 windows are used.
    write32(kNormalIntSignal, 0);   // polling only: keep the interrupt line quiet
    write32(kNormalIntEnable, 0);
    write32(kNormalIntStatus, 0xFFFFFFFFU);
    write8(kTimeoutControl, 0x0E);

    if (!waitCommandIdle(2000)) {
        lastPresentState = read32(kPresentState);
        IOLog("NativeSdhciBlock: command %u inhibited, present=0x%08x\n",
              commandIndex, lastPresentState);
        return false;
    }

    write16(kBlockSize, static_cast<uint16_t>((kBlockBytes & 0xFFFU) | 0x7000U));
    write32(kBlockCount, 1);
    write32(kAdmaAddressLow, static_cast<uint32_t>(descriptorPhysical));
    write32(kAdmaAddressHigh, static_cast<uint32_t>(descriptorPhysical >> 32));
    write32(kArgument, static_cast<uint32_t>(argument));
    // Status enable only: command/transfer complete must be observable in 0x30.
    write32(kNormalIntEnable, 0xFFFFFFFFU);
    write32(kNormalIntSignal, 0);
    dmaBarrier();

    write32(kTransferMode, ((commandIndex & 0x3FU) << 24) | kCommandDataPresent | kTransferModeRead);

    uint32_t pioWords = 0;
    uint32_t pioBuffer[128] = {};
    bool finished = false;
    for (uint32_t spin = 0; spin < 200000; ++spin) {
        const uint32_t status = read32(kNormalIntStatus);
        if ((status & kIntBufferReadReady) != 0) {
            if (pioWords < 128) {
                pioBuffer[pioWords] = read32(kBufferPort);
            }
            ++pioWords;
            write32(kNormalIntStatus, kIntBufferReadReady);
            continue;
        }
        if (((status & kIntError) != 0) ||
            ((status & (kIntCommandComplete | kIntTransferComplete)) ==
             (kIntCommandComplete | kIntTransferComplete))) {
            finished = true;
            break;
        }
        IODelay(10);
    }

    lastStatus = read32(kNormalIntStatus);
    lastPresentState = read32(kPresentState);
    const uint32_t response = read32(kResponse);
    lastError = read16(kErrorIntStatus);
    lastAdmaError = read8(kAdmaErrorStatus);
    write32(kNormalIntStatus, 0xFFFFFFFFU);
    write32(kNormalIntSignal, 0);
    write32(kNormalIntEnable, 0);
    dmaBarrier();
    cacheInvalidate(data, kBlockBytes);

    if (pioWords != 0) {
        const uint32_t words = (pioWords > 128) ? 128 : pioWords;
        memcpy(data, pioBuffer, words * sizeof(uint32_t));
        return true;
    }
    if (!finished || (lastStatus & kIntError) != 0 ||
        (lastStatus & (kIntCommandComplete | kIntTransferComplete)) !=
            (kIntCommandComplete | kIntTransferComplete)) {
        IOLog("NativeSdhciBlock: cmd %u lba 0x%llx failed status=0x%08x err=0x%04x adma=0x%02x ps=0x%08x resp=0x%08x\n",
              commandIndex, argument, lastStatus, static_cast<unsigned>(lastError),
              static_cast<unsigned>(lastAdmaError), lastPresentState, response);
        return false;
    }
    return true;
}

bool NativeSdhciBlock::readSector(uint64_t emmcLba)
{
    if (!dataCommand(17, emmcLba)) {
        __atomic_fetch_add(&native_sdhci_read_failures, 1, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_fetch_add(&native_sdhci_read_requests, 1, __ATOMIC_RELAXED);
    return true;
}

bool NativeSdhciBlock::readExtCsd()
{
    if (!dataCommand(8, 0)) {
        IOLog("NativeSdhciBlock: CMD8 SEND_EXT_CSD failed\n");
        return false;
    }
    const uint8_t *data = sectorData();
    uint32_t sectors = 0;
    memcpy(&sectors, data + 212, sizeof(sectors));
    native_sdhci_ext_csd_sectors = sectors;
    if (sectors != 0 && sectors != 0xFFFFFFFFU) {
        native_sdhci_ext_csd_ok = 1;
    }
    IOLog("NativeSdhciBlock: EXT_CSD rev=0x%02x sec_count=0x%x (%llu MiB)\n",
          static_cast<unsigned>(data[192]), sectors,
          (static_cast<uint64_t>(sectors) * kBlockBytes) >> 20);
    return native_sdhci_ext_csd_ok != 0;
}

bool NativeSdhciBlock::readGptHeader()
{
    if (!readSector(1)) {
        return false;
    }
    if (memcmp(sectorData(), "EFI PART", 8) != 0) {
        IOLog("NativeSdhciBlock: LBA 1 is not a GPT header\n");
        return false;
    }
    uint64_t firstUsable = 0;
    uint64_t lastUsable = 0;
    uint64_t alternateLba = 0;
    uint64_t entryLba = 0;
    uint32_t entryCount = 0;
    uint32_t entrySize = 0;
    memcpy(&alternateLba, sectorData() + 32, sizeof(alternateLba));
    memcpy(&firstUsable, sectorData() + 40, sizeof(firstUsable));
    memcpy(&lastUsable, sectorData() + 48, sizeof(lastUsable));
    memcpy(&entryLba, sectorData() + 72, sizeof(entryLba));
    memcpy(&entryCount, sectorData() + 80, sizeof(entryCount));
    memcpy(&entrySize, sectorData() + 84, sizeof(entrySize));
    IOLog("NativeSdhciBlock: GPT first=0x%llx last=0x%llx alt=0x%llx entries@0x%llx count=%u size=%u\n",
          firstUsable, lastUsable, alternateLba, entryLba, entryCount, entrySize);
    return true;
}

bool NativeSdhciBlock::findUserdataPartition()
{
    uint64_t entryLba = 0;
    uint32_t entryCount = 0;
    uint32_t entrySize = 0;
    memcpy(&entryLba, sectorData() + 72, sizeof(entryLba));
    memcpy(&entryCount, sectorData() + 80, sizeof(entryCount));
    memcpy(&entrySize, sectorData() + 84, sizeof(entrySize));
    if (entrySize < 128 || entrySize > kBlockBytes || entryLba == 0) {
        IOLog("NativeSdhciBlock: unusable GPT entry array (lba 0x%llx size %u)\n", entryLba, entrySize);
        return false;
    }
    const uint32_t entriesPerSector = kBlockBytes / entrySize;
    const uint32_t sectors = (entryCount + entriesPerSector - 1) / entriesPerSector;
    for (uint32_t sector = 0; sector < sectors; ++sector) {
        if (!readSector(entryLba + sector)) {
            return false;
        }
        for (uint32_t slot = 0; slot < entriesPerSector; ++slot) {
            const uint32_t index = sector * entriesPerSector + slot;
            if (index >= entryCount) {
                break;
            }
            const uint8_t *entry = sectorData() + slot * entrySize;
            bool empty = true;
            for (unsigned byte = 0; byte < 16; ++byte) {
                if (entry[byte] != 0) {
                    empty = false;
                    break;
                }
            }
            if (empty) {
                continue;
            }
            char name[37] = {};
            for (unsigned character = 0; character < 36; ++character) {
                uint16_t code = 0;
                memcpy(&code, entry + 56 + character * 2, sizeof(code));
                if (code == 0) {
                    break;
                }
                name[character] = (code < 0x80) ? static_cast<char>(code) : '?';
            }
            uint64_t first = 0;
            uint64_t last = 0;
            memcpy(&first, entry + 32, sizeof(first));
            memcpy(&last, entry + 40, sizeof(last));
            IOLog("NativeSdhciBlock: GPT [%02u] %s first=0x%llx last=0x%llx blocks=0x%llx (%llu MiB)\n",
                  index, name, first, last, last - first + 1,
                  ((last - first + 1) * kBlockBytes) >> 20);
            if (strcmp(name, "userdata") == 0 && last >= first) {
                native_sdhci_userdata_first = first;
                native_sdhci_userdata_last = last;
                native_sdhci_gpt_userdata = 1;
                partitionFirstLba = first;
                capacity = last - first + 1;
            }
        }
    }
    if (native_sdhci_gpt_userdata == 0) {
        IOLog("NativeSdhciBlock: no userdata partition in the GPT\n");
        return false;
    }
    return true;
}

bool NativeSdhciBlock::start(IOService *provider)
{
    if (!provider || !IOService::start(provider)) {
        return false;
    }
    if (provider->getResources() != kIOReturnSuccess) {
        IOLog("NativeSdhciBlock: the platform device has no memory resource\n");
        return false;
    }
    mapping = provider->mapDeviceMemoryWithIndex(0, kIOMapAnywhere | kIOMapInhibitCache);
    if (!mapping || mapping->getLength() < 0x1000) {
        IOLog("NativeSdhciBlock: cannot map the controller registers\n");
        return false;
    }
    registers = static_cast<uintptr_t>(mapping->getVirtualAddress());
    ownsDevice = true;

    const uint32_t capabilities = read32(kCapabilities);
    const uint16_t version = read16(kVersion);
    const uint16_t clock = read16(kClockControl);
    const uint8_t hostControl = read8(kHostControl1);
    const uint16_t hostControl2 = read16(kHostControl2);
    const uint32_t vendorClock = read32(kVendorClock);
    native_sdhci_mmio_physical = mapping->getPhysicalAddress();
    IOLog("NativeSdhciBlock: SDHCR11 phys=0x%llx CAP=0x%08x version=0x%04x clk=0x%04x hc=0x%02x hc2=0x%04x vendor204=0x%08x ps=0x%08x\n",
          static_cast<uint64_t>(mapping->getPhysicalAddress()), capabilities,
          static_cast<unsigned>(version), static_cast<unsigned>(clock),
          static_cast<unsigned>(hostControl), static_cast<unsigned>(hostControl2),
          vendorClock, read32(kPresentState));

    descriptorMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) |
            static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), 4096, 4096);
    transferMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) |
            static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), 4096, 4096);
    if (!descriptorMemory || !transferMemory) {
        IOLog("NativeSdhciBlock: DMA buffer allocation failed\n");
        return false;
    }
    IOByteCount length = 0;
    descriptorPhysical = descriptorMemory->getPhysicalSegment(0, &length);
    if (descriptorPhysical == 0 || length < 4 * sizeof(uint32_t)) {
        IOLog("NativeSdhciBlock: the descriptor table is not physically contiguous\n");
        return false;
    }
    transferPhysical = transferMemory->getPhysicalSegment(0, &length);
    if (transferPhysical == 0 || length < kBlockBytes) {
        IOLog("NativeSdhciBlock: the transfer buffer is not physically contiguous\n");
        return false;
    }
    bzero(descriptorMemory->getBytesNoCopy(), 4096);
    bzero(transferMemory->getBytesNoCopy(), 4096);
    gate = IOLockAlloc();
    if (!gate) {
        return false;
    }

    readExtCsd();
    if (!readGptHeader() || !findUserdataPartition()) {
        IOLog("NativeSdhciBlock: eMMC read path not proven, nothing published\n");
        return false;
    }

    online = true;
    native_sdhci_ready = 1;
    native_sdhci_sectors = capacity;
    IOLog("NativeSdhciBlock: userdata first=0x%llx last=0x%llx sectors=0x%llx bytes=%llu\n",
          partitionFirstLba, native_sdhci_userdata_last, capacity,
          capacity * static_cast<uint64_t>(kBlockBytes));
    IOLog("NativeSdhciBlock: eMMC read path ready, block device publication is the next milestone\n");
    registerService();
    return true;
}

void NativeSdhciBlock::stop(IOService *provider)
{
    // Never reset the controller: cboot's state is what makes it work, and a
    // reset would also take the bootloader's card initialisation away.
    online = false;
    IOService::stop(provider);
}

void NativeSdhciBlock::free()
{
    if (transferMemory) {
        transferMemory->release();
        transferMemory = nullptr;
    }
    if (descriptorMemory) {
        descriptorMemory->release();
        descriptorMemory = nullptr;
    }
    if (mapping) {
        mapping->release();
        mapping = nullptr;
    }
    if (gate) {
        IOLockFree(gate);
        gate = nullptr;
    }
    registers = 0;
    ownsDevice = false;
    IOService::free();
}
