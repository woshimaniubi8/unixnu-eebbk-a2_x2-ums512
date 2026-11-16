// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef BBK_PHYSICAL_MEMORY_MAP_H
#define BBK_PHYSICAL_MEMORY_MAP_H
// The includer supplies BBK_U64/BBK_U32 for EDK2 or the kernel. All on-wire
// values are little endian; the ADT property is copied to aligned storage.
#define BBK_MEMORY_MAGIC 0x3150414d4b424255ULL
#define BBK_MEMORY_VERSION 1U
#define BBK_MEMORY_MAX_RANGES 32U
#define BBK_MEMORY_PAGE 16384ULL
#define BBK_MEMORY_PROPERTY "unixnu,physical-memory-map"
typedef struct { BBK_U64 start, end; } BbkMemoryRange;
typedef struct {
    BBK_U64 magic;
    BBK_U32 version, count;
    BBK_U64 physical_start, physical_end;
    BBK_U64 staging_start, staging_end, used_end, managed_bytes;
    BbkMemoryRange ranges[BBK_MEMORY_MAX_RANGES];
} BbkMemoryMap;

static inline BBK_U64 bbkMemoryFloor(BBK_U64 n) {
    return n & ~(BBK_MEMORY_PAGE - 1);
}
static inline BBK_U64 bbkMemoryCeil(BBK_U64 n) {
    return n > (~(BBK_U64)0) - (BBK_MEMORY_PAGE - 1) ? 0 :
        (n + BBK_MEMORY_PAGE - 1) & ~(BBK_MEMORY_PAGE - 1);
}
// Insert a usable range, coalescing adjacent/overlapping entries. A capacity
// error is fatal to handoff; it never discards reservations or ranges.
static inline int bbkMemoryAdd(BbkMemoryMap *m, BBK_U64 start, BBK_U64 end) {
    BBK_U32 i = 0, j, n;
    if (start < m->physical_start) start = m->physical_start;
    if (end > m->physical_end) end = m->physical_end;
    start = bbkMemoryCeil(start); end = bbkMemoryFloor(end);
    if (!start || start >= end) return 1;
    while (i < m->count && m->ranges[i].end < start) ++i;
    j = i;
    while (j < m->count && m->ranges[j].start <= end) {
        if (m->ranges[j].start < start) start = m->ranges[j].start;
        if (m->ranges[j].end > end) end = m->ranges[j].end;
        ++j;
    }
    n = m->count - (j - i) + 1;
    if (n > BBK_MEMORY_MAX_RANGES) return 0;
    if (j == i) {
        for (BBK_U32 k = m->count; k > i; --k) m->ranges[k] = m->ranges[k - 1];
    } else {
        for (BBK_U32 k = j; k < m->count; ++k) m->ranges[i + 1 + k - j] = m->ranges[k];
    }
    m->ranges[i].start = start; m->ranges[i].end = end; m->count = n;
    return 1;
}
// Reservations round outwards. A4KiB carve-out must withhold its complete
// native16KiB page, even when adjacent UEFI descriptors use4KiB pages.
static inline int bbkMemoryExclude(BbkMemoryMap *m, BBK_U64 start, BBK_U64 end) {
    if (start >= end) return 0;
    start = bbkMemoryFloor(start); end = bbkMemoryCeil(end);
    if (!end) return 0;
    for (BBK_U32 i = 0; i < m->count;) {
        BbkMemoryRange r = m->ranges[i];
        if (end <= r.start || start >= r.end) { ++i; continue; }
        if (start <= r.start && end >= r.end) {
            for (BBK_U32 j = i + 1; j < m->count; ++j) m->ranges[j - 1] = m->ranges[j];
            --m->count;
        } else if (start <= r.start) { m->ranges[i++].start = end; }
        else if (end >= r.end) { m->ranges[i++].end = start; }
        else {
            if (m->count == BBK_MEMORY_MAX_RANGES) return 0;
            for (BBK_U32 j = m->count; j > i + 1; --j) m->ranges[j] = m->ranges[j - 1];
            m->ranges[i].end = start;
            m->ranges[i + 1].start = end; m->ranges[i + 1].end = r.end;
            ++m->count; i += 2;
        }
    }
    return 1;
}
static inline int bbkMemoryContains(const BbkMemoryMap *m, BBK_U64 start, BBK_U64 end) {
    if (start >= end) return 0;
    for (BBK_U32 i = 0; i < m->count; ++i) {
        if (start < m->ranges[i].start) return 0;
        if (start < m->ranges[i].end) return end <= m->ranges[i].end;
    }
    return 0;
}
static inline BBK_U64 bbkMemoryNext(const BbkMemoryMap *m, BBK_U64 at) {
    for (BBK_U32 i = 0; i < m->count; ++i) {
        if (at < m->ranges[i].start) return m->ranges[i].start;
        if (at < m->ranges[i].end) return at;
    }
    return m->physical_end;
}
static inline BBK_U64 bbkMemoryCount(const BbkMemoryMap *m, BBK_U64 first, BBK_U64 end) {
    BBK_U64 bytes = 0;
    for (BBK_U32 i = 0; i < m->count; ++i) {
        BBK_U64 s = m->ranges[i].start > first ? m->ranges[i].start : first;
        BBK_U64 e = m->ranges[i].end < end ? m->ranges[i].end : end;
        if (s < e) bytes += e - s;
    }
    return bytes;
}
static inline int bbkMemoryValidate(const BbkMemoryMap *m) {
    BBK_U64 bytes = 0, last = m->physical_start;
    if (m->magic != BBK_MEMORY_MAGIC || m->version != BBK_MEMORY_VERSION ||
        !m->count || m->count > BBK_MEMORY_MAX_RANGES ||
        m->physical_start >= m->physical_end ||
        ((m->physical_start | m->physical_end | m->staging_start | m->staging_end | m->used_end) & (BBK_MEMORY_PAGE - 1)) ||
        m->staging_start < m->physical_start || m->staging_end > m->physical_end ||
        m->staging_start >= m->used_end || m->used_end >= m->staging_end) return 0;
    for (BBK_U32 i = 0; i < m->count; ++i) {
        BbkMemoryRange r = m->ranges[i];
        if (r.start < last || r.start >= r.end || r.end > m->physical_end ||
            ((r.start | r.end) & (BBK_MEMORY_PAGE - 1))) return 0;
        if (i && r.start == last) return 0;
        bytes += r.end - r.start; last = r.end;
    }
    return bytes == m->managed_bytes &&
        bbkMemoryContains(m, m->staging_start, m->staging_end);
}
#endif
