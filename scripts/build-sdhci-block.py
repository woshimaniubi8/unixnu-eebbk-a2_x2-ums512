#!/usr/bin/env python3
"""Build the SdhciBlock component archive and bind its IOStorageFamily imports.

IOStorageFamily (IOBlockStorageDevice, IOMedia, ...) is not part of the XNU
kernel link.  UniXNU binds the block transports to the original kernelcache
through the qualified storage audit; for this device the same dispatch table,
protected constructor, base destructor and metaclass addresses were derived from
the pinned iPadOS 17.7.11 kernelcache (drivers/SdhciBlock/storage-bindings.json,
see build/derive-sdhciblock-bindings2.py) and are bound here with absolute
symbols:

  1. compile NativeSdhciBlock.cpp with the kernel's own compile record (the
     IOPlatformExpert.cpo recipe), exactly like build-arm-driver.py;
  2. read the class vtable relocations to learn which IOBlockStorageDevice
     symbol sits in which slot;
  3. emit .set bindings for every undefined IOStorageFamily import and compile
     them into original-bindings.o;
  4. archive both objects into build/sdhciblock/libSdhciBlock.a, which the
     defconfig force-loads (CONFIG_XNU_EXTRA_LD_ARCHIVES).

The kernel addresses in the bindings JSON stay valid because the archive is
force-loaded at the end of LD_KERNEL_ARCHIVES: earlier kernel symbols keep their
addresses, and the IOStorageFamily kext itself is placed by the prelink step at
original_va + slide.
"""
import argparse
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "components.toml"
RECORD = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
STORAGE_INCLUDE = ROOT / "build/storage-abi/include"
BINDINGS = ROOT / "drivers/SdhciBlock/storage-bindings.json"
SLOTS = 190

SPECIAL = {
    "__ZTV20IOBlockStorageDevice": "vtable",
    "__ZN20IOBlockStorageDevice10gMetaClassE": "metaclass",
    "__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass": "constructor",
    "__ZN20IOBlockStorageDeviceD2Ev": "destructor",
}


def run(command, **kwargs):
    result = subprocess.run(command, **kwargs)
    if result.returncode:
        raise SystemExit(result.stdout[-3500:] if isinstance(result.stdout, str) else result.returncode)
    return result


def macho_object(data):
    magic, cpu, subtype, kind, count, size = struct.unpack_from("<6I", data)
    if (magic, cpu, subtype, kind) != (0xFEEDFACF, 0x100000C, 0, 1):
        raise SystemExit("Unexpected platform object architecture")
    commands, offset = [], 32
    for _ in range(count):
        command, length = struct.unpack_from("<II", data, offset)
        commands.append((command, offset, length))
        offset += length
    return commands


def parse_symbols(data, commands):
    command = next(c for c in commands if c[0] == 2)
    symoff, nsyms, stroff, strsize = struct.unpack_from("<4I", data, command[1] + 8)
    strings = data[stroff:stroff + strsize]
    symbols = []
    for index in range(nsyms):
        strx, kind, sect, desc, value = struct.unpack_from("<IBBHQ", data, symoff + index * 16)
        end = strings.find(b"\0", strx)
        name = strings[strx:end].decode() if 0 <= strx < strsize and end >= 0 else ""
        symbols.append(dict(name=name, kind=kind, sect=sect, desc=desc, value=value))
    return symbols


def parse_sections(data, commands):
    sections = []
    for command, offset, length in commands:
        if command != 0x19:
            continue
        nsects = struct.unpack_from("<I", data, offset + 64)[0]
        for index in range(nsects):
            at = offset + 72 + index * 80
            sectname, segname, address, size, file_offset = struct.unpack_from("<16s16sQQI", data, at)
            reloff, nreloc = struct.unpack_from("<II", data, at + 56)
            sections.append(dict(name=sectname.rstrip(b"\0").decode(), segment=segname.rstrip(b"\0").decode(),
                                 address=address, size=size, offset=file_offset,
                                 reloff=reloff, nreloc=nreloc))
    return sections


def vtable_bindings(data, commands, symbols, sections, slot_count):
    vtable_symbol = next((s for s in symbols if s["name"] == "__ZTV16NativeSdhciBlock"), None)
    if vtable_symbol is None:
        raise SystemExit("the object has no NativeSdhciBlock vtable")
    section = next((s for s in sections if s["address"] <= vtable_symbol["value"] <
                    s["address"] + s["size"]), None)
    if section is None:
        raise SystemExit("the vtable is outside every section")
    relocations = {}
    for index in range(section["nreloc"]):
        address, info = struct.unpack_from("<iI", data, section["reloff"] + index * 8)
        if (info >> 27) & 1 and (info >> 28) == 0 and ((info >> 25) & 3) == 3:
            relocations[address] = symbols[info & 0xFFFFFF]["name"]
    start = vtable_symbol["value"] - section["address"] + 16
    return [relocations.get(start + index * 8, "") for index in range(slot_count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", nargs="?", default="SdhciBlock")
    args = parser.parse_args()

    import tomllib
    catalog = {entry["id"]: entry for entry in tomllib.loads(CATALOG.read_text())["component"]}
    spec = catalog[args.component]
    identifier = spec["id"]
    runtime = spec.get("legacy_name", identifier)
    source_files = [name for name in spec.get("source_files", []) if name.endswith(".cpp")]
    if not source_files:
        raise SystemExit(f"{identifier}: no C++ source in the catalog entry")
    source = ROOT / "drivers" / identifier / source_files[0]
    outputs = spec.get("outputs", [])
    archive_output = next((path for path in outputs if path.endswith(".a")), None)
    if archive_output is None:
        raise SystemExit(f"{identifier}: the catalog entry has no archive output")
    out = ROOT / Path(archive_output).parent
    out.mkdir(parents=True, exist_ok=True)
    destination = out / (runtime + ".o")

    if not BINDINGS.is_file():
        raise SystemExit(f"{BINDINGS} is missing; regenerate it from the pinned kernelcache")
    bindings = json.loads(BINDINGS.read_text())
    slot_count = len(bindings["slots"])
    if slot_count != SLOTS:
        raise SystemExit(f"the binding table has {slot_count} slots, expected {SLOTS}")

    record = json.loads(RECORD.read_text().strip().rstrip(","))
    command, index = [], 0
    arguments = record["arguments"]
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(str(source))
        elif value == "-o":
            command.extend(["-o", str(destination)])
            index += 1
        elif value in ("-MF", "-MJ", "-MT", "-MD", "-MP"):
            if value in ("-MF", "-MJ", "-MT"):
                index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif not value.startswith("-W"):
            command.append(value)
        index += 1
    command += ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-c99-designator",
                "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
                "-Wno-unnecessary-virtual-specifier", "-Wno-deprecated-declarations",
                "-I", str(ROOT / ".cache/xnu-work/osfmk"), "-I", str(STORAGE_INCLUDE)]
    (out / "compile-command.json").write_text(json.dumps(command, indent=2) + "\n")
    result = subprocess.run(command, cwd=record["directory"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (out / "compile.log").write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout[-3500:])

    data = destination.read_bytes()
    commands = macho_object(data)
    deployment = None
    for command_id, offset, length in commands:
        if command_id == 0x32:
            deployment = struct.unpack_from("<II", data, offset + 8)
    if deployment != (2, 0x110000):
        raise SystemExit(f"Unexpected platform deployment {deployment}")

    symbols = parse_symbols(data, commands)
    sections = parse_sections(data, commands)
    slots = vtable_bindings(data, commands, symbols, sections, slot_count)
    by_name = {}
    for slot, name in enumerate(slots):
        if name:
            by_name.setdefault(name, []).append(slot)

    defined = {symbol["name"] for symbol in symbols if symbol["sect"] != 0 and symbol["kind"] & 0x0E}
    resolved = {}
    unresolved = []
    for symbol in symbols:
        name = symbol["name"]
        if not name or symbol["sect"] != 0 or not (symbol["kind"] & 0x01):
            continue
        if name in SPECIAL:
            resolved[name] = bindings[SPECIAL[name]]
        elif "IOBlockStorageDevice" in name:
            # Only IOStorageFamily imports are bound here; IOService, OSObject,
            # IOLib, ... stay undefined and are resolved by the kernel link.
            if name not in by_name:
                unresolved.append(name)
                continue
            if len(by_name[name]) != 1:
                raise SystemExit(f"{name} appears in several vtable slots {by_name[name]}")
            resolved[name] = bindings["slots"][by_name[name][0]]
        elif name.startswith("__ZTI"):
            unresolved.append(name)
    if unresolved:
        raise SystemExit("unbound IOStorageFamily imports: " + ", ".join(sorted(unresolved)))

    assembly = out / "original-bindings.S"
    assembly.write_text("".join(f".globl {name}\n.set {name}, {address}\n"
                                for name, address in sorted(resolved.items())))
    bindings_object = out / "original-bindings.o"
    # Reuse the record's target/sysroot flags for the assembly, dropping the
    # source, the language selector, forced includes and the old output path.
    # The explicit "-x assembler-with-cpp" wins over anything left behind.
    assembler, index = [], 0
    while index < len(command):
        value = command[index]
        if value in ("-x", "-include"):
            index += 2
            continue
        if value.startswith(("-x", "-include")):
            index += 1
            continue
        if value in ("-o", str(source), str(destination)) or value.startswith("-std="):
            index += 1
            continue
        assembler.append(value)
        index += 1
    assembler = [value for value in assembler if value != "-c"]
    run([*assembler, "-x", "assembler-with-cpp", "-c", str(assembly), "-o", str(bindings_object)],
        cwd=record["directory"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    archive = out / ("lib" + identifier + ".a")
    archive.unlink(missing_ok=True)
    subprocess.run(["xcrun", "ar", "rcs", str(archive), str(destination), str(bindings_object)], check=True)
    print(f"{identifier}: {len(resolved)} IOStorageFamily imports bound "
          f"({len(slots)} slots, {sum(1 for name in slots if name in defined)} overridden)")
    print(f"{identifier}: {archive}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
