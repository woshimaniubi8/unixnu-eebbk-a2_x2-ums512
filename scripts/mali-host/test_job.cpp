/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliJobChain against golden words from the pinned Mesa XML.
 *
 * There is no hardware here and no MMIO: the chain is built in a plain host
 * buffer and every field is compared with the bit positions in
 * third_party/mesa-panfrost/v7.xml:1334-1399. The chaining rule is
 * pan_jc_add_job() (pan_jc.h): index = ++job_index, and the previous job's Next
 * is pointed at the new one, so the last job keeps Next = 0. */
#include "NativeMaliJob.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

constexpr size_t kSlot = NativeMaliJobChain::SlotBytes;

/* A 64-byte-aligned buffer, filled with a non-zero pattern so any byte the
 * builder forgets to write shows up in the padding checks. */
class Buffer {
public:
    std::vector<uint8_t> storage;
    explicit Buffer(size_t bytes)
        : storage(bytes + kSlot, 0xcc)
    {
        /* aligned_alloc needs a multiple of the alignment */
        const uintptr_t raw = reinterpret_cast<uintptr_t>(storage.data());
        offset = (kSlot - (raw & (kSlot - 1))) & (kSlot - 1);
    }
    uint8_t *cpu() const
    {
        return const_cast<uint8_t *>(storage.data()) + offset;
    }
    size_t offset = 0;
    static constexpr uint64_t kGpu = UINT64_C(0x100000);
};

uint32_t word(const uint8_t *slot, size_t index)
{
    uint32_t value = 0;
    std::memcpy(&value, slot + index * 4, sizeof(value));
    return value;
}

uint64_t dword(const uint8_t *slot, size_t index)
{
    return uint64_t(word(slot, index)) | (uint64_t(word(slot, index + 1)) << 32);
}

void testPrepareRefusals()
{
    Buffer buffer(4 * kSlot);
    NativeMaliJobChain chain;

    CHECK(chain.addWriteValue(NativeMaliWriteValueZero, 0x1000, 0, nullptr) ==
        kIOReturnNotReady);
    CHECK(!chain.prepared());
    CHECK(chain.firstJob() == 0);

    CHECK(chain.prepare(nullptr, Buffer::kGpu, kSlot) == kIOReturnBadArgument);
    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu, 0) == kIOReturnBadArgument);
    /* Not 64-byte aligned: the XML's align=64 is the only placement constraint. */
    CHECK(chain.prepare(buffer.cpu() + 4, Buffer::kGpu, kSlot) ==
        kIOReturnBadArgument);
    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu + 8, kSlot) ==
        kIOReturnBadArgument);
    /* A partial slot is never written. */
    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu, kSlot - 4) ==
        kIOReturnBadArgument);
    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu, 2 * kSlot + 1) ==
        kIOReturnBadArgument);
    /* gpu + bytes wraps. */
    CHECK(chain.prepare(buffer.cpu(), UINT64_MAX - kSlot + 1, kSlot) ==
        kIOReturnBadArgument);
    CHECK(!chain.prepared());

    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu, 4 * kSlot) ==
        kIOReturnSuccess);
    CHECK(chain.prepared());
    CHECK(chain.capacity() == 4);
    CHECK(chain.jobs() == 0);
    CHECK(chain.firstJob() == 0);
    CHECK(chain.used() == 0);
    CHECK(chain.jobAt(0) == 0);

    /* One address space per builder until reset(). */
    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu, kSlot) == kIOReturnBusy);
    chain.reset();
    CHECK(!chain.prepared());
    CHECK(chain.capacity() == 0);
}

/* One WRITE_VALUE job, every word compared against v7.xml.
 *
 * Job Header (words 0-7): word 4 = Is 64b (bit 0) | Type 2 << 1 (bits 1:7) |
 * Index 1 << 16. The type is "Write value" = 2 (v7.xml:40) and the index is
 * ++job_index, so a first job is 1. Everything else is 0.
 * Write Value Job Payload at offset 32: Address (words 8-9), Type (word 10),
 * unused word 11, Immediate Value (words 12-13), unused words 14-15. */
void testFirstWriteValue()
{
    Buffer buffer(4 * kSlot);
    NativeMaliJobChain chain;
    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu, 4 * kSlot) ==
        kIOReturnSuccess);

    uint32_t index = 0;
    CHECK(chain.addWriteValue(NativeMaliWriteValueImmediate32,
        UINT64_C(0x100000200), UINT64_C(0xdeadbeef), &index) == kIOReturnSuccess);
    CHECK(index == 1);
    CHECK(chain.jobs() == 1);
    CHECK(chain.used() == kSlot);
    CHECK(chain.firstJob() == Buffer::kGpu);
    CHECK(chain.jobAt(0) == Buffer::kGpu);
    CHECK(chain.jobAt(1) == 0);

    const uint8_t *job = buffer.cpu();
    CHECK(word(job, 0) == 0);
    CHECK(word(job, 1) == 0);
    CHECK(word(job, 2) == 0);
    CHECK(word(job, 3) == 0);
    CHECK(word(job, 4) == 0x00010005);   /* 64b | Write value << 1 | 1 << 16 */
    CHECK(word(job, 5) == 0);            /* no dependencies */
    CHECK(word(job, 6) == 0);            /* Next = 0 ends the chain */
    CHECK(word(job, 7) == 0);
    CHECK(word(job, 8) == 0x00000200);
    CHECK(word(job, 9) == 0x1);
    CHECK(word(job, 10) == NativeMaliWriteValueImmediate32);
    CHECK(word(job, 11) == 0);           /* payload word 3 is unused */
    CHECK(word(job, 12) == 0xdeadbeef);
    CHECK(word(job, 13) == 0);
    CHECK(word(job, 14) == 0);
    CHECK(word(job, 15) == 0);

    /* The slot is exactly 64 bytes; the next slot must still be untouched. */
    for(size_t i = kSlot; i < 2 * kSlot; ++i)
        CHECK(buffer.cpu()[i] == 0xcc);
}

/* pan_jc_initialize_tiler() sets the address and the type and nothing else, so
 * every non-Immediate type leaves the immediate field at zero, even when the
 * caller passes a value. */
void testImmediateOnlyForImmediateTypes()
{
    Buffer buffer(4 * kSlot);
    NativeMaliJobChain chain;
    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu, 4 * kSlot) ==
        kIOReturnSuccess);

    const uint32_t types[] = {
        NativeMaliWriteValueCycleCounter,
        NativeMaliWriteValueSystemTimestamp,
        NativeMaliWriteValueZero,
    };
    for(size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        uint32_t index = 0;
        CHECK(chain.addWriteValue(types[i], UINT64_C(0x40000000),
            UINT64_C(0xffffffffffffffff), &index) == kIOReturnSuccess);
        CHECK(index == i + 1);
        const uint8_t *job = buffer.cpu() + i * kSlot;
        CHECK(word(job, 10) == types[i]);
        CHECK(word(job, 12) == 0);
        CHECK(word(job, 13) == 0);
        CHECK(dword(job, 8) == UINT64_C(0x40000000));
    }

    /* The Immediate* types store the value, including a full 64-bit one. */
    uint32_t index = 0;
    CHECK(chain.addWriteValue(NativeMaliWriteValueImmediate64,
        UINT64_C(0x40001000), UINT64_C(0x0123456789abcdef), &index) ==
        kIOReturnSuccess);
    const uint8_t *last = buffer.cpu() + 3 * kSlot;
    CHECK(word(last, 12) == 0x89abcdef);
    CHECK(word(last, 13) == 0x01234567);

    /* Unknown types are refused before anything is written. */
    const size_t before = chain.jobs();
    CHECK(chain.addWriteValue(0, 0x40000000, 0, &index) == kIOReturnBadArgument);
    CHECK(chain.addWriteValue(8, 0x40000000, 0, &index) == kIOReturnBadArgument);
    CHECK(chain.jobs() == before);
    CHECK(index == 0);
}

/* Three jobs: each new one patches the previous job's Next (words 6-7), the
 * last keeps Next = 0, and the indexes count from 1. The GPU addresses are the
 * IOVA of the slot, not a CPU pointer. */
void testChaining()
{
    Buffer buffer(4 * kSlot);
    NativeMaliJobChain chain;
    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu, 4 * kSlot) ==
        kIOReturnSuccess);

    for(uint32_t i = 0; i < 3; ++i) {
        uint32_t index = 0;
        CHECK(chain.addWriteValue(NativeMaliWriteValueZero,
            UINT64_C(0x50000000) + i * 4, 0, &index) == kIOReturnSuccess);
        CHECK(index == i + 1);
        CHECK(chain.jobAt(i) == Buffer::kGpu + i * kSlot);
    }
    CHECK(chain.jobs() == 3);

    for(uint32_t i = 0; i < 3; ++i) {
        const uint8_t *job = buffer.cpu() + i * kSlot;
        CHECK(((word(job, 4) >> 16) & 0xFFFF) == i + 1);
        CHECK(word(job, 10) == NativeMaliWriteValueZero);
        if(i + 1 < 3) {
            CHECK(dword(job, 6) == Buffer::kGpu + (i + 1) * kSlot);
            CHECK(word(job, 7) == 0);   /* the IOVA fits in 32 bits here */
        } else {
            CHECK(dword(job, 6) == 0);
        }
    }

    /* The last slot is still untouched. */
    for(size_t i = 3 * kSlot; i < 4 * kSlot; ++i)
        CHECK(buffer.cpu()[i] == 0xcc);
}

/* A Next pointer above 4 GiB has to land in both halves of the word pair. */
void testHighGpuAddress()
{
    Buffer buffer(2 * kSlot);
    NativeMaliJobChain chain;
    const uint64_t gpu = UINT64_C(0x7f00000000);
    CHECK(chain.prepare(buffer.cpu(), gpu, 2 * kSlot) == kIOReturnSuccess);
    uint32_t index = 0;
    CHECK(chain.addWriteValue(NativeMaliWriteValueZero, gpu, 0, &index) ==
        kIOReturnSuccess);
    CHECK(chain.addWriteValue(NativeMaliWriteValueZero, gpu + kSlot, 0, &index) ==
        kIOReturnSuccess);
    const uint8_t *first = buffer.cpu();
    CHECK(word(first, 6) == uint32_t(gpu + kSlot));
    CHECK(word(first, 7) == uint32_t((gpu + kSlot) >> 32));
    const uint8_t *second = buffer.cpu() + kSlot;
    CHECK(word(second, 6) == 0);
    CHECK(word(second, 7) == 0);
    CHECK(dword(second, 8) == gpu + kSlot);
}

void testCapacity()
{
    Buffer buffer(2 * kSlot);
    NativeMaliJobChain chain;
    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu, 2 * kSlot) ==
        kIOReturnSuccess);

    uint32_t index = 0;
    CHECK(chain.addWriteValue(NativeMaliWriteValueZero, 0x1000, 0, &index) ==
        kIOReturnSuccess);
    CHECK(chain.addWriteValue(NativeMaliWriteValueZero, 0x2000, 0, &index) ==
        kIOReturnSuccess);
    CHECK(index == 2);

    /* Full: NoResources, the index output is cleared and the chain unchanged. */
    const uint8_t *first = buffer.cpu();
    const uint64_t next = dword(first, 6);
    CHECK(chain.addWriteValue(NativeMaliWriteValueZero, 0x3000, 0, &index) ==
        kIOReturnNoResources);
    CHECK(index == 0);
    CHECK(chain.jobs() == 2);
    CHECK(dword(first, 6) == next);

    /* The index pointer is optional. */
    CHECK(chain.addWriteValue(NativeMaliWriteValueZero, 0x3000, 0, nullptr) ==
        kIOReturnNoResources);
}

void testHeaderCodec()
{
    NativeMaliJobHeader header;
    header.exceptionStatus = 0x11223344;
    header.firstIncompleteTask = 0x55667788;
    header.faultPointer = UINT64_C(0x99aabbccddeeff00);
    header.type = NativeMaliJobIndexedVertex;
    header.is64Bit = true;
    header.barrier = true;
    header.invalidateCache = true;
    header.suppressPrefetch = true;
    header.enableTextureMapper = true;
    header.relaxDependency1 = true;
    header.relaxDependency2 = true;
    header.index = 0xabcd;
    header.dependency1 = 0x1234;
    header.dependency2 = 0x5678;
    header.next = UINT64_C(0x0102030405060708);

    uint8_t slot[kSlot];
    std::memset(slot, 0xaa, sizeof(slot));
    NativeMaliJobChain::packHeader(slot, header);

    CHECK(word(slot, 0) == 0x11223344);
    CHECK(word(slot, 1) == 0x55667788);
    CHECK(word(slot, 2) == 0xddeeff00);
    CHECK(word(slot, 3) == 0x99aabbcc);
    /* bit 0 = 1, type 10 << 1 = 0x14, bits 8/9/11/12/14/15 = 0xdb00,
     * index 0xabcd << 16. */
    CHECK(word(slot, 4) == 0xabcddb15);
    CHECK(word(slot, 5) == 0x56781234);
    CHECK(word(slot, 6) == 0x05060708);
    CHECK(word(slot, 7) == 0x01020304);
    /* packHeader writes the whole 64-byte slot, so the payload area is zero. */
    for(size_t i = 8; i < 16; ++i)
        CHECK(word(slot, i) == 0);

    NativeMaliJobHeader round;
    CHECK(NativeMaliJobChain::unpackHeader(slot, &round) == kIOReturnSuccess);
    CHECK(round.exceptionStatus == header.exceptionStatus);
    CHECK(round.firstIncompleteTask == header.firstIncompleteTask);
    CHECK(round.faultPointer == header.faultPointer);
    CHECK(round.type == header.type);
    CHECK(round.is64Bit == header.is64Bit);
    CHECK(round.barrier == header.barrier);
    CHECK(round.invalidateCache == header.invalidateCache);
    CHECK(round.suppressPrefetch == header.suppressPrefetch);
    CHECK(round.enableTextureMapper == header.enableTextureMapper);
    CHECK(round.relaxDependency1 == header.relaxDependency1);
    CHECK(round.relaxDependency2 == header.relaxDependency2);
    CHECK(round.index == header.index);
    CHECK(round.dependency1 == header.dependency1);
    CHECK(round.dependency2 == header.dependency2);
    CHECK(round.next == header.next);

    /* Every flag off, 32-bit: bit 0 clear means the GPU reads 32-bit pointers,
     * and the type still sits at bits 1:7. */
    NativeMaliJobHeader plain;
    plain.type = NativeMaliJobNull;
    plain.is64Bit = false;
    std::memset(slot, 0xff, sizeof(slot));
    NativeMaliJobChain::packHeader(slot, plain);
    CHECK(word(slot, 4) == 1u << 1);
    CHECK(word(slot, 5) == 0);
    CHECK(word(slot, 6) == 0);
    CHECK(word(slot, 7) == 0);
    CHECK(NativeMaliJobChain::unpackHeader(slot, &round) == kIOReturnSuccess);
    CHECK(!round.is64Bit);
    CHECK(round.type == NativeMaliJobNull);
    CHECK(round.index == 0);

    CHECK(NativeMaliJobChain::unpackHeader(nullptr, &round) ==
        kIOReturnBadArgument);
    CHECK(NativeMaliJobChain::unpackHeader(slot, nullptr) ==
        kIOReturnBadArgument);
}

/* The Index field is 16 bits, so 65535 jobs fit and the 65536th is refused
 * rather than truncated to index 0. */
void testIndexWidth()
{
    Buffer buffer(size_t(0xFFFF) * kSlot);
    NativeMaliJobChain chain;
    CHECK(chain.prepare(buffer.cpu(), Buffer::kGpu, size_t(0xFFFF) * kSlot) ==
        kIOReturnSuccess);
    uint32_t index = 0;
    for(uint32_t i = 0; i < 0xFFFF; ++i) {
        CHECK(chain.addWriteValue(NativeMaliWriteValueZero, Buffer::kGpu, 0,
            &index) == kIOReturnSuccess);
        CHECK(index == i + 1);
    }
    CHECK(chain.jobs() == 0xFFFF);
    CHECK(chain.addWriteValue(NativeMaliWriteValueZero, Buffer::kGpu, 0,
        &index) == kIOReturnNoResources);
    CHECK(index == 0);
    CHECK(chain.jobs() == 0xFFFF);
}

/* A builder that is reset can be pointed at a different buffer without any
 * state leaking from the old one. */
void testResetReuse()
{
    Buffer first(2 * kSlot);
    Buffer second(2 * kSlot);
    NativeMaliJobChain chain;
    CHECK(chain.prepare(first.cpu(), Buffer::kGpu, 2 * kSlot) ==
        kIOReturnSuccess);
    uint32_t index = 0;
    CHECK(chain.addWriteValue(NativeMaliWriteValueZero, 0x1000, 0, &index) ==
        kIOReturnSuccess);

    chain.reset();
    CHECK(!chain.prepared());
    const uint64_t gpu = UINT64_C(0x200000);
    CHECK(chain.prepare(second.cpu(), gpu, 2 * kSlot) == kIOReturnSuccess);
    CHECK(chain.jobs() == 0);
    CHECK(chain.firstJob() == 0);
    CHECK(chain.addWriteValue(NativeMaliWriteValueZero, gpu, 0, &index) ==
        kIOReturnSuccess);
    CHECK(index == 1);
    CHECK(dword(second.cpu(), 8) == gpu);
    /* The old buffer still holds the old job; nothing was written to it. */
    CHECK(dword(first.cpu(), 8) == UINT64_C(0x1000));
}

} // namespace

int main()
{
    testPrepareRefusals();
    testFirstWriteValue();
    testImmediateOnlyForImmediateTypes();
    testChaining();
    testHighGpuAddress();
    testCapacity();
    testIndexWidth();
    testHeaderCodec();
    testResetReuse();
    std::puts("PASS: v7 job header golden words, WRITE_VALUE payload, 64-byte slot "
        "alignment, Next chaining with a 1-based index, immediate only for the "
        "Immediate* types, capacity exhaustion, the 16-bit index width and the "
        "header codec");
    return 0;
}
