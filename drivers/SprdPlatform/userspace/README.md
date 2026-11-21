# NativeMali / Viva userspace integration

This directory adapts pinned Viva `9ebe5e46f74f6d75fce1d76b2ea4ea603cb63637`
and Mesa `8e87a503ebd91408c88596d1b9eeb5f4c0b2088d` to the board's existing
`NativeMaliGpuService` type-0 connection. The service is also the board's real
`IOAcceleratorES` subclass and advertises this bundle to original Metal
discovery; it remains the only GPU owner.

`prepare-mali-metal-source.py` installs the transport into a checked upstream
tree. The existing Viva Metal/AIR/NIR and Mesa Bifrost compiler paths are reused;
the G57-only direct Valhall encoder and mock backend selection are disabled.

The connection reports Viva 1.0 (no trap, completion page, batch or surface
promise interface) and Panfrost 1.2 (no timestamp or priority API). Physical
properties are returned by NativeMali selector 12 from its measured GPU probe.
Selector 13 synchronizes an owned BO range using the existing cache maintainer.
NOEXEC maps through the real execute-never PTE attribute. Every CPU mapping is
16 KiB aligned, while GPU page tables retain their existing 4 KiB granule.

The board currently uses a fully committed 16 MiB tiler heap. Demand allocation
and fault-grown heaps are not advertised; heap exhaustion is an error. BO and
syncobj limits are 64 and 256 per connection. GPU work is submitted and waited
synchronously. Fence failures are sticky; timeout retains the real pending
fence and allocations. Close uses the existing kernel quiesce/quarantine path.

`native-metal-probe` explicitly loads the bundle, compiles actual Metal pipeline
states from its companion precompiled metallib, and checks 64 compute results
and a 4096-pixel rendered triangle with hardware submission counters. It uses
only the existing owner's exact `NativeSprdMaliUserClient` entitlement. Its
hardware PASS line is required; successful compilation alone is not a pass.

The system discovery path grants NativeMali type-0 opens only to `backboardd`
and `SpringBoard`; all other callers retain the administrator check. The
private probe still has its separate IOSurface parent/child validation gate.
Publishing the accelerator does not qualify system UI composition: original
IOSurface ownership, child import, display fences, and a successful SpringBoard
check-in must still be observed on-device.

Host checks (from the device repository):

```sh
python3 scripts/run-mali-session-tests.py
python3 scripts/run-mali-native-backend-tests.py /path/to/pinned/viva-gpu
```

The host transport is a fault injector; it performs no GPU execution. Production
transport records have independent MIT declarations and a kernel layout test.
The Viva-facing adapter is GPL-3.0-or-later, matching that userspace project;
the kernel remains separately built and linked under its existing license.
