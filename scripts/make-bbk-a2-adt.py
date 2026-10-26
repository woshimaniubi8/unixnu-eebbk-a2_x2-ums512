#!/usr/bin/env python3
"""Generate the minimal Apple device tree used by the UMS512 bring-up."""
import argparse
import os
import struct
import uuid
import zlib
from pathlib import Path

def prop(name, data):
    return (name, data)

def u32(value):
    return struct.pack('<I', value)

def u64(value):
    return struct.pack('<Q', value)

def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)

def nvram_proxy():
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw
    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 2064)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [prop('nvram-bank-size', u32(len(image))), prop('nvram-bank-count', u32(1)),
            prop('nvram-current-bank', u32(0)), prop('nvram-proxy-data', bytes(image))]

parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--dram-size', type=lambda x: int(x, 0), required=True)
parser.add_argument('--owned-memory-size', type=lambda x: int(x, 0))
parser.add_argument('--cpu-regs', default='0,0x100')
parser.add_argument('--psci-method', choices=('smc', 'hvc'), default='smc')
parser.add_argument('--chip-id', type=lambda x: int(x, 0), default=0x512)
parser.add_argument('--unique-chip-id', type=lambda x: int(x, 0), default=0x554d533531325450)
parser.add_argument('--root-match-volume-uuid')
parser.add_argument('--data-ramdisk', action='store_true')
parser.add_argument('--packed-data-ramdisk', action='store_true')
parser.add_argument('--trust-cache', action='store_true')
parser.add_argument('--ipad-identity', action='store_true')
parser.add_argument('--gpu-addon', action='store_true')
args = parser.parse_args()

regs = [int(x, 0) for x in args.cpu_regs.split(',') if x.strip()]
if not regs or len(regs) > 8:
    raise SystemExit('--cpu-regs must contain 1..8 MPIDR values')
cpus = []
for index, reg in enumerate(regs):
    compatible = b'arm,cortex-a55\0arm,armv8\0' if index < 6 else b'arm,cortex-a75\0arm,armv8\0'
    cpus.append(node('cpu%d' % index, [prop('state', b'running\0' if index == 0 else b'waiting\0'),
        prop('reg', u64(reg)), prop('device_type', b'cpu\0'), prop('compatible', compatible),
        prop('timebase-frequency', u32(26000000))]))

chosen = [prop('random-seed', os.urandom(64)), prop('dram-base', u64(0x80000000)),
          prop('dram-size', u64(args.dram_size)), prop('osenvironment', b'main\0'),
          prop('framebuffer-base', u64(0x9e000000)), prop('framebuffer-size', u64(0x01000000)),
          prop('unique-chip-id', u64(args.unique_chip_id)), prop('chip-id', u32(args.chip_id)),
          prop('crypto-hash-method', b'sha2-384\0'), *nvram_proxy()]
if args.owned_memory_size:
    chosen.append(prop('darwinboot-memory-size', u64(args.owned_memory_size)))
if args.root_match_volume_uuid:
    uuid.UUID(args.root_match_volume_uuid)
    xml = ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
           '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
           args.root_match_volume_uuid.upper() + '</string></dict></dict>\0')
    chosen.append(prop('root-matching', xml.encode()))
if args.data_ramdisk or args.packed_data_ramdisk:
    chosen.append(prop('ephemeral-storage', u32(1)))
memory_map = [node('memory-map', [prop('RAMDisk', bytes(16)), prop('PackedRAMDisk', bytes(16)), prop('TrustCache', bytes(16))])]

arm_io = node('arm-io', [prop('device_type', b'arm-io\0'), prop('#address-cells', u32(2)),
    prop('#size-cells', u32(2)), prop('ranges', struct.pack('<QQQ', 0, 0x12000000, 0x5f000000))], [
    node('intc', [prop('compatible', b'arm,gic-v3\0'),
        prop('reg', struct.pack('<QQQQ', 0, 0x20000, 0x40000, 0x100000)),
        prop('redistributor-stride', u64(0x20000)), prop('#redistributor-regions', u32(1)),
        prop('#address-cells', u32(2)), prop('#size-cells', u32(2)),
        prop('interrupt-controller', b''), prop('interrupts', struct.pack('<III', 1, 9, 4))]),
    node('timer', [prop('compatible', b'arm,armv8-timer\0'),
        prop('interrupts', struct.pack('<12I', 1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4))]),
    node('pmu', [prop('compatible', b'arm,armv8-pmuv3\0'),
        prop('interrupts', struct.pack('<24I', 0, 0x70, 4, 0, 0x71, 4, 0, 0x72, 4, 0, 0x73, 4,
                                       0, 0x74, 4, 0, 0x75, 4, 0, 0x76, 4, 0, 0x77, 4))]),
    node('uart0', [prop('compatible', b'sprd,ums512-uart\0'),
        prop('reg', struct.pack('<QQ', 0x5e100000, 0x1000)), prop('clock-always-on', u32(1))]),
    node('framebuffer', [prop('compatible', b'uefi,framebuffer\0'),
        prop('reg', struct.pack('<QQ', 0x9e000000, 0x01000000))])])

tree = node('device-tree', [prop('model', b'BBK A2 X2 X2P (ums512_1h190)\0'),
    prop('compatible', b'unisoc,ums512\0sprd,sharkl5pro\0')], [
    node('chosen', chosen, memory_map), node('cpus', children=cpus),
    node('psci', [prop('compatible', b'arm,psci-1.0\0'), prop('method', args.psci_method.encode() + b'\0')]),
    node('defaults', [prop('sleep-disabled', u32(1))]), node('options'),
    node('product', [prop('product-name', b'BBK A2\0'), prop('model', b'UMS512\0')]), arm_io])
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(tree)
print('BBK A2 ADT: %d bytes, %d CPUs, GICD 0x12000000, UART 0x70100000' % (len(tree), len(regs)))
