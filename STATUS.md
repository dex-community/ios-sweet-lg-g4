# STATUS — iOS 16 on LG G4 (H815)

Last updated: 2026-11-10

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
| 1 — shim + minimal boot | **mostly done** | Shim builds, passes the offline boot-image check, runs under QEMU **and on the real phone** (LK loads it, it runs to the end and resets itself). The panel does not show it: the display driver is Phase 4 |
| 2 — XNU kernel and RAM disk | **in progress** | The upstream `ios-sweet` turns out to be a port of XNU to a **Xiaomi phone with a device tree** (IODTPlatformExpert, DT virtual timer, boot CPU without PSCI), i.e. the same shape as this board, so its patch set is the base. The kernel is built **in CI** (macos-14, Xcode 15.x — the only `iig` that matches the xnu-8796 IORPC ABI), not locally. The G4 adaptations are done: `-march=armv8-a+crypto` (the G4 is all Cortex-A53, armv8.0 — armv8.2 lets the compiler emit LSE/complex instructions the A53 does not have) and an `IONameMatch = 'lg,h815'` entry so the platform expert attaches to our ADT. `tools/build-g4-image.sh` chains CI artifact → ADT → payload → LK boot image |
| 3 — eMMC storage and iOS root | **in progress** | La carta **risponde**: e' una Samsung 032G74 (CID `11010030333247373400b448b72e5200`), CMD5/CMD7/CMD0/CMD1 vanno a buon fine e restituiscono l'OCR. L'unico comando che fallisce e' **CMD2 (SEND_ARG)**, con un interrupt di errore (`int=00088000`, non un timeout). Il driver Linux che funziona su questo telefono programma CMD2 come 136 bit + CRC7 **senza** il check dell'indice, mentre lo shim lo programmava con il check dell'indice: e' il sospetto, e il codice ora fa uno sweep delle quattro combinazioni (l'esito dello sweep non e' ancora stato letto: vedi `NOTES.md` FATO 14). Il rootfs iOS su `userdata` e' un volume HFS+ **case-sensitive** ('HX'), journaled e **senza backup volume header**: e' proprio il backup mancante che faceva fallire il mount del driver Linux, e la patch e' scritta (`kernel/patches/0001-hfsplus-mount-a-volume-with-no-backup-volume-header.patch`, applica pulita). Il volume si monta in sola lettura, quindi il rootfs non viene toccato. XNU stesso ha ancora bisogno di un driver eMMC (quello di UniXNU e' il riferimento) | **Sicurezza:** dal 2026-11-09 gli accessi MMIO passano per una allowlist a runtime (`shim/mmio.h`) e un audit statico nella build (`tools/check-mmio-writes.py`): due hang del telefono erano scritture a controller mai inizializzati. Dettagli in `GUIDE-05-MMIO-SAFE.md`.
| 4 — touch, buttons, display | **in progress** | Touch is the Synaptics `s3528` @ I²C 0x20; buttons are PM8994 GPIO 3 + PON1/2. The display is command-mode and needs a real MDP/DSI driver; the NetHunter G4 kernel is the reference |
| 5 — polish and documentation | partial | `GUIDE-00`, `GUIDE-01`, `STATUS.md` |

## What works

- Working copy of `ios-sweet` in `ios-sweet-lg-g4/`, with the sweet UFS
  driver removed.
- `shim/` ported to the G4: hardware constants in `shim/g4.h`, a device-tree
  driven framebuffer lookup, a watchdog stop, and a live dump of `/memory`
  and `/chosen`.
- `shim/build.sh` builds with `aarch64-elf-gcc` (brew) and wraps the shim in
  an **LK-style** boot image (see `GUIDE-01-SHIM.md`).
- `tools/mkbootimg_lg.py` builds that boot image, and
  `tools/verify_bootimg.py` re-parses it exactly the way LK does and checks
  that the H815 device tree is the one that would be selected.
- `tools/qemu-g4.py` boots the shim under QEMU and saves the framebuffer;
  `tools/fb2text.py` decodes such a framebuffer back into text. The QEMU
  run reaches the diagnostics and prints them, which validates the device
  tree parsing, the framebuffer lookup, the console and the payload
  rejection path.
- A **Linux kernel built for this phone** (`.github/workflows/kernel-h815.yml`,
  the Titan 3.10.108 tree) with `CONFIG_DEVMEM` and `CONFIG_HFSPLUS_FS`: it
  boots with `fastboot boot` and gives `/dev/mem`, which is how the shim's
  log is read back from the RAM (`tools/read-shim-log.py`).
- The shim's log chain works end to end: `fastboot boot` the shim, the
  phone resets itself into the recovery, then `fastboot boot` the kernel
  with `/dev/mem` and read `0x50000000`. The log of the run of 2026-11-10
  is in `docs/shim-log-2026-11-10.txt`.

## What does not work yet

- The phone **is** running our shim (verified by the reset timing), but the
  panel shows the LG logo: the G4's panel is in DSI command mode and the
  bootloader re-programmed the controller, so the cont_splash window is not
  scanned out any more. A real display driver is needed (Phase 4).
- A **log channel** is in place that does not need the screen: the shim
  copies its ASCII console log to scratch RAM at 0x50000000 before the
  reset, and a root recovery reads it back with `/dev/mem`
  (`tools/pull-mem.sh`).
- The XNU kernel has not been built for `msm8992` yet, and the ADT has not
  been generated for the G4.
- No eMMC driver, no touch driver, no button driver, no display driver.
- The iOS rootfs is **on the phone** (HFS+ case-sensitive on `userdata`,
  18.35 GB of volume) but the stock kernel cannot mount it: the volume has
  no backup volume header and the driver refuses it. The patch that accepts
  it is written and the CI that applies it is ready, but the kernel with
  the patch **has not been built yet**, so the rootfs has not been opened.
- The shim's CMD2 (SEND_ARG) still fails on the phone. The sweep of the
  four response-type combinations is compiled and running on the phone, but
  its log has not been read back yet (`NOTES.md` FATTO 14).

## Hardware facts (all from the phone's own device tree)

| Item | Value | Source |
| --- | --- | --- |
| SoC | MSM8992, Cortex-A53 x4 + A57 x2 | `msm8992-lg-h815-boot.dts` |
| Board id | `0xb64` (`qcom,board-id`), `qcom,msm-id = <0xfb 0x00 0xfc 0x00>` | DTS root |
| Bootloader | **LK**, not ABL | header layout, `dt.img` handling |
| Panel | 1440x2560, `qcom,mdss-dsi-bpp = 0x18` (24 bpp), **command mode**, dual DSI | DTS:1309, 1329 |
| Framebuffer window | `cont_splash_mem` 0x03400000 + 0xc00000 (12 MiB) | DTS:13337 |
| Touch | Synaptics `s3528` I2C 0x20 **or** LG SIC `lg2411` I2C 0x28, bus `i2c@f9924000`, IRQ 96 | DTS:5514, 5667 |
| Buttons | volume-up = PM8994 GPIO 3; volume-down = PMIC PON2; power = PMIC PON1 | DTS:13207, 4035 |
| PMIC | PM8994 + PMI8994 over SPMI at 0xfc4c0000, IRQ 190 | DTS:3989 |
| eMMC | `sdhci@f9824900` (sdhc1/mmc0), non-removable, 8-bit, IRQ 123/138 | DTS:6392 |
| GIC | distributor 0xf9000000, redistributor 0xf9002000 | DTS:3484 |
| Timer | `arm,armv8-timer`, 19.2 MHz, PPI 2/3/4/1 | DTS:3497 |
| RAM | `10000000-5fffffff` (1.25 GB) + `80000000-dfffffff` (1.5 GB), hole at `60000000-7fffffff` | `/proc/iomem` on the phone |
| WDT | `qcom,wdt@f9017000` | DTS:3721 |
| PSCI | **absent** — AOSP's h815 port says "psci is broken" | `docs/g4-hardware/HARDWARE-G4.md` |

## The three open questions that block the next phases

0. **The RAM map is now known:** `PHYS_BASE = 0x80000000`, `MEM_SIZE =
   0x60000000` (the 1.5 GB upper bank, above every XPU carve-out).
   The ADT generator has a G4 profile (`tools/mkadt.py --device g4`).
1. **Page size.** The sweet kernel and XNU build assume a 16 KiB granule
   (SM8150 is ARMv8.2). The G4 is ARMv8.0. Cortex-A53/A57 implement the
   Large Physical Address extension, so 16 KiB *should* be available, but
   the G4's own Android kernel runs 4 KiB. The shim prints
   `ID_AA64MMFR0_EL1.TGran1` at boot; if it is 0, XNU has to be rebuilt for
   4 KiB.
2. **The RAM map.** The stored device tree has `memory reg = <0 0 0 0>`; LK
   fills it in at boot. The shim now dumps `/memory` (and its carve-outs)
   from the live tree, so the answer comes from the first device boot. Until
   then the ADT's `physBase`/`memSize` are unknown.
3. **PSCI.** The CPU nodes use `enable-method = "qcom,8994-arm-cortex-acc"`
   (Qualcomm's SCF/ACC path), not PSCI `CPU_ON`. Single-core XNU is fine,
   but SMP needs either a working PSCI or an XNU patch that uses the SCF
   handover. The shim probes PSCI at boot and prints the version.

## Reproducible guides

| Guide | What it covers |
| --- | --- |
| `GUIDE-00-SETUP.md` | Repository layout, toolchain, where the assets come from, how to rebuild everything |
| `GUIDE-01-SHIM.md` | The LK boot-image format, the G4 hardware constants, building, verifying, flashing |
| `GUIDE-02-TELETEST.md` | What the phone says about itself, writing a partition, and reading the log without a screen |
| `NOTES.md` (sezione G4) | The engineering log: every measurement, every failed attempt, every dead end |
| `docs/g4-hardware/HARDWARE-G4.md` | Every device-tree fact, with file:line references |
| `docs/g4-hardware/EMMC-REFERENCE.md` | How UniXNU's eMMC driver works and what has to change for the G4 |

## Rules being followed

- Nothing is flashed permanently: only `fastboot boot` (temporary).
- `userdata` is only written after an explicit confirmation. The HFS+ mount
  is read-only by construction (the driver forces `MS_RDONLY` on a
  journaled/locked volume), so the rootfs cannot be touched by accident.
- **Never detach a driver of a block device that has a mounted volume**:
  doing that (sdhci_msm with `mmcblk0p49` on `/cache`) crashed the phone
  and left its USB stack wedged for a full battery pull (`NOTES.md` FATTO 16).
- Every failed attempt is recorded with its exact error in `NOTES.md`.
- After 10 failed attempts at the same step, stop and ask.
