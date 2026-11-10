# ios-sweet-lg-g4

**Apple's iOS 16 userland on an LG G4 (H815).**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the LG G4 H815 (`msm8992`, Qualcomm Snapdragon 808: 4× Cortex-A53 + 2× Cortex-A57). The target userland is the **unmodified iOS 16.7.16 (20H392) root filesystem of the iPhone X**, already written to the phone's `userdata` partition.

It starts from [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) (XNU on a Xiaomi `sweet`, SM7150), uses [`shrimpapplepro/UniXNU-lumia950xl`](https://github.com/shrimpapplepro/UniXNU-lumia950xl) as the eMMC reference (same MSM8992 SDHCI controller), and [`ios-topaz`](https://github.com/turgayasuk-sketch/ios-topaz) as the display bring-up reference.

> **Status: experimental bring-up, not a usable phone OS.** XNU reaches its entry point on the real phone and the iOS root is on disk, but iOS does not boot yet: the eMMC is not yet readable from the shim/XNU (the card answers, one command does not), and there is no working display driver. As of 2026-11-10 the phone is fully recoverable: every test is a `fastboot boot`, nothing is written.

## Where the project stands

| Area | State |
| --- | --- |
| Shim | Builds with `aarch64-elf-gcc`, wraps into an LK-style boot image, runs on the real phone |
| Boot chain | LK → flashed `boot` partition → shim → XNU. **This LK does accept `fastboot boot`**: every test is a temporary boot, nothing is written on the phone |
| XNU entry | Reached on hardware: 11 Mach-O segments loaded, ADT + boot-args prepared, jump to `__start` at PA `0x8147B488` |
| Kernel CI | `.github/workflows/xnu.yml` builds the patched xnu-8796 kernel on macOS runners |
| Rootfs | iOS 16.7.16 iPhone X HFS+ (case-sensitive 'HX', journaled) written to `userdata` (`mmcblk0p50`): 18.35 GB of volume, 4096 byte blocks. **No backup volume header**, which is why no stock kernel can mount it; the patch that accepts it is written (`kernel/patches/`) |
| eMMC from shim | The card **answers**: Samsung 032G74, CMD5/CMD7/CMD0/CMD1 all work and return the OCR. The only failing command is **CMD2 (SEND_ARG)**, with an error interrupt; the Linux driver that works on this phone programs it without the index check, and the shim now sweeps the four possible response types |
| RAM disk | A minimal `sweet-init` Mach-O PID 1 with heartbeat, packed into the payload |
| Fallback | Shim arms a 60 s watchdog and writes `LK_RECOVERY_MODE`, so the phone returns to TWRP without the damaged buttons |
| Display | LG panel is JDI NT36850, 1440×2560, **command mode**, dual MIPI DSI. The active pipe is **RGB2/INTF2** (MDP3), not VIG0 |
| Input | Touch is either Synaptics `s3528` or LG SIC `lg2411` (same GPIOs, not yet probed); only Volume Up is reliable |
| SMP | PSCI is absent on this board (Qualcomm SCF/ACC path), so XNU runs single-core for now |
| USB, Wi-Fi, audio, camera, GPU | Not started |

## Boot flow

```
LK (stock LG bootloader, unlocked)
  -> boot partition: shim (LK boot image v1, QCDT v3 for board 0xb64)
     -> shim sets up its own MMU, parses the live device tree,
        loads and decompresses the payload from the ramdisk
        -> XNU Mach-O + generated Apple Device Tree + boot-args
           -> XNU entry -> future: eMMC driver -> HFS+ root on userdata
              -> launchd -> system daemons -> SpringBoard
```

The shim draws diagnostics into the framebuffer and writes a log to the phone's ramoops/pstore zone. **The pstore is one-shot**: the next Linux kernel overwrites it. A secondary trail lives in IMEM at `0xfe80f000`, but reading it needs `/dev/mem`.

## Current blocker and next step

**The eMMC is one command away.** The card is a Samsung 032G74 and it
answers: the wake-up, the select, the idle and the `SEND_OP_COND` (which
returns the OCR with the power-up bit) all work. The only command that
fails is **CMD2 (SEND_ARG)**, and it fails with an *error interrupt*, not
with a timeout: the controller says the answer is wrong.

The driver Linux that works on this phone sends CMD2 as a 136 bit answer
with the CRC7 check and **without** the index check; the shim sent it with
the index check as well. That is the only difference, and the shim now
tries the four combinations one after the other and writes the outcome in
the log. Reading that log is the next measurement, and it costs nothing:
the phone is in fastboot, `fastboot boot` the shim, let it reset itself
into the recovery, then `fastboot boot` the kernel with `/dev/mem` and
read `0x50000000` (`tools/read-shim-log.py`).

**The iOS rootfs is readable in principle, but not yet in practice.** The
volume on `userdata` is HFS+ case-sensitive, journaled and **without a
backup volume header**; the Linux driver refuses a volume whose backup is
missing (`hfsplus: invalid secondary volume header`), and that is the one
thing that stops the mount. The patch is written and applies cleanly; the
CI builds the kernel with it (`.github/workflows/kernel-h815.yml`). The
driver forces the mount read-only on a journaled volume, so the rootfs
cannot be damaged by it.

**Then the display.** The panel is driven by the **RGB2 pipe on INTF2**
(MDP3), not by VIG0, so the `ios-topaz` patch that re-points VIG0 does not
apply here: the MDP has to be clocked and the RGB2 scanout re-armed. The
safe place to measure all of this is the recovery, through
`/sys/kernel/debug/mdp/`, never with a probe in the shim.

## Safety rules

- **Never write to an uninitialised controller.** Every MMIO access goes through `mmio_rd32`/`mmio_wr32` (`shim/mmio.h`) and the runtime allowlist. An allowlist entry does not prove the clock is on: three hangs came from probing blocks whose clock was off. See `GUIDE-05-MMIO-SAFE.md`.
- **Do not write on the phone.** Every test is a temporary `fastboot boot`: nothing is flashed, the `boot` partition is not touched. The old flash-and-restore path (`tools/run-t2.sh`, `backup/boot-stock.img`, LAF restore with KILO authentication) is kept only for the case where the phone has to be recovered.
- **Never touch `misc`, `cache`, `aboot`, `sbl1`, `tz`, `rpm`, `hyp`, `persist`, `modem`.** The eMMC driver only allows the two `spare` sectors.
- **Do not rewrite `userdata`.** The iOS HFS+ volume is already written and verified; the goal is to read it. The driver mounts it read-only by itself (a journaled volume is forced to `MS_RDONLY`), and the rootfs must stay byte-identical.
- **Never detach a driver of a block device that has a mounted volume.** Doing it with the eMMC (`mmcblk0p49` on `/cache`) crashed the phone and left its USB stack wedged until the battery was pulled. Measure with tracepoints instead.
- Keep partition dumps, serial numbers, recovery images, Apple firmware and downloaded build trees out of git.

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm), eMMC/SD bring-up, loader, pstore, watchdog, MMIO allowlist |
| `xnu/patches/` | XNU patch series (applied in order by CI) |
| `xnu/out-*/` | CI build outputs used locally |
| `.github/workflows/xnu.yml` | XNU kernel CI (macOS) |
| `.github/workflows/kernel-h815.yml` | Linux H815 kernel CI with DEVMEM + HFS+ (for diagnostics), applies `kernel/patches/` |
| `kernel/patches/` | Patches for the Linux kernel of the phone (the HFS+ one first) |
| `tools/mkadt.py` | Generates the Apple Device Tree from the G4 layout and iPhone X identity |
| `tools/mkpayload.py`, `tools/mkinit.py` | Pack kernel + ADT + ramdisk into the shim payload |
| `tools/build-g4-image.sh` | Chains CI artifact → ADT → payload → LK boot image |
| `tools/mkbootimg_lg.py`, `tools/verify_bootimg.py` | Build and re-parse the LK boot image; checks the H815 device tree selection |
| `tools/run-t2.sh` | Flash → measure → pull pstore → restore stock boot |
| `tools/pack-twrp-kernel.sh` | Repack TWRP ramdisk with a new Linux kernel |
| `tools/lglaf/` | LAF / Download Mode tool with KILO authentication |
| `tools/check-mmio-writes.py` | Static audit: no raw store may bypass the MMIO allowlist |
| `tools/qemu-g4.py`, `qemu-emmc.py`, `qemu-mmio-guard.py` | Offline QEMU tests for framebuffer, eMMC model and MMIO guard |
| `tools/fetch-pstore.sh`, `read-pstore.py`, `read-trail-imem.py` | Log retrieval |
| `tools/read-shim-log.py`, `boot-test-kernel.sh`, `check-kernel-hfs.sh` | The shim → RAM log cycle and the checks on the diagnostic kernel |
| `tools/sweet-root-tweaks.py`, `mkiosroot.sh` | iOS root image preparation |
| `backup/` | Partition backups (not in git) |
| `docs/`, `GUIDE-*.md`, `STATUS.md`, `NOTES.md` | Hardware facts, procedures and full engineering log |

## Guides

| Document | What it covers |
| --- | --- |
| `GUIDE-00-SETUP.md` | Repository layout, toolchain, assets, rebuilding everything |
| `GUIDE-01-SHIM.md` | LK boot-image format, G4 constants, build, verify, flash |
| `GUIDE-02-TELETEST.md` | What the phone reports, writing a partition, reading logs without a screen |
| `GUIDE-03-EMMC-DEBUG.md` | eMMC/SDCC bring-up measurements and dead ends |
| `GUIDE-04-PSTORE.md` | Reading the shim log from pstore |
| `GUIDE-05-MMIO-SAFE.md` | The allowlist rule and how to prove a block is clocked |
| `GUIDE-06-SD-MISURE.md` | microSD measurements (SDCC2 works; the inserted card is read-only) |
| `docs/PLAN-2026-11-10.md` | Current action plan: eMMC, display, input |
| `docs/g4-hardware/HARDWARE-G4.md` | Every device-tree fact with file:line references |
| `docs/g4-hardware/EMMC-REFERENCE.md` | UniXNU eMMC driver and what to change for the G4 |
| `docs/QEMU_TESTING.md` | Offline QEMU test setup |
| `NOTES.md` | Engineering log: every measurement, boot, crash and decision |
| `STATUS.md` | Phase status and open questions |

## Hardware facts

All values below come from the phone's own device tree and from measurements on the device.

- SoC: MSM8992, 4× Cortex-A53 + 2× Cortex-A57, ARMv8.0, no pointer authentication.
- Bootloader: **LK** (not ABL), board id `0xb64`, `qcom,msm-id = <0xfb 0x00 0xfc 0x00>`.
- RAM: `0x10000000–0x5fffffff` (1.25 GB) + `0x80000000–0xdfffffff` (1.5 GB), hole at `0x60000000–0x7fffffff`. The shim/XNU payload uses the upper bank, `PHYS_BASE = 0x80000000`.
- Framebuffer: `cont_splash_mem` at `0x03400000` (12 MiB) is XPU-protected for EL1; `G4_FB_BASE` moved to `0x20000000`.
- Panel: JDI NT36850, 1440×2560, 24 bpp, command mode, dual DSI, 4 lanes, 910 MHz, 60 fps.
- Display controller: MDP3, RGB2 at `0xfd919000`, CTL0 at `0xfd902000`, INTF2 at `0xfd96c000`, DSI0 at `0xfd994000`.
- eMMC: `sdhci@f9824900`, non-removable, 8-bit, HS400, IRQ 123/138. `userdata` is `mmcblk0p50`, 23.2 GiB.
- Touch: Synaptics `s3528` @ I²C `0x20` or LG SIC `lg2411` @ `0x28`, bus `i2c@f9924000`, IRQ 96.
- Buttons: Volume Up = PM8994 GPIO 3; Power = PON1; Volume Down = PON2. Power and Volume Down are physically unreliable; Volume Up is the recovery key.
- PMIC: PM8994 + PMI8994 over SPMI at `0xfc4c0000`, IRQ 190.
- GICv3 at `0xf9000000`, ARMv8 timer at 19.2 MHz. PSCI is absent; the CPU enable method is `qcom,8994-arm-cortex-acc`.
- Watchdog: `qcom,wdt@f9017000`.

## Credits

Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [ios-sweet](https://github.com/dipa-studio/ios-sweet), [UniXNU-lumia950xl](https://github.com/shrimpapplepro/UniXNU-lumia950xl), [ios-topaz](https://github.com/turgayasuk-sketch/ios-topaz), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), the postmarketOS MSM8992 DTS, Qualcomm/LG's published kernel sources for the hardware details, and TeamWin/OrangeFox recovery. iOS itself is Apple's and is not distributed here.
