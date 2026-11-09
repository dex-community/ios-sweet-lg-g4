# STATUS — iOS 16 on LG G4 (H815)

Last updated: 2026-11-05

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
| 3 — eMMC storage and iOS root | **in progress** | the controller is healthy and the bus is powered (CORE+0xdc = BUS_ON after HC+0x29 = 0x0f) but the card is silent: CMD5/CMD1 end in a *command timeout* with a zero response. Measured over 8 bring-up variants in 3 runs (3.3 V / 1.8 V, with and without the LK's PWRCTL ack, with and without a 10 s delay, with and without HC+0x29, with the LK's own registers). The shim now leaves its log in the phone's ramoops zone, so the controller registers are readable after a run (`tools/fetch-pstore.sh`, GUIDE-04-PSTORE.md). Leading hypothesis: the eMMC VCC regulator (pm8994_l20, `vdd-supply`) is not powered by the shim, which has no regulator driver (H7). XNU itself still needs an eMMC driver (the sweet port reads the root from UFS): the UniXNU driver is the reference | **Sicurezza:** dal 2026-11-09 gli accessi MMIO passano per una allowlist a runtime (`shim/mmio.h`) e un audit statico nella build (`tools/check-mmio-writes.py`): due hang del telefono erano scritture a controller mai inizializzati. Dettagli in `GUIDE-05-MMIO-SAFE.md`.
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
- The iOS root image has not been built (the IPSW is on disk, so this is
  unblocked as soon as a Linux box or `hfsprogs` is available).

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
- `userdata` is only written after an explicit confirmation.
- Every failed attempt is recorded with its exact error in `NOTES.md`.
- After 10 failed attempts at the same step, stop and ask.
