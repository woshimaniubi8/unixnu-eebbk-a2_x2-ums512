#!/usr/bin/env python3
"""Check and embed the Mesa G52 r1 probe. No compiler is used at boot."""
import argparse
import hashlib
import re
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "drivers/SprdPlatform/SprdMaliComputePayload.h"
XML = ROOT / "hardware/mesa-bifrost/v7.xml"
PARTS = (
    ("Job", 0, 0xC0),
    ("State", 0x100, 0x40),
    ("Storage", 0x140, 0x20),
    ("Code", 0x1000, 208),
)
BASE = 0x03000000
PAYLOAD_SHA256 = "7860d9608f4fa0bff04762841211efed7475da29bde077701c826985df72fe37"
CODE_SHA256 = "007265318ef77ec4be373e41fe7326a4ba15de4de7327e5d5f352033f41dc0ed"
GRID = [32, 1, 1, 128, 1, 1]
TASK_SPLIT = 8
NAMESPACE = "SprdMaliComputePayload"


def decode(xml, name, data):
    word = int.from_bytes(data, "little")
    fields = {}
    for node in xml.find(f"./struct[@name='{name}']").findall("field"):
        start = [int(x) for x in node.attrib["start"].split(":")]
        shift = start[0] * 32 + start[1] if len(start) == 2 else start[0]
        fields[node.attrib["name"]] = (word >> shift) & ((1 << int(node.attrib["size"])) - 1)
    return fields


def check(data):
    assert len(data) == 0x1000 + PARTS[-1][2] and hashlib.sha256(data).hexdigest() == PAYLOAD_SHA256
    x = ET.parse(XML).getroot()
    job = decode(x, "Job Header", data[:32])
    compute = x.find("./enum[@name='Job Type']/value[@name='Compute']")
    assert job["Type"] == int(compute.attrib["value"]) and job["Is 64b"] and job["Index"] == 1
    assert job["Exception Status"] == job["Next"] == job["Dependency 1"] == job["Dependency 2"] == 0
    inv = decode(x, "Invocation", data[32:40])
    shifts = [0] + [inv[f] for f in
                    ["Size Y shift", "Size Z shift", "Workgroups X shift", "Workgroups Y shift",
                     "Workgroups Z shift"]] + [32]
    values = [1 + ((inv["Invocations"] >> a) & ((1 << (b - a)) - 1))
              for a, b in zip(shifts, shifts[1:])]
    assert values == GRID and inv["Thread group split"] == shifts[3]
    assert decode(x, "Compute Job Parameters", data[40:64])["Job Task Split"] == TASK_SPLIT
    draw = decode(x, "Draw", data[64:192])
    assert draw["State"] == BASE + 0x100 and draw["Thread Storage"] == BASE + 0x140
    for field in ("Textures", "Samplers", "Push uniforms", "Uniform buffers",
                  "Attribute buffers", "Attributes"):
        assert draw[field] == 0
    assert decode(x, "Shader", data[0x100:0x110])["Shader"] == BASE + 0x1000
    ls = decode(x, "Local Storage", data[0x140:0x160])
    assert ls["WLS Instances"] == 31 and ls["TLS Size"] == ls["TLS Base Pointer"] == ls["WLS Base Pointer"] == 0
    assert hashlib.sha256(data[0x1000:]).hexdigest() == CODE_SHA256


def generated(data):
    result = ("// SPDX-License-Identifier: MIT\n"
              "// Generated from Mesa 26.2.6 v7 packers and the G52 r1 NIR compiler.\n"
              "// Do not edit. Rebuild using scripts/mali-host/a2-compute.c.\n"
              "#pragma once\n#include <stdint.h>\n\n"
              f"namespace {NAMESPACE} {{\n")
    for name, start, length in PARTS:
        result += f"constexpr uint32_t {name}Offset=0x{start:x};\n"
        result += f"constexpr uint8_t {name}[{length}]={{\n"
        for i in range(start, start + length, 16):
            result += "    " + ",".join(f"0x{b:02x}" for b in data[i:min(i+16, start+length)]) + ",\n"
        result += "};\n"
    return result + "}\n"


def read_header():
    data = bytearray(0x1000 + PARTS[-1][2])
    text = HEADER.read_text()
    for name, start, length in PARTS:
        matches = re.findall(r"\b" + name + r"\[\d+\]=\{(.*?)\};", text, re.S)
        assert len(matches) == 1
        blob = bytes(int(x, 16) for x in re.findall(r"0x[a-f0-9]{2}", matches[0]))
        assert len(blob) == length
        data[start:start + length] = blob
    assert text == generated(data)
    return data


def main():
    global HEADER, PARTS, BASE, PAYLOAD_SHA256, CODE_SHA256, GRID, TASK_SPLIT, NAMESPACE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emit-from", type=Path,
                        help="verified Mesa generator output; default is read-only validation")
    parser.add_argument("--present", action="store_true", help="check the full-panel blit packet")
    args = parser.parse_args()
    if args.present:
        HEADER = ROOT / "drivers/SprdPlatform/SprdMaliPresentPayload.h"
        PARTS = (*PARTS[:-1], ("Code", 0x1000, 576))
        BASE = 0x04000000
        PAYLOAD_SHA256 = "35d5c183a52c84a8651e1d0ff401c82d88d9170472ab0fafc51753a19aed4c9a"
        CODE_SHA256 = "e48edab4cccbc76626bf4ad578deaad219e1bcf6594093073236e492c6330546"
        GRID = [16, 1, 1, 5, 1920, 1]
        TASK_SPLIT = 7
        NAMESPACE = "SprdMaliPresentPayload"
    data = args.emit_from.read_bytes() if args.emit_from else read_header()
    check(data)
    if args.emit_from:
        HEADER.write_bytes(generated(data).encode("ascii"))
    print("PASS: Mesa G52 r1 code digest and independent v7 "
          + ("present" if args.present else "compute") + " job, state, TLS and grid decode")


if __name__ == "__main__":
    main()
