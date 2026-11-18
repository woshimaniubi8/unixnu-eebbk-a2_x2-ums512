"""Compile the actual service against a narrow host lifecycle test double.

Checks the open/answer boundaries, not real IOKit registration. Without aks every open is refused (closed boundary);
with it the user client also wraps and unwraps keys (10, 11) in a form checked against hashlib here, and answers the handshake/bookkeeping selectors, get_system (14), lock state (7) and device state (17, 35; decoded below the way libaks decodes it) and rejects the rest.
"""
from pathlib import Path
import hashlib
import subprocess
import tempfile

root = Path(__file__).resolve().parent
# The on-disk blob format: existing keychain items depend on it, so the test pins it independently of the C++ code.
identity = bytes(range(1, 9))
device_key = hashlib.sha256(b"UniXNU AppleKeyStore v1" + identity).digest()
plain_key = bytes(range(40))
tag = hashlib.sha256(device_key + b"T" + plain_key).digest()[:8]
stream = b"".join(hashlib.sha256(device_key + b"S" + tag + i.to_bytes(4, "little")).digest() for i in range(2))
expected_blob = (tag + bytes(a ^ b for a, b in zip(plain_key, stream))).hex()
expected_uuid = hashlib.sha256(device_key + b"U").hexdigest()[:32]
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / "IOKit").mkdir()
    (work / "IOKit/IOService.h").write_text(r'''
#pragma once
#include <cstdint>
#include <cstddef>
using UInt32 = uint32_t;
using task_t = void *;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnNotReady = -7;
constexpr IOReturn kIOReturnBadArgument = -8;
constexpr IOReturn kIOReturnNoSpace = -9;
constexpr IOReturn kIOReturnNoMemory = -10;
constexpr IOReturn kIOReturnError = -11;
class OSDictionary {};
class OSObject { public: virtual ~OSObject() = default; void release() { delete this; } };
class IOMemoryDescriptor {
public:
    uint8_t bytes[512]; size_t length = 512;
    size_t getLength() const { return length; }
    size_t writeBytes(size_t offset, const void *p, size_t n) { for (size_t i = 0; i < n; ++i) bytes[offset + i] = ((const uint8_t *)p)[i]; return n; }
};
struct IOExternalMethodArguments {
    const uint64_t *scalarInput; uint32_t scalarInputCount; uint64_t *scalarOutput; uint32_t scalarOutputCount;
    void *structureOutput; uint32_t structureOutputSize;
    IOMemoryDescriptor *structureOutputDescriptor = nullptr; uint32_t structureOutputDescriptorSize = 0;
    const void *structureInput = nullptr; uint32_t structureInputSize = 0;
};
struct IOExternalMethodDispatch {};
class IOService;
class IOUserClient : public OSObject {
public:
    bool terminated = false, started = false;
    virtual bool initWithTask(task_t, void *, UInt32, OSDictionary *) { return true; }
    bool attach(IOService *) { return true; }
    void detach(IOService *) {}
    virtual bool start(IOService *) { started = true; return true; }
    void terminate() { terminated = true; }
    virtual IOReturn clientClose() { return kIOReturnSuccess; }
    virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch * = nullptr,
                                    OSObject * = nullptr, void * = nullptr) = 0;
};
#define OSDeclareDefaultStructors(name)
#define OSDefineMetaClassAndStructors(name, base)
class IOService : public OSObject {
public:
    bool baseOK = true;
    unsigned properties = 0, failProperty = 0, publications = 0, stops = 0;
    virtual bool start(IOService *) { return baseOK; }
    virtual void stop(IOService *) { ++stops; }
    void setName(const char *) {}
    bool setProperty(const char *, bool) { return ++properties != failProperty; }
    void registerService() { ++publications; }
    virtual IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) = 0;
    virtual IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **) = 0;
};
''')
    (work / "IOKit/IOUserClient.h").write_text("#pragma once\n#include <IOKit/IOService.h>\n")
    (work / "test.cpp").write_text(r'''
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
static bool enabled;
static unsigned refused, opened, calls;
static uint32_t lockState;
extern "C" bool native_aks_state_enabled() { return enabled; }
extern "C" uint32_t native_aks_lock_state() { return lockState; }
extern "C" void native_keyevents_open_refused() { ++refused; }
extern "C" void native_aks_opened() { ++opened; }
extern "C" void native_aks_call(uint32_t, int32_t) { ++calls; }
static bool haveIdentity = true;
extern "C" size_t native_aks_device_id(uint8_t *out, size_t capacity) {
    assert(capacity >= 8);
    if (!haveIdentity) return 0;
    for (unsigned i = 0; i < 8; ++i) out[i] = (uint8_t)(i + 1);
    return 8;
}
static std::string hex(const uint8_t *p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}
#include "NativeKeyStoreEvents.cpp"

// The decoder libaks uses (_der_dict_iterate/_der_get_number): SET { SEQUENCE { UTF8String key, INTEGER } ... }.
static bool decode(const uint8_t *p, size_t n, std::map<std::string, int64_t> &out) {
    size_t i = 0;
    auto len = [&](size_t &l) { if (i >= n) return false; uint8_t b = p[i++]; if (b < 0x80) { l = b; return true; }
        if (b != 0x81 || i >= n) return false; l = p[i++]; return true; };
    if (n < 2 || p[i++] != 0x31) return false;
    size_t total; if (!len(total) || i + total != n) return false;
    while (i < n) {
        if (p[i++] != 0x30) return false;
        size_t sl; if (!len(sl) || i + sl > n) return false;
        size_t end = i + sl;
        if (p[i++] != 0x0c) return false;
        size_t kl; if (!len(kl) || i + kl > end) return false;
        std::string key((const char *)p + i, kl); i += kl;
        if (p[i++] != 0x02) return false;
        size_t il; if (!len(il) || i + il != end || il == 0 || il > 8) return false;
        int64_t v = (int8_t)p[i]; for (size_t k = 1; k < il; ++k) v = (v << 8) | p[i + k];
        i = end; out[key] = v;
    }
    return true;
}
int main() {
    auto *device = static_cast<AppleKeyStore *>(native_keyevents_allocate());
    IOService *base = device;
    assert(base->start(nullptr) && device->publications == 1);
    IOUserClient sentinel2 = *(IOUserClient *)nullptr;
}
'''.replace('    IOUserClient sentinel2 = *(IOUserClient *)nullptr;\n', '''
    struct Probe : IOUserClient { IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *) override { return 0; } };
    Probe sentinel;
    // Closed boundary (default): every open is refused and clears the output.
    for (UInt32 type : {0u, 1u, 7u, 0xffffffffu}) {
        IOUserClient *out = &sentinel;
        assert(base->newUserClient(nullptr, nullptr, type, nullptr, &out) == kIOReturnNotReady && !out);
        out = &sentinel;
        assert(base->newUserClient(nullptr, nullptr, type, &out) == kIOReturnNotReady && !out);
        assert(base->newUserClient(nullptr, nullptr, type, nullptr, nullptr) == kIOReturnNotReady);
    }
    assert(refused == 12 && !opened);
    // State mode: the client opens and answers only the state queries.
    enabled = true;
    IOUserClient *client = nullptr;
    assert(base->newUserClient(nullptr, nullptr, 0, nullptr, &client) == kIOReturnSuccess && client && client->started);
    assert(opened == 1);
    uint64_t in[1] = {0}, outScalar[1] = {0};
    IOExternalMethodArguments a{in, 1, outScalar, 1, nullptr, 0};
    assert(client->externalMethod(7, &a) == kIOReturnSuccess && outScalar[0] == 4);
    a.scalarInputCount = 2; assert(client->externalMethod(7, &a) == kIOReturnBadArgument);
    // get_system hands out the system bag handle 0; handshake and bookkeeping selectors succeed without outputs.
    outScalar[0] = 99; IOExternalMethodArguments sys{in, 0, outScalar, 1, nullptr, 0};
    assert(client->externalMethod(14, &sys) == kIOReturnSuccess && outScalar[0] == 0 && sys.scalarOutputCount == 1);
    IOExternalMethodArguments nosys{in, 0, nullptr, 0, nullptr, 0};
    assert(client->externalMethod(14, &nosys) == kIOReturnBadArgument);
    for (uint32_t sel : {0u, 4u, 5u, 9u, 12u, 13u, 26u, 27u, 55u, 101u, 102u}) {
        IOExternalMethodArguments h{nullptr, 0, nullptr, 0, nullptr, 0};
        assert(client->externalMethod(sel, &h) == kIOReturnSuccess);
    }
    uint8_t buffer[0x1000];
    for (uint32_t want : {0u, 3u}) {
        lockState = want;
        for (uint32_t sel : {17u, 35u}) {
            IOExternalMethodArguments s{in, 1, nullptr, 0, buffer, sizeof buffer};
            assert(client->externalMethod(sel, &s) == kIOReturnSuccess && s.structureOutputSize > 20 && s.structureOutputSize < 200);
            std::map<std::string, int64_t> state;
            assert(decode(buffer, s.structureOutputSize, state));
            assert(state.size() == 10 && state["sls"] == want && state["ss"] == 4 && state["bh"] == 0 && state.count("sgpe") && state.count("srcd"));
            // Large outputs come through a descriptor.
            IOMemoryDescriptor descriptor;
            IOExternalMethodArguments d{in, 1, nullptr, 0, nullptr, 0, &descriptor, 0};
            assert(client->externalMethod(sel, &d) == kIOReturnSuccess && d.structureOutputDescriptorSize == s.structureOutputSize);
            std::map<std::string, int64_t> viaDescriptor;
            assert(decode(descriptor.bytes, d.structureOutputDescriptorSize, viaDescriptor) && viaDescriptor == state);
            IOMemoryDescriptor tiny; tiny.length = 10;
            IOExternalMethodArguments td{in, 1, nullptr, 0, nullptr, 0, &tiny, 0};
            assert(client->externalMethod(sel, &td) == kIOReturnNoSpace);
            IOExternalMethodArguments none{nullptr, 0, nullptr, 0, buffer, sizeof buffer};
            assert(client->externalMethod(sel, &none) == kIOReturnSuccess);
            IOExternalMethodArguments small{in, 1, nullptr, 0, buffer, 10};
            assert(client->externalMethod(sel, &small) == kIOReturnNoSpace);
            IOExternalMethodArguments bad{in, 1, outScalar, 1, buffer, sizeof buffer};
            assert(client->externalMethod(sel, &bad) == kIOReturnBadArgument);
        }
    }
    // Everything else is unsupported (keys, bags, passcodes are not provided); a missing argument block is an error.
    for (uint32_t sel : {1u, 2u, 3u, 6u, 8u, 15u, 16u, 18u, 36u, 43u, 73u, 1000u}) {
        IOExternalMethodArguments x{in, 1, outScalar, 1, buffer, sizeof buffer};
        assert(client->externalMethod(sel, &x) == (IOReturn)0xe00002c7);
    }
    assert(client->externalMethod(7, nullptr) == (IOReturn)0xe00002bc);
    // Without an identity no key is derived, and wrapping reports the plain AKS error.
    {
        haveIdentity = false;
        uint64_t wrapIn[2] = {0, 7}, wrapOut[1] = {0};
        uint8_t key[40], blob[64];
        for (unsigned i = 0; i < sizeof key; ++i) key[i] = (uint8_t)i;
        IOExternalMethodArguments w{wrapIn, 2, wrapOut, 1, blob, sizeof blob, nullptr, 0, key, sizeof key};
        assert(client->externalMethod(10, &w) == (IOReturn)0xe00002bc);
        haveIdentity = true;
        // The blob matches the independently computed format and the class comes back unchanged.
        w = IOExternalMethodArguments{wrapIn, 2, wrapOut, 1, blob, sizeof blob, nullptr, 0, key, sizeof key};
        assert(client->externalMethod(10, &w) == kIOReturnSuccess && w.structureOutputSize == 48 && wrapOut[0] == 7);
        assert(hex(blob, 48) == EXPECTED_BLOB);
        uint8_t opened_key[64] = {};
        IOExternalMethodArguments u{wrapIn, 2, nullptr, 0, opened_key, sizeof opened_key, nullptr, 0, blob, 48};
        assert(client->externalMethod(11, &u) == kIOReturnSuccess && u.structureOutputSize == 40 && !memcmp(opened_key, key, 40));
        // A different stored class opens the same key; the class is not part of the blob.
        uint64_t otherClass[2] = {0, 11};
        IOExternalMethodArguments v{otherClass, 2, nullptr, 0, opened_key, sizeof opened_key, nullptr, 0, blob, 48};
        assert(client->externalMethod(11, &v) == kIOReturnSuccess);
        // Every changed byte is refused, in the tag and in the ciphertext.
        for (unsigned at : {0u, 7u, 8u, 47u}) {
            blob[at] ^= 1;
            IOExternalMethodArguments bad{wrapIn, 2, nullptr, 0, opened_key, sizeof opened_key, nullptr, 0, blob, 48};
            assert(client->externalMethod(11, &bad) == (IOReturn)0xe00002bc);
            blob[at] ^= 1;
        }
        // Sizes and layouts: 1 and 256 bytes round trip, the rest is refused before any output.
        static uint8_t big[257], wrapped[300], back[300];
        for (size_t n : {size_t(1), size_t(33), size_t(256)}) {
            for (size_t i = 0; i < n; ++i) big[i] = (uint8_t)(i * 3);
            IOExternalMethodArguments r{wrapIn, 2, wrapOut, 1, wrapped, sizeof wrapped, nullptr, 0, big, (uint32_t)n};
            assert(client->externalMethod(10, &r) == kIOReturnSuccess && r.structureOutputSize == n + 8);
            IOExternalMethodArguments q{wrapIn, 2, nullptr, 0, back, sizeof back, nullptr, 0, wrapped, r.structureOutputSize};
            assert(client->externalMethod(11, &q) == kIOReturnSuccess && q.structureOutputSize == n && !memcmp(back, big, n));
        }
        IOExternalMethodArguments tooBig{wrapIn, 2, wrapOut, 1, wrapped, sizeof wrapped, nullptr, 0, big, 257};
        assert(client->externalMethod(10, &tooBig) == kIOReturnBadArgument);
        IOExternalMethodArguments empty{wrapIn, 2, wrapOut, 1, wrapped, sizeof wrapped, nullptr, 0, big, 0};
        assert(client->externalMethod(10, &empty) == kIOReturnBadArgument);
        IOExternalMethodArguments noScalar{wrapIn, 1, wrapOut, 1, wrapped, sizeof wrapped, nullptr, 0, big, 32};
        assert(client->externalMethod(10, &noScalar) == kIOReturnBadArgument);
        IOExternalMethodArguments shortOut{wrapIn, 2, wrapOut, 1, wrapped, 39, nullptr, 0, big, 32};
        assert(client->externalMethod(10, &shortOut) == kIOReturnNoSpace);
        IOExternalMethodArguments shortBlob{wrapIn, 2, nullptr, 0, back, sizeof back, nullptr, 0, wrapped, 8};
        assert(client->externalMethod(11, &shortBlob) == kIOReturnBadArgument);
        // The bag identifier is stable and the bookkeeping selectors succeed.
        uint8_t uuid[16];
        uint64_t bag[1] = {0};
        IOExternalMethodArguments id{bag, 1, nullptr, 0, uuid, sizeof uuid};
        assert(client->externalMethod(23, &id) == kIOReturnSuccess && id.structureOutputSize == 16 && hex(uuid, 16) == EXPECTED_UUID);
        uint64_t generation[1] = {9};
        IOExternalMethodArguments gen{wrapIn, 2, generation, 1, nullptr, 0};
        assert(client->externalMethod(40, &gen) == kIOReturnSuccess && generation[0] == 0);
        IOExternalMethodArguments flow{bag, 1, nullptr, 0, nullptr, 0};
        assert(client->externalMethod(160, &flow) == kIOReturnSuccess);
    }
    assert(client->clientClose() == kIOReturnSuccess && static_cast<IOUserClient *>(client)->terminated);
    assert(calls > 10);
    client->release();
    delete device;
    AppleKeyStore failed;
    failed.baseOK = false;
    assert(!failed.start(nullptr) && !failed.publications && !failed.properties);
    for (unsigned which : {1u, 2u}) {
        AppleKeyStore propertyFailure;
        propertyFailure.failProperty = which;
        assert(!propertyFailure.start(nullptr));
        assert(!propertyFailure.publications && propertyFailure.stops == 1);
    }
''').replace('EXPECTED_BLOB', '"' + expected_blob + '"').replace('EXPECTED_UUID', '"' + expected_uuid + '"'))
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(work), "-I", str(root), str(work / "test.cpp"),
                    "-o", str(work / "test")], check=True)
    subprocess.run([str(work / "test")], check=True)
print("PASS: closed boundary by default; state mode answers state and bookkeeping, wraps and unwraps keys, and rejects every other selector")
