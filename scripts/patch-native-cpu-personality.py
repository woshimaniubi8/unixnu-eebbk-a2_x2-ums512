#!/usr/bin/env python3
"""Keep the AppleARMCPU personality from binding Unisoc CPU nubs.

Native ArmCpu owns the boot CPU and starts secondaries only with sheng-smp=1.
The original AppleARMCPU matches the remaining device_type=cpu nubs and waits
forever for AppleARMCPU-BootCPU-Initialized, which the native driver does not
publish. Remove only that automatic match from AppleARMPlatform metadata;
retain its executable, metaclasses and every other personality.
"""
import argparse
import copy
import hashlib
import importlib
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

IDENTIFIER = "com.apple.driver.AppleARMPlatform"
PERSONALITY = "AppleARMCPU"
EXPECTED = {"IOClass": "AppleARMCPU", "IOProviderClass": "IOPlatformDevice",
            "IONameMatch": "cpu", "IOProbeScore": 100}


def prelink_tools(path):
    sys.path.insert(0, str(path))
    return (importlib.import_module("native_prelink"),
            importlib.import_module("emit_prelink"))


def patch_kernel(data, tools):
    native, emitter = tools
    image = native.macho(data)
    symbols = native.exports(data, image)
    if not any("__ZTV6ArmCpu" in names for names in symbols.values()):
        raise ValueError("refusing to exclude AppleARMCPU without the native ArmCpu driver")
    regions = [s for s in image["segments"] if s["name"] == "__PRELINK_INFO"]
    if len(regions) != 1:
        raise ValueError("expected one prelink metadata segment")
    region = regions[0]
    offset = native.backed_offset(image["segments"], region["va"],
                                  region["filesize"], len(data))
    raw = data[offset:offset + region["filesize"]]
    xml = raw.split(b"\0", 1)[0]
    if any(raw[len(xml):]):
        raise ValueError("nonzero data after the prelink XML")
    metadata = native.unserialize_xml(raw)
    entries = [e for e in metadata["_PrelinkInfoDictionary"]
               if e["CFBundleIdentifier"] == IDENTIFIER]
    if len(entries) != 1:
        raise ValueError("expected one AppleARMPlatform record")
    personalities = entries[0]["IOKitPersonalities"]
    if PERSONALITY not in personalities:
        return data, dict(changed=False, reason="AppleARMCPU automatic match already absent")
    if personalities[PERSONALITY] != EXPECTED:
        raise ValueError("AppleARMCPU personality changed; review the new matching contract")

    # Use the existing prelink serializer for the exact record, rather than
    # reserializing unrelated metadata or relocating any mapped segment.
    fragment = ET.tostring(emitter.to_xml({PERSONALITY: EXPECTED}),
                           encoding="utf-8").replace(b" />", b"/>")[6:-7]
    if xml.count(fragment) != 1:
        raise ValueError("expected one canonical AppleARMCPU personality fragment")
    modified_xml = xml.replace(fragment, b"", 1)
    expected_metadata = copy.deepcopy(metadata)
    platform = next(e for e in expected_metadata["_PrelinkInfoDictionary"]
                    if e["CFBundleIdentifier"] == IDENTIFIER)
    del platform["IOKitPersonalities"][PERSONALITY]
    if native.unserialize_xml(modified_xml) != expected_metadata:
        raise ValueError("personality exclusion changed unrelated metadata")
    replacement = modified_xml + bytes(len(raw) - len(modified_xml))
    modified = data[:offset] + replacement + data[offset + len(raw):]
    assert len(modified) == len(data)
    assert modified[:offset] == data[:offset]
    assert modified[offset + len(raw):] == data[offset + len(raw):]
    return modified, dict(changed=True, excluded_personality=PERSONALITY,
                          bundle=IDENTIFIER, metadata_offset=offset,
                          metadata_bytes=len(raw), removed_xml_bytes=len(fragment),
                          executable_and_binding_addresses_unchanged=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--prelink-tools", type=Path,
                        default=Path.cwd() / "kits/XnuPrelink")
    args = parser.parse_args()
    original = args.kernel.read_bytes()
    modified, report = patch_kernel(original, prelink_tools(args.prelink_tools))
    output = args.output or args.kernel
    if args.output or modified != original:
        output.write_bytes(modified)
    report.update(input_sha256=hashlib.sha256(original).hexdigest(),
                  output_sha256=hashlib.sha256(modified).hexdigest(),
                  bytes=len(modified), hardware_tested=False)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print("bbk cpu-personality: " + json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
