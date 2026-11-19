/* SPDX-License-Identifier: GPL-2.0-only */
/* The wire ABI between the (future) NativeMaliService IOUserClient and a
 * userspace GPU driver.
 *
 * This is not the IOKit ABI and not an ioctl: the service copies exactly these
 * fixed-size records across its external methods. All fields are little endian;
 * both sides are ARM64. Every array lives inside one record, so a caller cannot
 * pass a length that disagrees with the storage. An unknown version, method or
 * flag bit is refused, never ignored.
 *
 * The set of methods is what the driver can actually honour today:
 *   Query, GetDeviceInfo          read-only facts (M1/M3 probe output)
 *   BoAlloc, BoMap, BoFree        NativeMaliMemory (M5)
 *   BoCpuMap, BoCpuUnmap          the caller's CPU view of a BO
 *   Submit, WaitFence             NativeMaliJob + NativeMaliJobSlot (M6)
 *   GetStatus                     fault and reset counters
 *   Reset                         device-wide recovery after a fault
 *
 * Reset is deliberately device-wide, not per client: there is one GPU, one job
 * manager and one MMU, so a state that wedges a slot cannot be recovered inside
 * one address space. Any connected caller may issue it (the service has no
 * finer permission model than "opened the service", the same boundary Submit
 * crosses). It carries no records: the selector is the whole request. The call
 * destroys every client's in-flight job, so those fences answer IOError through
 * WaitFence and GetStatus::resets advances for every client; the page tables
 * are then programmed again from the software state that survived, so BOs and
 * mappings stay valid. Without it the first job fault would make the device
 * unusable until the driver is reloaded. Adding it does not change the version:
 * a v1 client that predates the selector never calls it, and a v1 driver that
 * predates it answers kIOReturnUnsupported.
 *
 * Version 2 appends the fields a Vulkan backend needs (mem/MMU/AFBC/texture
 * features, derived thread properties, page shift and VM operation flags).
 * The v1 64-byte GetDeviceInfo record is a strict prefix of v2: a v1 caller
 * still gets 64 valid bytes, and Query echoes the version it asked for.
 *
 * Deliberately absent, with the reason:
 *   - CreateQueue/CloseQueue. A queue would be an address space, and this
 *     device has three present address spaces (GPU_AS_PRESENT == 0x7, see
 *     docs/DEVICE.md) shared by every client. One address space per client is
 *     what the driver can guarantee; a client that could open several would
 *     starve the others. Submit therefore names no queue.
 *   - a job-slot field. The three job slots are shared hardware; the service
 *     picks a free one, the way panfrost's scheduler does, and returns Busy
 *     when none is free. A slot number is not the client's to choose.
 *   - BoImport. Pinning a caller's own pages needs a memory source that can
 *     describe them (IOBufferMemoryDescriptor or an IOSurface); the host-tested
 *     NativeMaliMemorySource cannot yet (docs/OPEN_QUESTIONS.md item 5).
 *   - requirements/affinity. panfrost computes the shader-core affinity from
 *     the job's requirements; this driver drives JS_AFFINITY_NEXT with
 *     features.shader_present, "use all cores for now"
 *     (panfrost_job.c:135-151). Advertising a requirements field the driver
 *     ignores would be a lie.
 *   - flush id. The service reads GPU_LATEST_FLUSH_ID itself
 *     (NativeMaliGpuCore), so the client cannot pick one.
 *
 * Sizes and the arithmetic behind the constants:
 *   - 4 KiB is the only page size NativeMaliVm maps (NativeMaliVmGranule).
 *   - 128 MiB is NativeMaliVmMaxTables (64) * NativeMaliVmPtesPerTable (512) *
 *     NativeMaliVmGranule (4096) = 2^27: one buffer cannot need more leaves
 *     than the page-table budget of the address space it lives in.
 *   - 64 bytes is the v7 job header size and its required alignment
 *     (NativeMaliJobHeadAlignment, from mesa v7.xml:1334-1351).
 *
 * The conventions - fixed-size records with the arrays inside them, a version
 * field that must match, unknown flags refused rather than ignored, and
 * static_assert on every layout - follow NativeAdrenoABI.h (MIT, see NOTICE).
 * None of its records or constants are copied: the fields here are the ones
 * this driver can honour, listed above.
 *
 * A fence is opaque to userspace: nonzero, produced by Submit, consumed by
 * WaitFence. The driver packs (job slot, emit_seqno) into it - the low 32 bits
 * are NativeMaliJobSlot's seqno and the high 32 its slot - but that layout may
 * change with the ABI version, so a client must round-trip it, not decode it.
 *
 * Dispatch: every method number fixes exactly one input record and one output
 * record (either may be absent), and the sizes are exactly the structs below.
 * A wrapper rejects any other size before copying, and writes no output byte
 * unless the call succeeded. NativeMaliUserClient::dispatch() is that rule in
 * code; the IOUserClient subclass only copies memory around it.
 */
#ifndef NATIVE_MALI_ABI_H
#define NATIVE_MALI_ABI_H
#include <stddef.h>
#include <stdint.h>

enum {
    /* v2 appends the fields a Vulkan backend needs; the v1 record is a strict
     * prefix of v2, and dispatch still accepts the 64-byte v1 reply. */
    NativeMaliAbiVersion = 2,
    NativeMaliAbiVersionV1 = 1,

    /* Method selectors. */
    NativeMaliMethodQuery = 1,
    NativeMaliMethodGetDeviceInfo = 2,
    NativeMaliMethodBoAlloc = 3,
    NativeMaliMethodBoMap = 4,
    NativeMaliMethodBoFree = 5,
    NativeMaliMethodBoCpuMap = 6,
    NativeMaliMethodBoCpuUnmap = 7,
    NativeMaliMethodSubmit = 8,
    NativeMaliMethodWaitFence = 9,
    NativeMaliMethodGetStatus = 10,
    /* Device-wide recovery. No records exist for it. */
    NativeMaliMethodReset = 11,
    NativeMaliMethodLast = NativeMaliMethodReset,

    /* The single page size the MMU layer maps. */
    NativeMaliAbiPageBytes = 4096,
    /* NativeMaliVmMaxTables * NativeMaliVmPtesPerTable * NativeMaliAbiPageBytes. */
    NativeMaliAbiMaxBoBytesShift = 27,
    /* NativeMaliJobHeadAlignment. */
    NativeMaliAbiJobChainAlign = 64,
    /* log2 of NativeMaliAbiPageBytes, for pan_kmod_dev_props.pgsize_bitmap. */
    NativeMaliAbiPageShift = 12,
    /* The v1 GetDeviceInfo record; v2 is a strict prefix-compatible extension. */
    NativeMaliAbiDeviceInfoV1Bytes = 64,
    NativeMaliAbiDeviceInfoBytes = 112
};

#define NATIVE_MALI_ABI_MAX_BO_BYTES (UINT64_C(1) << NativeMaliAbiMaxBoBytesShift)

/* Query. The caller sets abi_version and flags; the reply carries the rest. */
struct NativeMaliAbiQuery {
    uint32_t abi_version;         /* must equal NativeMaliAbiVersion */
    uint32_t input_bits;          /* GPU address bits: 48 (NativeMaliVmMaxIas) */
    uint32_t output_bits;         /* physical address bits from the probe: 40 */
    uint32_t flags;               /* request: zero. No optional feature exists. */
    uint64_t max_bo_bytes;        /* 1 << NativeMaliAbiMaxBoBytesShift */
};

/* GetDeviceInfo. Every field is a register reading from the M3 probe, not a
 * guess: gpu_id is GPU_ID, the masks are GPU_SHADER/TILER_PRESENT_LO/HI, the
 * limits are the GPU_THREAD_* registers, and the feature words are
 * GPU_L2/CORE/TILER_FEATURES (NativeMaliGpuObservation). */
enum {
    NativeMaliDeviceNonCoherent = 1   /* the CPU must clean/invalidate around GPU use */
};
/* v2 VM operation bits: what NativeMaliMemory::map()/unmap() actually support.
 * Sparse mappings and automatic VA assignment are not in the ABI. */
enum {
    NativeMaliAbiVmOpMap = 1u << 0,
    NativeMaliAbiVmOpUnmap = 1u << 1
};
struct NativeMaliAbiDeviceInfo {
    /* v1 fields: a v1 caller receives exactly these 64 bytes. */
    uint32_t gpu_id;
    uint32_t shader_present_lo;
    uint32_t shader_present_hi;
    uint32_t tiler_present_lo;
    uint32_t tiler_present_hi;
    uint32_t thread_max_threads;
    uint32_t max_workgroup_size;
    uint32_t max_barrier_size;
    uint32_t l2_features;
    uint32_t core_features;
    uint32_t tiler_features;
    uint32_t flags;               /* NativeMaliDeviceNonCoherent */
    uint64_t va_start;            /* first GPU address a BO may map at: 0 */
    uint64_t va_size;             /* 1 << input_bits (the whole space) */

    /* v2: the raw feature words a pan_kmod backend needs. Zero means the
     * probe has not answered that register; it is never a substitute value. */
    uint32_t mem_features;        /* GPU_MEM_FEATURES */
    uint32_t mmu_features;        /* GPU_MMU_FEATURES; [7:0] ias, [15:8] oas */
    uint32_t afbc_features;       /* GPU_AFBC_FEATURES */
    uint32_t texture_features[4]; /* GPU_TEXTURE_FEATURES(0..3) */
    /* Derived in the driver exactly as panfrost_kmod.c:138-175 does. */
    uint32_t max_tasks_per_core;
    uint32_t registers_per_core;
    uint32_t tls_instances_per_core;
    /* Driver capability constants, not measurements. */
    uint32_t pgsize_shift;        /* NativeMaliAbiPageShift */
    uint32_t vm_op_flags;         /* NativeMaliAbiVmOpMap | NativeMaliAbiVmOpUnmap */
};

struct NativeMaliAbiBoAlloc {
    uint64_t bytes;               /* nonzero multiple of NativeMaliAbiPageBytes, <= max_bo_bytes */
};
struct NativeMaliAbiBoAllocReply {
    uint64_t handle;              /* opaque, nonzero */
};

/* Map an allocated BO into the client's address space at a caller-chosen IOVA,
 * because NativeMaliMemory::map() takes the address and the MMU has no
 * allocator of its own. Mapping twice, or onto a page another BO of this
 * address space owns, is refused. */
enum {
    NativeMaliBoRead = 1u << 0,   /* required: panfrost maps IOMMU_READ always */
    NativeMaliBoWrite = 1u << 1
};
struct NativeMaliAbiBoMap {
    uint64_t handle;
    uint64_t iova;                /* nonzero, NativeMaliAbiPageBytes aligned */
    uint32_t permissions;         /* NativeMaliBoRead, plus NativeMaliBoWrite for a writable map */
    uint32_t reserved;            /* zero */
};
struct NativeMaliAbiBoMapReply {
    uint64_t iova;
};

struct NativeMaliAbiBoFree {
    uint64_t handle;
};

/* The caller's own CPU view. `token` is what the IOUserClient's
 * clientMemoryForType() expects; a token is valid until BoCpuUnmap or BoFree. */
struct NativeMaliAbiBoCpuMap {
    uint64_t handle;
};
struct NativeMaliAbiBoCpuMapReply {
    uint32_t token;
    uint32_t reserved;            /* zero */
    uint64_t bytes;
};
struct NativeMaliAbiBoCpuUnmap {
    uint64_t handle;
    uint32_t token;               /* exactly the token BoCpuMap returned */
    uint32_t reserved;            /* zero */
};

/* Start the job chain at `job_chain`: one or more Bifrost v7 job headers,
 * 64-byte aligned, in memory the client has already mapped. The chain is
 * built by the client (NativeMaliJob describes the layout), so no job
 * descriptor crosses this boundary.
 *
 * wait_fence is zero or an earlier fence of this client; it is waited for
 * before the job is submitted, with wait_timeout_ns (0 polls once).
 */
struct NativeMaliAbiSubmit {
    uint64_t job_chain;           /* GPU address, 64-byte aligned, nonzero */
    uint64_t wait_fence;          /* zero or an earlier fence of this client */
    uint64_t wait_timeout_ns;     /* used only when wait_fence is nonzero */
    uint32_t flags;               /* zero */
    uint32_t reserved;            /* zero */
};
struct NativeMaliAbiSubmitReply {
    uint64_t fence;               /* nonzero, opaque */
};

struct NativeMaliAbiWaitFence {
    uint64_t fence;               /* a fence this client received */
    uint64_t timeout_ns;          /* 0 polls once */
};

/* Monotonic counters: a changed `faults` means the context was lost and every
 * fence before it is dead. */
struct NativeMaliAbiStatus {
    uint64_t faults;
    uint64_t resets;
};

#if defined(__cplusplus)
static_assert(sizeof(NativeMaliAbiQuery) == 24, "Mali ABI query layout");
static_assert(sizeof(NativeMaliAbiDeviceInfo) == NativeMaliAbiDeviceInfoBytes,
    "Mali ABI device info layout");
static_assert(offsetof(NativeMaliAbiDeviceInfo, mem_features) ==
    NativeMaliAbiDeviceInfoV1Bytes, "v1 record is a strict prefix of v2");
static_assert(sizeof(NativeMaliAbiBoAlloc) == 8, "Mali ABI alloc layout");
static_assert(sizeof(NativeMaliAbiBoAllocReply) == 8, "Mali ABI alloc reply layout");
static_assert(sizeof(NativeMaliAbiBoMap) == 24, "Mali ABI map layout");
static_assert(sizeof(NativeMaliAbiBoMapReply) == 8, "Mali ABI map reply layout");
static_assert(sizeof(NativeMaliAbiBoFree) == 8, "Mali ABI free layout");
static_assert(sizeof(NativeMaliAbiBoCpuMap) == 8, "Mali ABI cpu map layout");
static_assert(sizeof(NativeMaliAbiBoCpuMapReply) == 16, "Mali ABI cpu map reply layout");
static_assert(sizeof(NativeMaliAbiBoCpuUnmap) == 16, "Mali ABI cpu unmap layout");
static_assert(sizeof(NativeMaliAbiSubmit) == 32, "Mali ABI submit layout");
static_assert(sizeof(NativeMaliAbiSubmitReply) == 8, "Mali ABI submit reply layout");
static_assert(sizeof(NativeMaliAbiWaitFence) == 16, "Mali ABI wait layout");
static_assert(sizeof(NativeMaliAbiStatus) == 16, "Mali ABI status layout");
#endif
#endif
