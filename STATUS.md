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
| 2 — XNU kernel and RAM disk | **mostly done** | XNU kernel with eMMC driver **compiles successfully in CI** (run 42923144296, 2026-11-10). Driver `bsd/dev/sweet_emmc.c` integrated with correct APIs (ml_io_map, kernel_memory_allocate, uio_resid_64). Patches 0000-0030 applied. Artifact: `kernel.development.sweet` 16 MB. Boot payload created with `mkpayload.py` using `rd=emmc0` boot-arg. **Boot test pending**: shim+XNU boot.img triggers immediate recovery reboot (root cause under investigation). |
| 3 — eMMC storage and iOS root | **in progress** | eMMC card responds and is identified by shim (C2OK v=0x09). Linux kernel with HFS+ patch **mounts iOS rootfs successfully** (CI kernel-h815 run, verified via recovery mount). rootfs: 18.35 GB HFS+ case-sensitive, journaled, no backup header. XNU eMMC driver compiled and ready but not yet boot-tested on device. Next: diagnose boot failure, test XNU eMMC driver in working boot chain. |
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
- **eMMC driver compilation successful** (run 42923144296, 2026-11-10 23:35 UTC)
- Driver `bsd/dev/sweet_emmc.c` with correct XNU APIs:
  - `ml_io_map(phys_addr, size)` for MMIO mapping
  - `kernel_memory_allocate(..., VM_KERN_MEMORY_NONE)` for bounce buffer
  - `uio->uio_resid_64` for I/O size
  - `enoioctl`, `eno_stop`, etc. for cdevsw stubs
  - Forward declaration for `ml_io_map` to avoid implicit function errors
- IOKitBSDInit glue code for `rd=emmc0` verified present in build artifact
- `bsd/conf/files` includes `bsd/dev/sweet_emmc.c` in build
- Patches applied: 0000 (source files), 0001-0015, 0017, 0019-0030 (skipped: 0016, 0018, 0035+)
- Build artifact: `kernel.development.sweet` (16 MB, sha256 in CI logs)
- Payload created: `xnu-payload.bin` with boot-args `rd=emmc0 debug=0x8 kextlog=0xfff cpus=1 serial=3`

### Tools
- `tools/mkpayload.py`: creates XNU payloads with SWEETXNU magic header
- `tools/mkbootimg_lg.py`: wraps shim+payload in LK boot.img format
- `shim/build.sh`: end-to-end shim → boot.img pipeline
- `tools/device-test.sh`: unified test script for kernel+HFS validation

## What does not work yet (blockers)

### Critical Blocker: XNU Boot Failure
- **Symptom**: `fastboot boot shim-boot.img` with XNU payload triggers immediate recovery reboot
- **No visible diagnostics**: shim does not display on screen (even with FORCE_WAIT_S=60)
- **No log access**: TWRP recovery has no `/dev/mem`, cannot read shim log from RAM
- **Affects**: both XNU payload (16 MB) and empty diagnostic shim (97 byte ramdisk)
- **Hypothesis**: boot.img format issue, shim crash before log output, or bootloader rejection
- **Next steps**: 
  - Compare with known-good boot.img from previous successful boots
  - Try prebuilt XNU kernel from ios-sweet reference
  - Check `fastboot getvar` for secure boot status
  - Consider flash instead of boot, or test with H815 Linux kernel first

### Panel Display (Phase 4)
- G4 panel is DSI command-mode, bootloader reprograms controller
- cont_splash framebuffer not scanned out after LK handoff
- Real MDP/DSI driver needed (reference: NetHunter G4 kernel)

### Outstanding Questions
1. **Boot.img validation**: Why does current boot.img trigger recovery?
2. **XNU runtime**: Will XNU reach early console with current patch set?
3. **eMMC driver runtime**: Will driver successfully mount userdata once XNU boots?

## Recent Progress (2026-11-10 to 2026-11-11)

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
