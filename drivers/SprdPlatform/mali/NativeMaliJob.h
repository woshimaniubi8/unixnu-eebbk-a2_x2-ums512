/* SPDX-License-Identifier: GPL-2.0-only */
/* The Bifrost v7 job chain: the 32-byte job header, the WRITE_VALUE payload,
 * and the linked list the job manager walks. No MMIO.
 *
 * Source (content-pinned, third_party/PINNED.md):
 *   third_party/mesa-panfrost/v7.xml:1334-1351
 *     struct "Job Header": Exception Status (word 0), First Incomplete Task
 *     (word 1), Fault Pointer (words 2-3), then word 4 with Is 64b at bit 0,
 *     Type at 1:7, Barrier at 8, Invalidate Cache at 9, Suppress Prefetch at
 *     11, Enable Texture Mapper at 12, Relax Dependency 1 at 14, Relax
 *     Dependency 2 at 15 and Index at 16:31; word 5 with Dependency 1 at 0:15
 *     and Dependency 2 at 16:31; Next (words 6-7).
 *   third_party/mesa-panfrost/v7.xml:37-49
 *     enum "Job Type": Not started 0, Null 1, Write value 2, Cache flush 3,
 *     Compute 4, Vertex 5, Geometry 6, Tiler 7, Fused 8, Fragment 9,
 *     Indexed Vertex 10.
 *   third_party/mesa-panfrost/v7.xml:1370-1399
 *     enum "Write Value Type": Cycle Counter 1, System Timestamp 2, Zero 3,
 *     Immediate 8 4, Immediate 16 5, Immediate 32 6, Immediate 64 7; struct
 *     "Write Value Job Payload": Address (words 0-1), Type (word 2), Immediate
 *     Value (words 4-5); aggregate "Write Value Job" align 64 with the Header
 *     at offset 0 and the Payload at offset 32.
 *   third_party/mesa-panfrost/pan_jc.h:pan_jc_add_job()
 *     the index is ++job_index (so the first job is 1, not 0), the type,
 *     barrier, suppress-prefetch and the two dependencies go into the header,
 *     and the previous job's Next (opaque[6]/opaque[7]) is pointed at the new
 *     job; the last job keeps Next = 0 to end the chain.
 *   third_party/mesa-panfrost/pan_jc.h:pan_jc_initialize_tiler()
 *     the only WRITE_VALUE job the source builds is the tiler initialiser:
 *     type Write value, the reserved index, Next = the old first job, payload
 *     address = polygon list, payload type = Zero. It is compiled out for
 *     PAN_ARCH >= 6, so on v7 that job does not exist; the payload layout and
 *     the "Zero leaves the immediate at 0" behaviour are still what the XML and
 *     this function show.
 *
 * The chain is placed in caller-owned memory: `cpu` is where the CPU writes,
 * `gpu` is the same memory's GPU address (an IOVA from NativeMaliMemory). The
 * XML's only placement constraint is align 64, and a WRITE_VALUE job is
 * 32 + 24 = 56 bytes, so this builder uses 64-byte slots. That satisfies the
 * alignment and cannot overlap; it wastes 8 bytes per job. The exact rounding
 * pan_pool_alloc_desc() does is not in the pinned set, so it is not claimed.
 *
 * Not here: the JS_HEAD/JS_AFFINITY/JS_CONFIG/JS_COMMAND slot registers and the
 * interrupt/fence side (panfrost_job.c); those need the interruption path and
 * belong to the submission step.
 */
#ifndef NATIVE_MALI_JOB_H
#define NATIVE_MALI_JOB_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

enum : uint32_t {
    NativeMaliJobNotStarted = 0,
    NativeMaliJobNull = 1,
    NativeMaliJobWriteValue = 2,
    NativeMaliJobCacheFlush = 3,
    NativeMaliJobCompute = 4,
    NativeMaliJobVertex = 5,
    NativeMaliJobGeometry = 6,
    NativeMaliJobTiler = 7,
    NativeMaliJobFused = 8,
    NativeMaliJobFragment = 9,
    NativeMaliJobIndexedVertex = 10
};

enum : uint32_t {
    NativeMaliWriteValueCycleCounter = 1,
    NativeMaliWriteValueSystemTimestamp = 2,
    NativeMaliWriteValueZero = 3,
    NativeMaliWriteValueImmediate8 = 4,
    NativeMaliWriteValueImmediate16 = 5,
    NativeMaliWriteValueImmediate32 = 6,
    NativeMaliWriteValueImmediate64 = 7
};

struct NativeMaliJobHeader {
    uint32_t exceptionStatus = 0;     /* word 0; written by the GPU on a fault */
    uint32_t firstIncompleteTask = 0; /* word 1; written by the GPU on a fault */
    uint64_t faultPointer = 0;        /* words 2-3; written by the GPU on a fault */
    uint32_t type = NativeMaliJobNull;/* word 4, bits 1:7 */
    bool is64Bit = true;              /* word 4, bit 0 (the XML default) */
    bool barrier = false;             /* word 4, bit 8 */
    bool invalidateCache = false;     /* word 4, bit 9 */
    bool suppressPrefetch = false;    /* word 4, bit 11 */
    bool enableTextureMapper = false; /* word 4, bit 12 */
    bool relaxDependency1 = false;    /* word 4, bit 14 */
    bool relaxDependency2 = false;    /* word 4, bit 15 */
    uint32_t index = 0;               /* word 4, bits 16:31 */
    uint32_t dependency1 = 0;         /* word 5, bits 0:15 */
    uint32_t dependency2 = 0;         /* word 5, bits 16:31 */
    uint64_t next = 0;                /* words 6-7; 0 ends the chain */
};

class NativeMaliJobChain final {
    uint8_t *cpu_ = nullptr;
    uint64_t gpu_ = 0;
    size_t slots_ = 0;
    size_t jobs_ = 0;
    bool prepared_ = false;

    uint8_t *slot(size_t index) const;
public:
    enum : size_t {
        HeaderBytes = 32,
        WriteValueBytes = 56,   /* header 32 + payload 24 */
        SlotBytes = 64          /* the XML's align=64 */
    };

    NativeMaliJobChain() = default;
    ~NativeMaliJobChain() = default;
    NativeMaliJobChain(const NativeMaliJobChain &) = delete;
    NativeMaliJobChain &operator=(const NativeMaliJobChain &) = delete;

    /* `cpu` and `gpu` must be 64-byte aligned and `bytes` a non-zero multiple
     * of 64, so a partial slot is never written. The builder never allocates:
     * the memory is the caller's (NativeMaliMemory is one way to get it). */
    IOReturn prepare(void *cpu, uint64_t gpu, size_t bytes);
    void reset();
    bool prepared() const { return prepared_; }
    size_t capacity() const { return slots_; }
    size_t jobs() const { return jobs_; }
    /* GPU address of the first job, or 0 when the chain is empty. */
    uint64_t firstJob() const { return jobs_ ? gpu_ : 0; }
    uint64_t jobAt(size_t index) const { return index < jobs_ ? gpu_ + index * SlotBytes : 0; }
    /* Bytes of the buffer the chain actually uses. */
    size_t used() const { return jobs_ * SlotBytes; }

    /* Append a WRITE_VALUE job and return its scoreboard index (1-based, as
     * pan_jc_add_job() assigns it). `target` is the GPU address written;
     * `immediate` is only stored for the Immediate* types, because every other
     * type leaves the field at zero in the source. When this is not the first
     * job, the previous job's Next is pointed at it; the new job's Next stays
     * 0 until another job is appended. kIOReturnNoResources when every slot is
     * used. */
    IOReturn addWriteValue(uint32_t writeValueType, uint64_t target,
        uint64_t immediate, uint32_t *index);

    /* Header codec. pack() zeroes the whole 64-byte slot first, so the unused
     * bytes (payload words 3 and 6-7, everything past byte 56) are 0. */
    static void packHeader(uint8_t *slot, const NativeMaliJobHeader &header);
    static IOReturn unpackHeader(const uint8_t *slot, NativeMaliJobHeader *header);
};
#endif
