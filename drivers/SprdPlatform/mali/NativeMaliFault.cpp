/* SPDX-License-Identifier: GPL-2.0-only */
/* Fault decoding; the register offsets and the pinned sources are in
 * NativeMaliFault.h. */
#include "NativeMaliFault.h"

const char *NativeMaliFault::exceptionName(uint32_t exceptionCode)
{
    /* DRM_PANFROST_EXCEPTION_* from panfrost_device.h:258-330; the names are
     * the enumerator suffixes because panfrost_device.c:418-421 builds its
     * table with the PANFROST_EXCEPTION(id) macro. The table is sparse and
     * 0xF0 entries long (panfrost_device.c:425-500), so an unnamed hole and
     * anything at or above 0xF0 both report the same string. */
    switch(exceptionCode) {
    case 0x00:return "OK";
    case 0x01:return "DONE";
    case 0x02:return "INTERRUPTED";
    case 0x03:return "STOPPED";
    case 0x04:return "TERMINATED";
    case 0x05:return "KABOOM";
    case 0x06:return "EUREKA";
    case 0x08:return "ACTIVE";
    case 0x40:return "JOB_CONFIG_FAULT";
    case 0x41:return "JOB_POWER_FAULT";
    case 0x42:return "JOB_READ_FAULT";
    case 0x43:return "JOB_WRITE_FAULT";
    case 0x44:return "JOB_AFFINITY_FAULT";
    case 0x48:return "JOB_BUS_FAULT";
    case 0x50:return "INSTR_INVALID_PC";
    case 0x51:return "INSTR_INVALID_ENC";
    case 0x52:return "INSTR_TYPE_MISMATCH";
    case 0x53:return "INSTR_OPERAND_FAULT";
    case 0x54:return "INSTR_TLS_FAULT";
    case 0x55:return "INSTR_BARRIER_FAULT";
    case 0x56:return "INSTR_ALIGN_FAULT";
    case 0x58:return "DATA_INVALID_FAULT";
    case 0x59:return "TILE_RANGE_FAULT";
    case 0x5A:return "ADDR_RANGE_FAULT";
    case 0x5B:return "IMPRECISE_FAULT";
    case 0x60:return "OOM";
    case 0x61:return "OOM_AFBC";
    case 0x7F:return "UNKNOWN";
    case 0x80:return "DELAYED_BUS_FAULT";
    case 0x88:return "GPU_SHAREABILITY_FAULT";
    case 0x89:return "SYS_SHAREABILITY_FAULT";
    case 0x8A:return "GPU_CACHEABILITY_FAULT";
    case 0xC0:return "TRANSLATION_FAULT_0";
    case 0xC1:return "TRANSLATION_FAULT_1";
    case 0xC2:return "TRANSLATION_FAULT_2";
    case 0xC3:return "TRANSLATION_FAULT_3";
    case 0xC4:return "TRANSLATION_FAULT_4";
    case 0xC7:return "TRANSLATION_FAULT_IDENTITY";
    case 0xC8:return "PERM_FAULT_0";
    case 0xC9:return "PERM_FAULT_1";
    case 0xCA:return "PERM_FAULT_2";
    case 0xCB:return "PERM_FAULT_3";
    case 0xD0:return "TRANSTAB_BUS_FAULT_0";
    case 0xD1:return "TRANSTAB_BUS_FAULT_1";
    case 0xD2:return "TRANSTAB_BUS_FAULT_2";
    case 0xD3:return "TRANSTAB_BUS_FAULT_3";
    case 0xD8:return "ACCESS_FLAG_0";
    case 0xD9:return "ACCESS_FLAG_1";
    case 0xDA:return "ACCESS_FLAG_2";
    case 0xDB:return "ACCESS_FLAG_3";
    case 0xE0:return "ADDR_SIZE_FAULT_IN0";
    case 0xE1:return "ADDR_SIZE_FAULT_IN1";
    case 0xE2:return "ADDR_SIZE_FAULT_IN2";
    case 0xE3:return "ADDR_SIZE_FAULT_IN3";
    case 0xE4:return "ADDR_SIZE_FAULT_OUT0";
    case 0xE5:return "ADDR_SIZE_FAULT_OUT1";
    case 0xE6:return "ADDR_SIZE_FAULT_OUT2";
    case 0xE7:return "ADDR_SIZE_FAULT_OUT3";
    case 0xE8:return "MEM_ATTR_FAULT_0";
    case 0xE9:return "MEM_ATTR_FAULT_1";
    case 0xEA:return "MEM_ATTR_FAULT_2";
    case 0xEB:return "MEM_ATTR_FAULT_3";
    case 0xEC:return "MEM_ATTR_NONCACHE_0";
    case 0xED:return "MEM_ATTR_NONCACHE_1";
    case 0xEE:return "MEM_ATTR_NONCACHE_2";
    case 0xEF:return "MEM_ATTR_NONCACHE_3";
    default:return "Unknown exception type";
    }
}

const char *NativeMaliFault::accessName(uint32_t accessType)
{
    /* access_type_name(), panfrost_mmu.c:847-869. The ATOMIC case depends on
     * HW_FEATURE_AARCH64_MMU, which the G52 feature set includes. */
    switch(accessType & 0x3) {
    case 0:return "ATOMIC";
    case 1:return "EXECUTE";
    case 2:return "READ";
    case 3:return "WRITE";
    default:return "UNKNOWN";
    }
}
