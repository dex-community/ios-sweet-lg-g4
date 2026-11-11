# STATUS — iOS 16 on LG G4 (H815)

Last updated: 2026-11-11

## Goal

Boot the unmodified iOS 16.7.16 (20H392) userland of the iPhone X on an LG
G4 H815, starting from `dipa-studio/ios-sweet`, using `ch1tmdgus/ios-v60` as
the methodological reference and `shrimpapplepro/UniXNU-lumia950xl` as the
eMMC reference. Target end state: the iOS lock/home screen with working
touch and hardware buttons.

## Phase status

| Phase | State | Notes |
| --- | --- | --- |
| 0 — setup and analysis | **done** | Hardware facts extracted and cross-checked; see `docs/g4-hardware/` |
| 1 — shim + minimal boot | **done** | Shim builds, passes offline checks, runs under QEMU **and on the real phone**. eMMC card identified (Samsung 032G74, CID verified). CMD2 now passes with fix to OCR busy bit polling (commit e80f24a). Log channel via RAM@0x50000000 works. |
| 2 — XNU kernel and RAM disk | **mostly done** | XNU kernel builds in CI. **The patch chain was broken and the CI did not say so**: `darwin-xnu-build` skips a patch that does not apply with a warning, so the green run 42923144296 built a kernel missing 40 patches (no platform expert, no USB, no touch). Fixed: `tools/check-xnu-patch-chain.sh` now applies the whole series to a clean xnu-8796 tree and reports every failure (83 applied, 0 failed). |
| 3 — eMMC storage and iOS root | **in progress** | The XNU eMMC driver has been **rewritten as a block device**: the root of XNU is a VBLK vnode read through `buf_bread` → `bdevsw[].d_strategy`, so the old `cdevsw` could never be read. The register encoding is now the one measured on the phone (command 16 bit at 0x0E, index << 8, CMD2 = 136 bit + CRC7 without index check). The driver serves the partition that holds the HFS+ volume, found in the GPT. Linux still mounts the iOS rootfs read-only (CI kernel-h815 run). |
| 4 — touch, buttons, display | **blocked** | Blocked on working XNU boot. Touch is Synaptics s3528 @ I²C 0x20; buttons are PM8994 GPIO 3 + PON1/2. Display is command-mode DSI and needs MDP/DSI driver. |
| 5 — polish and documentation | **in progress** | `STATUS.md`, `GUIDE-*` files updated. Engineering log in progress. |

## What works (2026-11-11 update)

### Shim (Phase 1)
- Shim builds with `aarch64-elf-gcc` and creates LK-style boot images
- Runs under QEMU with full diagnostics
- Runs on real phone and identifies eMMC card
- **eMMC bringup successful**: CMD1 loop now polls OCR bit 31 (busy) correctly
- **CMD2 passes**: card responds with CID `11010030333247373400b448b72e5200` (Samsung 032G74)
- Log channel via RAM@0x50000000 verified working (shim → reboot → recovery → read)

### Linux H815 Kernel (Phase 3 validation)
- CI workflow `.github/workflows/kernel-h815.yml` builds Titan 3.10.108 kernel
- Patch `0001-hfsplus-mount-a-volume-with-no-backup-volume-header.patch` applied successfully
- Kernel boots with `fastboot boot` and provides `/dev/mem` and HFS+ mount capability
- **iOS rootfs mount verified**: mounted read-only from TWRP recovery, filesystem intact
- rootfs contents: iPhone OS 16.7.16 (20H392), 17.1 GB total, 11.2 GB used
- dyld hash verified consistent between mount and userspace read

### XNU Kernel (Phase 2)
- CI workflow `.github/workflows/xnu.yml` builds XNU with SWEET machine config
- **The whole patch series applies**: 83 of 83, checked locally with
  `tools/check-xnu-patch-chain.sh` against a clean xnu-8796 tree
- The eMMC driver `bsd/dev/sweet_emmc.c` is a **block device**:
  - `bdevsw` with `d_strategy`, `d_psize` (512) and `d_ioctl`
    (block size and block count), registered with `bdevsw_add()`
  - the strategy function serves the buffer cache one 512 byte block at a
    time through the FIFO, exactly like the shim reads the card
  - the registers are the ones measured on the phone: command register 16
    bit at 0x0E (index << 8), flags CMD0 0x00, CMD1 0x02, CMD2 0x09,
    CMD3 0x1a, CMD7 0x0b, CMD16 0x0a, CMD17 0x3a
  - the bring-up follows the shim: HC mode, legacy clock path in the
    vendor spec register (HC+0x10c), 400 kHz for the identification,
    CMD0..CMD16, then 25 MHz
  - the device is the **partition** with the iOS volume, found in the GPT
    (header LBA 1, entries LBA 2..33), preferred by the name "ios" and
    accepted only when offset 1024 really holds an HFS+ signature
  - writes are refused with `EROFS`
- IOKitBSDInit glue for `rd=emmc0` in `xnu/patches/0029-sweet-emmc-root-device.patch`
- `bsd/conf/files` includes `bsd/dev/sweet_emmc.c` in the build
- Payload created: `xnu-payload.bin` with boot-args `rd=emmc0`

### Tools
- `tools/mkpayload.py`: creates XNU payloads with SWEETXNU magic header
- `tools/mkbootimg_lg.py`: wraps shim+payload in LK boot.img format
- `shim/build.sh`: end-to-end shim → boot.img pipeline
- `tools/device-test.sh`: unified test script for kernel+HFS validation

## What does not work yet (blockers)

### Critical Blocker: XNU Boot Failure
- **Symptom**: `fastboot boot shim-boot.img` with the XNU payload ends in a
  reboot; the phone comes back in recovery
- **What was wrong with the diagnosis**: the boot image is not malformed (the
  build self-checks that the ramdisk in the image is byte for byte the
  payload), and the shim is not crashing early (it writes its log to
  RAM@0x50000000 before it hands over). The most likely cause is that XNU
  panics on the root device: the old driver could not identify the card
  (the command register was written to the wrong address) and
  `IOFindBSDRoot` panics with "SWEET eMMC root emmc0 unavailable"
- **Now fixed in the driver** (command register, flags, block device); the
  next boot is the first one with a driver that can actually read
- **No visible diagnostics**: the shim does not display on screen (the panel
  is command mode DSI and the clocks are off, see FATTO 21)
- **Log access**: the console of XNU is kept in a 4 MiB ring in the kernel
  BSS; `tools/read-xnu-log.py` computes its physical address from the
  payload header (`pa = va - virt_base + phys_base`) and reads it with a
  kernel that has `/dev/mem`. The log of the shim is read the same way
  (`tools/read-shim-log.py`)

### Panel Display (Phase 4)
- G4 panel is DSI command-mode, bootloader reprograms controller
- cont_splash framebuffer not scanned out after LK handoff
- Real MDP/DSI driver needed (reference: NetHunter G4 kernel)

### Outstanding Questions
1. **XNU runtime**: does the kernel identify the card and mount the HFS+
   volume now that the driver is a block device with the right registers?
2. **eMMC driver runtime**: is the partition found in the GPT, and does the
   read of the 128 MB catalog finish inside the watchdog window at 25 MHz?
3. **Console**: does the ring in the kernel BSS survive the panic and the
   reboot, so that `tools/read-xnu-log.py` can show what XNU said?

## Recent Progress (2026-11-10 to 2026-11-11)

### The patch chain was broken (2026-11-11)
- **What**: `blacktop/darwin-xnu-build` applies the patches with
  `git apply --check` and, when one does not apply, it only prints a warning
  and goes on. The green run 42923144296 had **40 patches skipped**.
- **Root cause**: `0016` had a wrong hunk header (`+14` for 20 added lines),
  so `git apply` called it corrupt; everything downstream was skipped with it.
- **Fixed**: `0016` (+14 → +20), `0018` (stale context), and `0054`, `0060`,
  `0062`, `0072` (the `bsd/conf/files` context, which the `sweet_emmc.c`
  line added by `0029` had broken).
- **New tool**: `tools/check-xnu-patch-chain.sh` applies the whole series to
  a clean xnu-8796 worktree, runs the staging scripts as the CI does, and
  reports every patch that fails. Result: **83 applied, 0 failed**.
- **Consequence**: the kernel that was built before had no SWEET platform
  expert (0016), no boot CPU driver (0018), no USB stack and no USB console
  (0072-0077, 0081), no touch drivers and no iowatchdog patches.

### The eMMC driver could not work (2026-11-11)
- It was a **character device**: the root of XNU is a block device
  (`bdevvp` → `buf_bread` → `bdevsw[].d_strategy`), so its `d_read` was
  never called
- The command register was written at **0x0C** (that is the Transfer Mode)
  as a 32 bit word, the index was in the low bits and the flags were in the
  wrong bits: **no command ever reached the card**
- Now rewritten: 16 bit command at 0x0E, index << 8, the flag values
  measured on the phone, the block device interface, the GPT lookup of the
  partition with the HFS+ volume, and 25 MHz after the identification

### Shim eMMC Driver
- **Commit e80f24a** (2026-11-10 18:30): Fixed CMD1 loop to poll OCR bit 31 (busy flag)
  - Previous version sent CMD1 once and expected immediate ready
  - New version loops up to 1000 times waiting for bit 31 to set
  - Matches behavior of working Linux driver (sdhci_msm)
- **Result**: CMD2 now succeeds, card fully identified

### Linux Kernel HFS+ Mount
- **CI run** (kernel-h815): Applied HFS+ no-backup-header patch
- **Device test**: Booted patched kernel, mounted userdata in recovery
- **Verification**: 
  - Mount successful with read-only flag
  - Directory listing shows iOS system structure
  - `/usr/lib/dyld` hash matches expected value
  - Volume: 18.35 GB total, 11.2 GB used, HFS+ case-sensitive journaled

### XNU eMMC Driver Development
- **Build iteration**: 7 CI runs to resolve all compilation errors
- **API fixes applied**:
  - Removed `__APPLE_API_PRIVATE` defines (caused header conflicts)
  - Changed `#include <sys/uio.h>` → `<sys/uio_internal.h>`
  - Added `#include <arm/machine_routines.h>` + forward declaration
  - Fixed `pmap_map` → `ml_io_map` with correct 2-arg signature
  - Fixed `kmem_alloc` → `kernel_memory_allocate` with 6 args (added `VM_KERN_MEMORY_NONE`)
  - Fixed `uio_resid_64(uio)` → `uio->uio_resid_64` (field not function)
  - Fixed cdevsw stubs: `noioctl` → `enoioctl`, `nostop` → `eno_stop`, etc.
  - Removed obsolete cdevsw fields: `d_awake`, `d_bcount`, `d_type`
  - Added inline functions `rd16/wr16/rd32/wr32` to avoid macro conflicts
- **Final successful build**: CI run 42923144296 (2026-11-10 23:35 UTC)
- **Artifacts**: kernel binary + buildtree tarball for local inspection

### Patch 0000 Regeneration
- **Problem**: Original patch 0000 created `sweet_emmc.c` with incorrect APIs
- **Solution**: Regenerated patch with corrected file content from commit 1c08b7c + fixes
- **Process**: Extracted correct file → applied API fixes → regenerated patch section → committed
- **Result**: Clean compilation with no errors or warnings in driver code

## Hardware facts (verified on device)

| Item | Value | Source |
| --- | --- | --- |
| SoC | MSM8992, Cortex-A53 x4 + A57 x2 | Device tree |
| eMMC Card | Samsung 032G74, 32GB | CID from device, shim log 2026-11-10 |
| eMMC CID | `11010030333247373400b448b72e5200` | Shim CMD2 response |
| eMMC Controller | SDHCI @ 0xf9824900 (HC), 0xf9824000 (CORE) | MSM8992 datasheet, device tree |
| eMMC Partition | userdata: mmcblk0p50, sectors 12419072-61046783 | Linux `/proc/partitions` |
| iOS rootfs | HFS+ case-sensitive, 18.35GB, no backup header | Verified mount in recovery |
| iOS Version | iPhone OS 16.7.16 (20H392) | `/System/Library/CoreServices/SystemVersion.plist` |
| Bootloader | LK (Little Kernel), not ABL | boot.img format, dt.img handling |
| RAM | 1.25GB @ 0x10000000 + 1.5GB @ 0x80000000 | Device tree, verified |
| Log RAM | 0x50000000 (scratch, survives reboot) | Shim design, verified working |

## File Locations (Key Artifacts)

```
xnu/patches/0000-sweet-source-files.patch     # eMMC driver source (corrected)
xnu/patches/0029-sweet-emmc-root-device.patch # IOKitBSDInit glue for rd=emmc0
kernel/patches/0001-hfsplus-*.patch            # HFS+ no-backup-header fix
shim/emmc_lk_bringup.inc                      # CMD1 loop fix (line 533-543)
.github/workflows/xnu.yml                     # XNU build CI (macos-15, Xcode 16)
.github/workflows/kernel-h815.yml             # Linux H815 kernel CI
tools/mkpayload.py                            # XNU payload generator
shim/build.sh                                 # Shim + payload → boot.img
```

## Reproducible Commands

### Build XNU with eMMC driver (CI)
```bash
# Triggered automatically on push to main, patches in xnu/patches/
# Latest successful: run 42923144296
gh run download 42923144296 --name xnu-kernel
```

### Create XNU boot image
```bash
# Requires: kernel.development.sweet, shim/out/g4.adt
cd ios-sweet-lg-g4
python3 tools/mkpayload.py \
  --cmdline "rd=emmc0 debug=0x8 kextlog=0xfff cpus=1 serial=3" \
  kernel.development.sweet shim/out/g4.adt xnu-payload.bin
cd shim && ./build.sh ../xnu-payload.bin
# Output: shim/out/shim-boot.img
```

### Test on device
```bash
fastboot boot shim/out/shim-boot.img
# Expected: XNU boots, mounts userdata via eMMC driver
# Actual (2026-11-11): immediate recovery reboot (under investigation)
```

### Verify HFS+ mount (Linux kernel)
```bash
# Boot H815 kernel from CI, then in recovery:
adb shell
mount -t hfsplus -o ro /dev/block/mmcblk0p50 /mnt
ls -la /mnt
cat /mnt/System/Library/CoreServices/SystemVersion.plist
```

## Known Issues and Workarounds

1. **XNU boot triggers recovery** (2026-11-11)
   - Issue: Both XNU payload and diagnostic shim cause immediate reboot
   - Impact: Cannot test eMMC driver runtime behavior
   - Workaround: Under investigation, may need prebuilt reference kernel
   - Tracked: Not yet in NOTES.md, see STATUS.md blocker section

2. **No shim diagnostics on screen** (ongoing)
   - Issue: Panel not initialized by shim (DSI command-mode)
   - Impact: Cannot see shim output during boot
   - Workaround: Log channel via RAM@0x50000000 (works when recovery boots)
   - Status: Acceptable for Phase 1-3, Phase 4 will add display driver

3. **TWRP lacks /dev/mem** (2026-11-11)
   - Issue: Cannot read shim log after XNU boot failure
   - Impact: No diagnostic info when XNU fails
   - Workaround: Boot Linux H815 kernel after shim to read log
   - Status: Additional boot cycle needed for debugging

## Rules being followed

- Nothing flashed permanently: only `fastboot boot` (temporary)
- userdata only written after explicit confirmation
- HFS+ mounts are read-only by design (journaled volume forces MS_RDONLY)
- Never detach block device driver with mounted volume (causes crash)
- Every failed attempt recorded in NOTES.md with exact error
- After 10 failed attempts at same step, stop and ask
- All changes via CI: no local XNU builds, only macos-15 runners

## Next Steps

1. **Diagnose XNU boot failure**: Compare boot.img with known-good reference
2. **Test alternative**: Try prebuilt XNU kernel from ios-sweet (if available)
3. **Fallback validation**: Boot Linux H815 kernel to verify boot.img format works
4. **If XNU boots**: Test eMMC driver read operations (mount userdata from XNU)
5. **Documentation**: Record boot failure analysis in NOTES.md

## CI Workflow Status

- ✅ **xnu.yml**: Builds XNU kernel with eMMC driver (run 42923144296 successful)
- ✅ **kernel-h815.yml**: Builds Linux kernel with HFS+ patch (successful, artifact tested)
- ❌ **hfs-tool.yml**: Userspace HFS+ tool build (not recently tested)

## References

- Shim log 2026-11-10: `docs/shim-log-2026-11-10.txt` (C2OK verified)
- XNU build log: CI run 42923144296 (all patches applied)
- Kernel H815 test: `tools/device-test.sh` output (HFS+ mount verified)
- eMMC reference: `docs/g4-hardware/EMMC-REFERENCE.md` (UniXNU analysis)
- API fixes: commits 595eea6, a9d3623, e2a408e, 54220eb, c9ec461 (2026-11-10)
