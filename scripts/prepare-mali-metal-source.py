#!/usr/bin/env python3
"""Prepare the pinned Viva userspace frontend for the G52/iOS 17 build.

This builds the native transport and an owner-only Metal render probe. It does
not publish a system accelerator: actual render results and shared IOSurface
synchronization remain qualification gates before UI deployment.
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

    def emit(relative, data):
        path=root/relative
        before=path.read_bytes() if path.exists() else b''
        pending.append((path,data))
        changes.append(dict(path=relative,before_sha256=hashlib.sha256(before).hexdigest(),
                            after_sha256=hashlib.sha256(data).hexdigest()))

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
    native=Path(__file__).resolve().parents[1]/'drivers/SprdPlatform/userspace'
    emit('vgpu/vgpu_iokit.c',(native/'vgpu_native_mali.c').read_bytes()+b'\n#include "native_mali_iokit.inc"\n')
    emit('vgpu/native_mali_iokit.inc',(native/'native_mali_iokit.c').read_bytes())
    emit('vgpu/mali_native_wire.h',(native/'mali_native_wire.h').read_bytes())
    for name in ['native-metal-probe.m','native-metal-probe.metal','native-metal-probe.entitlements','native-metal-surface.inc','native-metal-surface-bridge.inc']:
        emit('ios/'+name,(native/name).read_bytes())
    edit('vgpu/vgpu.c',[(
        '        const char *env = getenv("VGPU_BACKEND");\n'
        '#if TARGET_OS_IPHONE || defined(VGPU_DEFAULT_IOKIT)\n'
        '        g_backend = &vgpu_backend_iokit;\n'
        '#else\n'
        '        g_backend = &vgpu_backend_mock;\n'
        '#endif\n'
        '        if (env && !strcmp(env, "iokit")) g_backend = &vgpu_backend_iokit;\n'
        '        if (env && !strcmp(env, "mock")) g_backend = &vgpu_backend_mock;',
        '        g_backend = &vgpu_backend_iokit; /* NativeMali only: no simulated execution. */',1)])
    edit('libdrm-xnu/src/xf86drm_xnu.c',[(
        '    d->version_minor = 5;',
        '    d->version_minor = 2; /* No timestamp/priority ABI is exposed by NativeMali. */',1)])
    mesa_actual=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root/'mesa/src',text=True).strip()
    if mesa_actual!=MESA:
        raise SystemExit(f'Unexpected Mesa source: {mesa_actual}')
    edit('mesa/src/src/gallium/drivers/panfrost/pan_device.c',[(
        'dev, 128 * 1024 * 1024, PAN_BO_INVISIBLE | PAN_BO_GROWABLE, "Tiler heap"',
        'dev, 16 * 1024 * 1024, PAN_BO_INVISIBLE, "NativeMali committed tiler heap"',1)])
    edit('mesa/src/src/panfrost/lib/kmod/panfrost_kmod.c',[(
        '   props->supported_bo_flags = PAN_KMOD_BO_FLAG_EXECUTABLE |\n'
        '                               PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT |\n'
        '                               PAN_KMOD_BO_FLAG_NO_MMAP;',
        '   props->supported_bo_flags = PAN_KMOD_BO_FLAG_EXECUTABLE |\n'
        '                               PAN_KMOD_BO_FLAG_NO_MMAP;',1)])
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
        ('        if (!res) {\n'
         '            templ.bind &= ~(unsigned)PIPE_BIND_SHARED;\n'
         '            res = dev->screen->resource_create(dev->screen, &templ);\n'
         '            if (!res) return NULL;\n'
         '            vmg_texture *t = texture_wrap(res, d);\n'
         '            t->surface_id = surface_id; t->plane = plane; t->surface_copy = true;\n'
         '            t->surface = (void *)IOSurfaceLookup(surface_id);\n'
         '            return t;\n'
         '        }\n'
         '    } else {\n'
         '        // llvmpipe:',
         '        if (!res) return NULL; /* NativeMali requires the original surface pages, no CPU copy. */\n'
         '    } else {\n'
         '        // llvmpipe:',1),
        ('pitch not 64-aligned, copied (texture','pitch not 64-aligned, rejected (texture',1),
        ('import refused, copied (texture','import refused, rejected (texture',1),
    ])
    edit("driver/metal/VMDevice.m", [
        ('    const char *strict = getenv("VMG_ISA_STRICT"), *isa = getenv("VMG_ISA");\n'
         "    if (!failure || !strict || *strict == '0' || !isa || *isa == '0') return NO;",
         '    if (!failure) return NO; /* Report rejected pipelines instead of successful inert states. */', 1),
    ])
    for path, modified in pending:
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(modified)
    record = dict(upstream_repository="https://github.com/Nik300/viva-gpu", upstream_commit=VIVA,
                  mesa_commit=MESA, target="arm64-apple-ios17.0", gpu_product_id="0x7402",
                  compiler_path="AIR -> NIR -> Mesa Bifrost v7", direct_valhall_v9_enabled=False,
                  transport='NativeMali type-0 connection, selectors 1-10/12-15',
                  vgpu_version='1.0',drm_panfrost_version='1.2',tiler_heap_committed_bytes=16*1024*1024,
                  mock_selectable=False,iosurface_import_compiled=True,iosurface_cpu_copy_fallback=False,
                  iosurface_import_qualified=False,metal_probe='native-metal-probe',
                  unsuccessful_pipelines_return_error=True, runtime_qualified=False, deployed=False, changes=changes)
    (root / "g52-source-receipt.json").write_text(json.dumps(record, indent=2) + "\n")
    return record


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.source.resolve()), indent=2))
