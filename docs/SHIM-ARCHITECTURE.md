# The architecture of the shim (after the refactoring of 2026-11-15)

> **Da leggere insieme a `docs/INDEX.md`.** La struttura dei moduli e i due
> hang (timebase, salto a XNU) sono correnti. La sezione "The display test"
> descrive una versione precedente di `display.c`: gli stage 1..5 allora non
> erano collegati e HELLO non veniva disegnato. Stato reale del test:
> `docs/DISPLAY-TEST.md`.

## The modules

```
shim/
  start.S        the entry: the stack, the vectors, the image base
  timer.h        THE TIMEBASE: the counters are switched on here
  mmio.h         the guarded accessors (the allowlist of the windows)
  clocks.c/.h    the clocks of the SoC: the ONLY place that switches one on
  console.c/.h   the log, the ASCII copy and the text on the framebuffer
  log.h          the three places where the log has to be written
  trail.h        the breadcrumb ring in the IMEM
  pstore.c/.h    the ramoops console zone
  wdt.c/.h       the watchdog guard
  fdt.c/.h       the device tree: the RAM map and /chosen
  mmu.c/.h       the MMU of the loader
  emmc.c/.h      the eMMC: the clocks, the host, the card
  emmc_lk_bringup.inc  the replay of the sequence of the factory LK
  emmc_diag.inc  the sweep of the variants (the diagnostic build)
  emmc_model.inc the software model of the controller (the QEMU build)
  emmc_probe.c  the read-only probes (E0 and D0)
  sd.c/.h        the microSD slot
  sd_probe.c/.inc the read-only probes of the microSD
  sd_payload.c  the payload of XNU read from the card
  display.c/.h   the panel: the clocks, the DSI, the MDP, the picture
  loader.c/.h    the jump to XNU
  pstore.c/.h    the ramoops
  crc32.c/.h     the CRC of the pstore record
  rs8.c/.h       the run-time base
  lib.c          memset and memcpy
```

The rule of the project is now in ONE place: `clocks.c` is the only module
that switches a clock on, and every driver asks it before it touches a
controller.

## The order of the run

```
shim_main()
  timer_init()            the counters, before any wait
  trail_init()            the breadcrumb ring
  find_framebuffer()      the device tree
  pstore_read_previous()  the log of the run before this one
  stop_watchdog()         the watchdog of the bootloader
  [the eMMC phase]        clocks -> host -> card       (skipped in the test)
  [the microSD phase]     clocks -> controller
  pstore_console_write()  the log is safe
  [the display test]      clocks -> DSI -> MDP -> picture -> stop
  [the loader]            the jump to XNU
  the diagnostics         the MMU, the GIC, the device tree
  reset(15)               the phone comes back
```

## The two hangs, and what they were

### 1. The hang of the eMMC phase (2026-11-15)

The pstore record of the run stopped between `emmc_write_log()` and
`pstore_console_write()`: the phone did not crash, it sat there until the
60 s watchdog rebooted it (the `fastboot boot` came back after 58.5 s).

The cause was the timebase, not the controller:

* `emmc.c` measured its timeouts with the VIRTUAL timer (`cntvct_el0`);
* the EL0 counters are not running at the shim's entry: nothing in the
  boot path (`start.S`) programs `CNTKCTL_EL1`, `CNTV_CTL_EL0` or
  `CNTP_CTL_EL0`, and the LK leaves them as it found them;
* a frozen counter reads a constant, so `emmc_deadline()` returned a
  constant and every wait loop (`emmc_wait`, `emmc_wait_idle`,
  `emmc_wait_busy`, `emmc_delay_us`) waited for ever.

The fix has two halves, and both are needed:

* `timer_init()` switches the counters on at the entry (`shim/timer.h`);
* every wait is bounded by that timebase and pets the watchdog, so a wait
  that cannot end becomes a reset instead of a hang.

The timebase is the PHYSICAL timer (`CNTPCT_EL0`): the virtual one at EL0
belongs to a hypervisor, and this phone has none.

### 2. The silent crash at the jump to XNU

The historical crash: the shim armed the watchdog for the jump, the phone
rebooted, and there was no record of what the kernel had found.

Two things were added:

* `loader.c` writes the log (the RAM copy and the ramoops) RIGHT BEFORE
  the jump, with the entry point and the address of the boot arguments in
  it. A kernel that dies at its first instruction leaves no trace of its
  own: ours is the last one there is.
* the exception vectors of the shim are still installed at that point. If
  the kernel faults before it installs its own, the panic handler of the
  shim catches it and writes the ESR, the ELR and the FAR to the same two
  places.

## The display test

`SHIM_DISPLAY_TEST=1` changes what the shim does: it does NOT jump to
XNU. It brings the display up, paints "HELLO" and stops.

```
the MMSS clocks (clocks.c)      the rule: the clock first
the DSI PHY PLL (the 20nm sequence of the clock driver of the phone)
the DSI PHY and the DSI controller (command mode, 4 lanes)
the MDP (CTL, the RGB2 pipe, INTF2 in command mode)
the picture (the buffer at 0x20000000, NOT the splash window)
the CTL flush, then the DCS (sleep out 0x11, display on 0x29)
the safe loop: the watchdog is armed, so the phone comes back in the
recovery in a minute
```

The stage is chosen at build time (`DISPLAY_STAGE=1..5`), so that a hang
can be bisected: every step writes the log into the ramoops before it
does anything risky, so the record of a run that hangs says where it
stopped.

The eMMC phase is skipped in the test (`SHIM_SKIP_EMMC`, on by default
when `SHIM_DISPLAY_TEST=1`): the test does not need the card, and the card
is where the hang of 2026-11-15 was.

## The registers of the display

Everything comes from the phone itself:

* the addresses: the device tree of the phone
  ("../lg g4/msm8992-lg-h815-boot.dts");
* the sequences: the kernel of the phone
  ("../lg g4/G4-Titan-Kernel-titan_bpf2", the msm-3.18 MDSS stack);
* the panel's DCS: the panel's own device tree node
  ("dsi-panel-jdi-nt36850-dualmipi0-qhd-cmd.dtsi");
* the values that the phone had while it was driving the panel:
  "docs/display-2026-11-10.txt".

The map of the DSI controller on this phone is the map of the tree plus
four bytes (measured on the live dump, six registers confirm it): the
default is the phone's map (`DISP_DSI_MAP_SHIFT=4`).

## What is still open

* the base of the DSI PHY (`DISP_DSI_PHY_BASE`, the default is the one
  the device tree supports);
* the tear of the panel (the shim does not use it: the TE comes on a GPIO
  that the shim does not route, and a trigger that never arrives is a
  panel that stays dark);
* the full sequence of the panel: the shim sends only the sleep out and
  the display on, because the bootloader has already initialised it;
* the buffer at 0x20000000: it is plain RAM and the address that the
  loader already hands to XNU as the video frame buffer, but the real RAM
  map is the one of the device tree.
