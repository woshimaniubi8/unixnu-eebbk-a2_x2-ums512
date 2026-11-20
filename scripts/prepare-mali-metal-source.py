#!/usr/bin/env python3
"""Prepare the pinned Viva userspace frontend for the G52/iOS 17 build.

This does not publish an accelerator or deploy a plugin. The existing NativeMali
ABI is not the Viva UAPI; kernel transport and IOSurface synchronization must be
qualified separately before the built bundle can enter a device image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

VIVA = "9ebe5e46f74f6d75fce1d76b2ea4ea603cb63637"
MESA = "8e87a503ebd91408c88596d1b9eeb5f4c0b2088d"


def prepare(root):
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    if actual != VIVA:
        raise SystemExit(f"Unexpected Viva source: {actual}")
    changes = []
    pending = []

    def edit(relative, replacements):
        path = root / relative
        before = path.read_bytes()
        text = before.decode().replace("\r\n", "\n")
        for old, new, count in replacements:
            if text.count(old) != count:
                raise SystemExit(f"Source drift: {relative}: expected {count} occurrences of {old!r}")
            text = text.replace(old, new)
        modified = text.encode()
        pending.append((path, modified))
        changes.append(dict(path=relative, before_sha256=hashlib.sha256(before).hexdigest(),
                            after_sha256=hashlib.sha256(modified).hexdigest()))

    edit("ios/build.sh", [("arm64-apple-ios16.0", "arm64-apple-ios17.0", 1)])
    edit("driver/vmg_screen.c", [
        ('if (dev->screen && strstr(dev->screen->get_name(dev->screen), "Mali-G57")) g_gpu_product_id = 0x9093;',
         'if (dev->screen && strstr(dev->screen->get_name(dev->screen), "Mali-G52")) g_gpu_product_id = 0x7402;\n'
         '        else {\n'
         '            snprintf(error, error_size, "G52 frontend: unexpected physical GPU");\n'
         '            if (dev->screen) dev->screen->destroy(dev->screen);\n'
         '            vgpu_close(dev->fd); free(dev); return NULL;\n'
         '        }', 1),
        ('vmg_isa_options_from_env(&iopt);',
         'memset(&iopt, 0, sizeof(iopt)); /* G52 v7: AIR -> NIR -> Mesa Bifrost, never the G57 v9 encoder. */', 2),
    ])
    edit("driver/metal/VMDevice.m", [
        ('    const char *strict = getenv("VMG_ISA_STRICT"), *isa = getenv("VMG_ISA");\n'
         "    if (!failure || !strict || *strict == '0' || !isa || *isa == '0') return NO;",
         '    if (!failure) return NO; /* Report rejected pipelines instead of successful inert states. */', 1),
    ])
    for path, modified in pending:
        path.write_bytes(modified)
    record = dict(upstream_repository="https://github.com/Nik300/viva-gpu", upstream_commit=VIVA,
                  mesa_commit=MESA, target="arm64-apple-ios17.0", gpu_product_id="0x7402",
                  compiler_path="AIR -> NIR -> Mesa Bifrost v7", direct_valhall_v9_enabled=False,
                  unsuccessful_pipelines_return_error=True, runtime_qualified=False, deployed=False, changes=changes)
    (root / "g52-source-receipt.json").write_text(json.dumps(record, indent=2) + "\n")
    return record


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.source.resolve()), indent=2))
