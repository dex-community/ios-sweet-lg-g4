# Device testing: one boot, one evidence file

What the phone does for a test has not changed: the kernel is packed with the
TWRP ramdisk and the phone's `dt.img`, and it is started with `fastboot boot`.
Nothing is written to the phone, and `userdata` is only ever mounted
read-only. What changed is the procedure around it.

## The cycle

1. If the test is about the shim, `fastboot boot shim/out/shim-boot.img`.
   The phone returns to TWRP by itself (watchdog + `LK_RECOVERY_MODE`).
2. Boot the kernel under test and run the battery:

   ```bash
   HFSPLUS=/path/to/hfsplus \
     tools/device-test.sh /tmp/kci/Image docs/test-$(date +%F).txt
   ```

   The script:
   - packs the Image (gzip) with `tools/pack-twrp-kernel.sh`;
   - if the phone is already in fastboot it does not try `adb reboot`;
   - boots, waits for adb, runs the battery, and saves everything in the
     evidence file.

3. Read the evidence file; `HFS+ rootfs: OK` and `exit=0` mean the volume
   mounted and `SystemVersion.plist` was readable.

One kernel boot is enough. The shim log at `0x50000000` survives the boot,
so the same boot that mounts the rootfs also prints `C2OK` and the eMMC
lines. The old "shim boot, then a second diagnostic kernel boot" is gone.

## What the battery checks

| check | why it matters |
| --- | --- |
| `uname -a`, `/proc/cmdline` | which kernel actually booted |
| `zcat /proc/config.gz` | `DEVMEM`, `HFS_FS`, `HFSPLUS_FS`, `IKCONFIG` at runtime, not from the artifact |
| `/proc/filesystems`, `/dev/mem` | the drivers and the RAM reader exist |
| `mount`, `dmesg`, `df`, `ls /data` | the HFS+ volume mounted, and why (the patch prints `no backup volume header ... primary one is used alone`) |
| `SystemVersion.plist` | it is the iOS rootfs, not another HFS volume |
| `sha256sum` of `/usr/lib/dyld` | reads through the mount are correct |
| the same file through `hfsplus` on the raw block device | a second, independent reader must give the same hash |
| `dd` from `/dev/mem` | the shim's `C2OK` / `EMMC` lines from the previous shim run |

The userspace reader is optional (`HFSPLUS=...`). It is useful in exactly one
case: the kernel mount is broken but the volume may still be readable. It has
no size accounting: on this 18.35 GB volume it prints
`Total filesystem size: -888385536` (32-bit signed overflow). Use it to
extract, not to compute free space.

## Collecting from a kernel that is already running

```bash
tools/device-test.sh --collect docs/collect.txt
```

No packing, no boot; same battery on whatever is running.

## Known harmless outputs

- `verify_bootimg.py` prints `FAIL dtb ... does not overlap kernel` for this
  packing. The LG stock layout is `tags=0x100` with the kernel at `0x8000`;
  the stock image has the same overlap and boots, and the CI kernel booted
  with it. A packing with `tags=0x400000`, `ramdisk=0x1000000` would make
  the check pass; not needed for the tests.
- `hfsplus` prints `HFS+ Private Data` with trailing NUL bytes; cosmetic.
- TWRP mounts `userdata` on `/data` by itself. The script accepts that and
  does not remount it.

## Last evidence

`docs/test-2026-11-10-hfs-mount.txt`:

- kernel `3.10.108-gacc9bfa9-dirty`, CI run `42873352434`;
- `hfsplus: no backup volume header (signature 0x0000), the primary one
  (0x4858) is used alone` and `mounting read-only`;
- `iPhone OS 16.7.16 (20H392)`;
- `/usr/lib/dyld` sha256 `58068345...3757` from the mount and from
  `hfsplus` on the raw device;
- shim log: `C2OK v=00000009 rsp=48B72E52`, `EMMC sec=03A3E000`.
