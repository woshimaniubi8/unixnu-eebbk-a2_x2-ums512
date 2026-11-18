// SPDX-License-Identifier: BSD-2-Clause-Patent
// AppleKeyStore for an ephemeral offline fixture: a notification service (the public IOKit matching class used by
// MKBEventsRegister) and, only when aks=1 is set, a user client that answers the state queries of the unchanged
// libaks/MobileKeyBag truthfully for this environment: no keybag, no passcode, nothing locked. Key wrapping is software,
// keyed from the platform identity, so the keychain works and its items survive a restart. Nothing protects that identity:
// this offers no hardware data protection and must not be taken for secure storage. Keybag, passcode and reference-key
// operations stay unsupported.
//
// Answers (selectors and layouts read from the unchanged target's libaks/MobileKeyBag, see docs/touch-nt36532e.md; the set that is
// enough for the system daemons on iOS 16.7 was established on hardware by github.com/dipa-studio/ios-sweet, whose notes document it):
//   0, 4, 5, 9, 12, 13, 26, 27, 55, 101, 102   succeed, no outputs (open handshake, bag/assertion/notify bookkeeping)
//   14       aks_get_system                        scalar out: system keybag handle 0 (keybagd then needs no systembag.kb)
//   7        aks_get_lock_state(device)            scalar out: bit 2 = unlocked since boot
//   17, 35   aks_get_(extended_)device_state       struct out: DER SET{ SEQUENCE{ UTF8String key, INTEGER value } ... }
//            keys ss sls sb sfa sgs sas sgpe srcd sms bh; ss = 4, sls = lock state (0 unlocked; 3 would be "no passcode").
//   10       aks_wrap_key                          scalars in: bag, class; scalar out: class; struct in: key; struct out: blob
//   11       aks_unwrap_key                        scalars in: bag, class; struct in: blob; struct out: key
//   23       aks_get_bag_uuid                      scalar in: bag; struct out: 16 bytes derived from the identity
//   40       aks_generation                        scalars in: bag, operation; scalar out: generation 0
//   160      aks_enable_cache_flow                 succeeds, no outputs
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
extern "C" void native_keyevents_open_refused(void);
extern "C" bool native_aks_state_enabled(void);
extern "C" uint32_t native_aks_lock_state(void);
extern "C" void native_aks_call(uint32_t selector, int32_t result);
extern "C" void native_aks_opened(void);
extern "C" size_t native_aks_device_id(uint8_t *out, size_t capacity);

namespace {
constexpr uint32_t kSelLockState = 7, kSelGetSystem = 14, kSelDeviceState = 17, kSelExtendedDeviceState = 35;
constexpr uint32_t kSelWrapKey = 10, kSelUnwrapKey = 11, kSelBagUuid = 23, kSelGeneration = 40, kSelCacheFlow = 160;
constexpr size_t kTagBytes = 8, kMaxKeyBytes = 256;
constexpr uint64_t kUnlockedSinceBoot = 4;
constexpr uint32_t kStateFlags = 4;
constexpr uint64_t kSystemHandle = 0;
constexpr IOReturn kAksError = (IOReturn)0xe00002bc;   // what libaks reports itself when the connection is missing
constexpr IOReturn kAksUnsupported = (IOReturn)0xe00002c7;

// Self-contained so wrapping does not depend on when the kernel's crypto provider registers.
class Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buffer[64];
    uint64_t total = 0;
    size_t fill = 0;
    static uint32_t rotate(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
    void block(const uint8_t *p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (unsigned i = 0; i < 16; ++i)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (unsigned i = 16; i < 64; ++i) {
            uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t s[8];
        for (unsigned i = 0; i < 8; ++i) s[i] = h[i];
        for (unsigned i = 0; i < 64; ++i) {
            uint32_t t1 = s[7] + (rotate(s[4], 6) ^ rotate(s[4], 11) ^ rotate(s[4], 25)) +
                          ((s[4] & s[5]) ^ (~s[4] & s[6])) + k[i] + w[i];
            uint32_t t2 = (rotate(s[0], 2) ^ rotate(s[0], 13) ^ rotate(s[0], 22)) +
                          ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
            for (unsigned j = 7; j > 0; --j) s[j] = s[j - 1];
            s[4] += t1;
            s[0] = t1 + t2;
        }
        for (unsigned i = 0; i < 8; ++i) h[i] += s[i];
    }
public:
    void update(const void *data, size_t n) {
        const uint8_t *p = (const uint8_t *)data;
        total += n;
        while (n) {
            if (!fill && n >= sizeof buffer) { block(p); p += sizeof buffer; n -= sizeof buffer; continue; }
            size_t take = sizeof buffer - fill < n ? sizeof buffer - fill : n;
            for (size_t i = 0; i < take; ++i) buffer[fill + i] = p[i];
            fill += take; p += take; n -= take;
            if (fill == sizeof buffer) { block(buffer); fill = 0; }
        }
    }
    void final(uint8_t out[32]) {
        uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (fill != 56) update(&zero, 1);
        uint8_t length[8];
        for (unsigned i = 0; i < 8; ++i) length[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(length, 8);
        for (unsigned i = 0; i < 8; ++i)
            for (unsigned j = 0; j < 4; ++j) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - 8 * j));
    }
};

// The same identity gives the same key at every boot, or existing keychain items could not be opened again.
bool deviceKey(uint8_t key[32]) {
    static uint8_t cached[32];
    static volatile bool ready;
    if (!ready) {
        uint8_t id[32];
        size_t n = native_aks_device_id(id, sizeof id);
        if (!n) return false;
        static const char domain[] = "UniXNU AppleKeyStore v1";
        Sha256 hash;
        hash.update(domain, sizeof domain - 1);
        hash.update(id, n);
        hash.final(cached);
        ready = true;
    }
    for (unsigned i = 0; i < 32; ++i) key[i] = cached[i];
    return true;
}
void wrapTag(const uint8_t key[32], const uint8_t *plain, size_t n, uint8_t tag[kTagBytes]) {
    uint8_t digest[32];
    const uint8_t label = 'T';
    Sha256 hash;
    hash.update(key, 32); hash.update(&label, 1); hash.update(plain, n); hash.final(digest);
    for (size_t i = 0; i < kTagBytes; ++i) tag[i] = digest[i];
}
void wrapStream(const uint8_t key[32], const uint8_t tag[kTagBytes], const uint8_t *in, uint8_t *out, size_t n) {
    const uint8_t label = 'S';
    for (size_t at = 0; at < n; at += 32) {
        uint8_t counter[4] = {(uint8_t)(at >> 5), (uint8_t)(at >> 13), (uint8_t)(at >> 21), (uint8_t)(at >> 29)};
        uint8_t digest[32];
        Sha256 hash;
        hash.update(key, 32); hash.update(&label, 1); hash.update(tag, kTagBytes); hash.update(counter, 4);
        hash.final(digest);
        for (size_t i = 0; i < 32 && at + i < n; ++i) out[at + i] = in[at + i] ^ digest[i];
    }
}
IOReturn writeOutput(IOExternalMethodArguments *args, const uint8_t *data, size_t n) {
    if (args->structureOutputDescriptor) {
        if (n > args->structureOutputDescriptor->getLength()) return kIOReturnNoSpace;
        args->structureOutputDescriptor->writeBytes(0, data, n);
        args->structureOutputDescriptorSize = (uint32_t)n;
        return kIOReturnSuccess;
    }
    if (!args->structureOutput || n > args->structureOutputSize) return kIOReturnNoSpace;
    for (size_t i = 0; i < n; ++i) ((uint8_t *)args->structureOutput)[i] = data[i];
    args->structureOutputSize = (uint32_t)n;
    return kIOReturnSuccess;
}
IOReturn wrapKey(IOExternalMethodArguments *args) {
    size_t n = args->structureInputSize;
    if (args->scalarInputCount != 2 || !args->scalarOutput || args->scalarOutputCount != 1 || !args->structureInput ||
        !n || n > kMaxKeyBytes) return kIOReturnBadArgument;
    uint8_t key[32], blob[kTagBytes + kMaxKeyBytes];
    if (!deviceKey(key)) return kAksError;
    wrapTag(key, (const uint8_t *)args->structureInput, n, blob);
    wrapStream(key, blob, (const uint8_t *)args->structureInput, blob + kTagBytes, n);
    IOReturn rc = writeOutput(args, blob, kTagBytes + n);
    if (rc == kIOReturnSuccess) args->scalarOutput[0] = args->scalarInput[1];
    return rc;
}
IOReturn unwrapKey(IOExternalMethodArguments *args) {
    size_t n = args->structureInputSize;
    if (args->scalarInputCount != 2 || args->scalarOutputCount != 0 || !args->structureInput ||
        n <= kTagBytes || n > kTagBytes + kMaxKeyBytes) return kIOReturnBadArgument;
    uint8_t key[32], plain[kMaxKeyBytes], tag[kTagBytes];
    if (!deviceKey(key)) return kAksError;
    const uint8_t *blob = (const uint8_t *)args->structureInput;
    n -= kTagBytes;
    wrapStream(key, blob, blob + kTagBytes, plain, n);
    wrapTag(key, plain, n, tag);
    uint8_t difference = 0;
    for (size_t i = 0; i < kTagBytes; ++i) difference |= (uint8_t)(tag[i] ^ blob[i]);
    return difference ? kAksError : writeOutput(args, plain, n);
}
IOReturn bagUuid(IOExternalMethodArguments *args) {
    uint8_t key[32], digest[32];
    if (!deviceKey(key)) return kAksError;
    const uint8_t label = 'U';
    Sha256 hash;
    hash.update(key, 32); hash.update(&label, 1); hash.final(digest);
    args->scalarOutputCount = 0;
    return writeOutput(args, digest, 16);
}

struct DerWriter {
    uint8_t *p; size_t cap, n = 0; bool ok = true;
    void put(uint8_t b) { if (n < cap) p[n++] = b; else ok = false; }
    void len(size_t l) { if (l < 0x80) put((uint8_t)l); else { put(0x81); put((uint8_t)l); } }   // entries stay below 256 bytes
};
// Smallest two's-complement big-endian INTEGER content for a non-negative value.
size_t derIntegerContent(uint64_t v, uint8_t out[9]) {
    uint8_t raw[8];
    for (int i = 0; i < 8; ++i) raw[i] = (uint8_t)(v >> (56 - 8 * i));
    size_t start = 0;
    while (start < 7 && raw[start] == 0) ++start;
    size_t n = 0;
    if (raw[start] & 0x80) out[n++] = 0;
    while (start < 8) out[n++] = raw[start++];
    return n;
}
size_t entrySize(const char *key, uint64_t value) {
    uint8_t c[9]; size_t kl = 0; while (key[kl]) ++kl;
    size_t body = (2 + kl) + (2 + derIntegerContent(value, c));
    return 2 + body;
}
}  // namespace

// DER SET of SEQUENCE{UTF8String, INTEGER}. Returns the length, or 0 when the buffer is too small.
extern "C" size_t native_aks_encode_state(uint8_t *out, size_t capacity, uint32_t lockState) {
    struct Entry { const char *key; uint64_t value; };
    const Entry entries[] = {{"ss", kStateFlags}, {"sls", lockState}, {"sb", 0}, {"sfa", 0}, {"sgs", 0},
                             {"sas", 0}, {"sgpe", 0}, {"srcd", 0}, {"sms", 0}, {"bh", 0}};
    size_t total = 0;
    for (const Entry &e : entries) total += entrySize(e.key, e.value);
    DerWriter w{out, capacity};
    w.put(0x31); w.len(total);
    for (const Entry &e : entries) {
        uint8_t c[9]; size_t kl = 0; while (e.key[kl]) ++kl;
        size_t il = derIntegerContent(e.value, c);
        w.put(0x30); w.len((2 + kl) + (2 + il));
        w.put(0x0c); w.put((uint8_t)kl); for (size_t i = 0; i < kl; ++i) w.put((uint8_t)e.key[i]);
        w.put(0x02); w.put((uint8_t)il); for (size_t i = 0; i < il; ++i) w.put(c[i]);
    }
    return w.ok ? w.n : 0;
}

class AppleKeyStoreUserClient final : public IOUserClient {
    OSDeclareDefaultStructors(AppleKeyStoreUserClient)
public:
    IOReturn clientClose() override { terminate(); return kIOReturnSuccess; }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
                            OSObject *, void *) override {
        IOReturn rc = kAksUnsupported;
        if (!args) rc = kAksError;
        else switch (selector) {
        // Handshake right after IOServiceOpen (a failure makes libaks drop the connection and reopen on every call), and the
        // bag/assertion/notification calls that need no answer in an environment without keybags.
        case 0: case 4: case 5: case 9: case 12: case 13: case 26: case 27: case 55: case 101: case 102: case kSelCacheFlow:
            args->scalarOutputCount = 0;
            rc = kIOReturnSuccess;
            break;
        case kSelGetSystem:
            if (args->scalarOutputCount < 1 || !args->scalarOutput) { rc = kIOReturnBadArgument; break; }
            args->scalarOutput[0] = kSystemHandle;
            args->scalarOutputCount = 1;
            rc = kIOReturnSuccess;
            break;
        case kSelLockState:
            rc = (args->scalarInputCount == 1 && args->scalarOutputCount == 1 && args->scalarOutput)
                     ? kIOReturnSuccess : kIOReturnBadArgument;
            if (rc == kIOReturnSuccess) args->scalarOutput[0] = kUnlockedSinceBoot;
            break;
        case kSelDeviceState:
        case kSelExtendedDeviceState: {
            if (args->scalarInputCount > 1 || args->scalarOutputCount != 0) { rc = kIOReturnBadArgument; break; }
            uint8_t der[160];
            size_t n = native_aks_encode_state(der, sizeof der, native_aks_lock_state());
            if (!n) { rc = kIOReturnNoSpace; break; }
            rc = writeOutput(args, der, n);
            break;
        }
        case kSelWrapKey: rc = wrapKey(args); break;
        case kSelUnwrapKey: rc = unwrapKey(args); break;
        case kSelBagUuid: rc = bagUuid(args); break;
        case kSelGeneration:
            if (args->scalarInputCount != 2 || !args->scalarOutput || args->scalarOutputCount != 1) { rc = kIOReturnBadArgument; break; }
            args->scalarOutput[0] = 0;
            rc = kIOReturnSuccess;
            break;
        default: break;
        }
        native_aks_call(selector, (int32_t)rc);
        return rc;
    }
};
OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient)

class AppleKeyStore final : public IOService {
    OSDeclareDefaultStructors(AppleKeyStore)
    IOReturn open(task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out) {
        if (out) *out = nullptr;
        if (!native_aks_state_enabled() || !out) { native_keyevents_open_refused(); return kIOReturnNotReady; }
        AppleKeyStoreUserClient *client = new AppleKeyStoreUserClient;
        if (!client) return kIOReturnNoMemory;
        if (!client->initWithTask(task, security, type, properties) || !client->attach(this)) {
            client->release(); return kIOReturnError;
        }
        if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
        native_aks_opened();
        *out = client;
        return kIOReturnSuccess;
    }
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        setName("Native keystore state service");
        if (!setProperty("NativeKeyStoreEvents", true) ||
            !setProperty("KeyOperationsAvailable", true)) {
            IOService::stop(provider);
            return false;
        }
        registerService();
        return true;
    }
    IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override {
        return open(t, s, type, p, out);
    }
    IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override {
        return open(t, s, type, nullptr, out);
    }
};
OSDefineMetaClassAndStructors(AppleKeyStore, IOService)
extern "C" IOService *native_keyevents_allocate(void) { return new AppleKeyStore; }
