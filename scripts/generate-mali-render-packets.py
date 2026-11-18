#!/usr/bin/env python3
"""Reproduce the pack-only subset using the pinned, unchanged Mesa generator."""
import argparse,hashlib,json,re,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PREFIX='/* SPDX-License-Identifier: MIT */\n/* Generated pack-only subset of Mesa26.2.6 v7.xml by its unmodified gen_pack.py.\n * Copyright (C) 2016 Intel Corporation, Broadcom; 2020 Collabora Ltd.\n * Reproduce with scripts/generate-mali-render-packets.py. No field offsets\n * or packing expressions are edited. Printing/unpacking code is omitted. */\n#pragma once\n#include <stdint.h>\n#include <string.h>\n#if defined(KERNEL)\n#include <kern/assert.h>\n#else\n#include <assert.h>\n#endif\n#ifndef IS_POT_NONZERO\n#define IS_POT_NONZERO(x) ((x) && !((x)&((x)-1)))\n#endif\n#ifndef ALIGN_POT\n#define ALIGN_POT(x,a) (((x)+(a)-1)&~((a)-1))\n#endif\nnamespace SprdMaliMesa {\n// Adapted Mesa bitpack_helpers.h / libcl integer helpers, without libc/CL types.\ninline uint64_t util_bitpack_uint(uint64_t v,unsigned start,unsigned end) {\n    assert(start<=end && end<64);\n    assert(end-start==63 || v<(UINT64_C(1)<<(end-start+1)));\n    return v<<start;\n}\ninline unsigned util_logbase2(unsigned value) {assert(value);return 31u-__builtin_clz(value);}\ninline uint32_t util_bitpack_float(float value) {uint32_t bits;memcpy(&bits,&value,4);return bits;}\n'
parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true');args=parser.parse_args()
source=ROOT/'hardware/mesa-bifrost';pin=json.loads((source/'provenance.json').read_text())
for name,key in [('v7.xml','xml_sha256'),('gen_pack.py','generator_sha256')]:
 assert hashlib.sha256((source/name).read_bytes()).hexdigest()==pin[key],name
r=subprocess.run([sys.executable,str(source/'gen_pack.py'),str(source/'v7.xml')],capture_output=True);assert r.returncode==0,r.stderr
generated=r.stdout.replace(b'\r\n',b'\n')
assert hashlib.sha256(generated).hexdigest()==pin['generated_full_sha256']
text=generated.decode();parts=[m.group() for m in re.finditer(r'enum mali_[a-z0-9_]+ \{.*?\n\};',text,re.S)]
for name in pin['selected_packets']:
 start=text.index('struct MALI_'+name+' {');end=text.index('#define MALI_'+name+'_unpack(',start)
 parts.append(text[start:end])
result=(PREFIX+'\n\n'.join(parts)+'\n}\n').encode()
assert hashlib.sha256(result).hexdigest()==pin['output_sha256']
target=ROOT/'drivers/SprdPlatform/mali/MesaV7RenderPackets.h'
if args.check:assert target.read_bytes()==result,'Generated packers drifted'
else:target.write_bytes(result)
print('PASS: unchanged Mesa26.2.6 XML/generator and byte-exact pack-only subset')
