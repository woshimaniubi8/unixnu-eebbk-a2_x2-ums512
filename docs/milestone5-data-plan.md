# Milestone 5 (part 2): the writable Data volume

Status: plan + workflow draft.  Nothing is enabled by default; the CI input
`data_seed` turns the step on.

## Why

`63.log`/`64.log` show the OS window mounting and APFS finding the system
volume (`disk1s1`, UUID `04769D26-2CA0-4825-B443-E2E5F88300F2`), but the image
carries no writable Data volume: `bsd_init`/launchd later need `/private/var`
(the Data volume) for preferences, containers and swap.  UniXNU's answer is a
Data ramdisk seed: a small APFS image supplied through the payload, which the
kernel mounts as the Data volume.  It is an input to the payload step, not a
kernel change.

## How it is delivered (verified in the UniXNU sources)

| layer | what happens | source |
| --- | --- | --- |
| payload FAT | `Payloads/scratch-data.img` (the seed) | `scripts/unixnu/payload.py:149` |
| DarwinBoot config | `initrd = "Payloads/scratch-data.img"` | `payload.py:179`, `docs/concepts/payload.md` |
| ADT `/chosen` | `ephemeral-storage = 1` (non-persistent Data volume) | `scripts/unixnu/devicetree.py:332` |
| ADT `/chosen/memory-map` | `RAMDisk` (dense seed) or `PackedRAMDisk` (packed seed) | `devicetree.py:340` |
| DarwinBoot | writes the ramdisk address/size into `/chosen/memory-map` | `docs/concepts/payload.md` |
| kernel | mounts the ramdisk as the writable Data volume; no extra patch | same UniXNU kernel the other UniXNU devices use |

Dense vs packed: `prepare-owner-data-fixture.py` produces a raw 32 MiB APFS
image (`owner-data-32.cdr`, checked with `NXSB`), so the board keeps
`[payload.adt] packed_data_ramdisk` unset and the ADT gets `RAMDisk`.
`--packed-data-ramdisk` is only for the `DBRDISK1` extent-table seeds produced
by `seed-data-grow.py` / `seed-data-skip-setup.py`.

## CI steps (draft in `.github/workflows/macos.yml`)

New input: `data_seed` (boolean, default `false`).

New step "Prepare the Data seed" (runs before "Build payload"):

1. `./unixnu ipsw OS` extracts `044-76040-038.dmg` from the pinned IPSW into
   `out/ipsw/21H461/` (checked against `[ipsw.sha256]` in the OS profile).
2. `hdiutil attach -nobrowse -readonly -mountpoint .cache/firmware/target-volumes/os-mount`
   mounts the original OS volume.
3. `scripts/prepare-owner-data-fixture.py` copies the initial `/private/var`
   tree into a fresh 32 MiB APFS image at
   `build/owner-data-fixture/owner-data-32.cdr` and writes `seed.json`
   (volume UUID, sha256, file/byte counts).  It requires the unchanged original
   `sbin/launchd` (sha256 `8cec9a3c…`).
4. `hdiutil detach` unmounts the OS volume.
5. "Build payload" adds `--data-ramdisk build/owner-data-fixture/owner-data-32.cdr`.

All of it is wrapped by `scripts/prepare-data-seed.py` (device-owned), which
also warns when `[payload] size_mib` is too small.

`seed.json` is uploaded with the `macos-logs` artifact for provenance.

## Board change needed (owner of board.toml)

`[payload] size_mib` is 64 today; the kernel (~50 MiB) plus the 32 MiB seed and
the ADT do not fit.  Raise it to 96 (or 100) for a `data_seed` run.  The payload
report then carries a non-null `data_ramdisk_sha256`, and the payload is about
86 MiB instead of 64 MiB, which matters for the boot image packaging the owner
does.

## Expected boot log

```
DarwinBoot: initrd Payloads/scratch-data.img ...
...
apfs: ... RAM Disk ...
... mounted ... /private/var ...        (or: apfs_vfsop_mount ... data volume)
bsd_init: ... (continues)
launchd: ...
```

and `Failed to open swap file` should disappear or be retried on the Data
volume.  The exact APFS lines depend on how the kernel binds the ramdisk; the
first run tells us which mount path it takes.

## Rollback

Remove the `data_seed` input (or the `--data-ramdisk` argument): the payload
goes back to kernel + ADT only, exactly the current behaviour.  The kernel and
the binding table are untouched by this milestone.

## Risks / open items

* Payload size and boot image packaging: a 32 MiB seed grows the payload by
  ~32 MiB; the owner's boot image packaging must accommodate it.
* The seed's APFS volume is named `OwnerData` and has no role flags; if the
  kernel insists on a Data-role volume, the next log shows it and the fixture
  script needs the role set (macOS `diskutil apfs addVolume ... -role D`).
* `--skip-setup` (Setup/Hello pages) is optional and needs
  `[payload] skip_setup_tool`; not part of the first run.
* The Data seed is ephemeral (`ephemeral-storage`); everything written to
  `/private/var` is lost on reboot, which is fine for bring-up.
