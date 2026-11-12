# GUIDE-01 — The G4 shim and the LK boot image

How the sweet shim was ported to the LG G4, and why the boot image had to
be rebuilt from scratch.

## 1. The problem: sweet was written for ABL, the G4 has LK

Upstream `ios-sweet` runs on the Redmi Note 10 Pro, whose bootloader is
Qualcomm **ABL**. ABL takes a boot image with a *raw* device tree in the
boot header:

```sh
mkbootimg --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
          --dtb ../dtb/boot_unpacked/dtb --base 0 --kernel_offset 0x8000 ...
```

The G4's bootloader is **LK** (Little Kernel). LK does not understand that
layout. It reads the device tree from a **QCDT v3 table** appended to the
boot image, exactly like LG's own boot images.

## 2. What LK does with a boot image

Read from the public LK sources (`app/aboot/aboot.c`,
`app/aboot/bootimg.h`, `platform/msm_shared/dev_tree.c`):

```
page_size      = hdr->page_size                      # offset 36
kernel_actual  = round_up(hdr->kernel_size,  page_size)
ramdisk_actual = round_up(hdr->ramdisk_size, page_size)
second_actual  = round_up(hdr->second_size,  page_size)

dt_table = image + page_size + kernel_actual + ramdisk_actual + second_actual
table->magic  == "QCDT"          # 0x54444351
table->version == 3
entries: 40 bytes each -> platform_id, variant_id, board_hw_subtype,
                           soc_rev, pmic_rev[4], offset, size
```

Then LK picks the entry that matches the hardware, copies that DTB to
`hdr->tags_addr`, patches `/memory` and `/chosen` into it, and jumps to the
kernel.

### 2.1 LK's `struct boot_img_hdr` is not the AOSP one

LK's header (`app/aboot/bootimg.h`) has **`dt_size` at offset 40**, where
AOSP v0 has `unused`:

```c
struct boot_img_hdr {
    unsigned char magic[8];       /*  0 */
    unsigned kernel_size;         /*  8 */
    unsigned kernel_addr;         /* 12 */
    unsigned ramdisk_size;        /* 16 */
    unsigned ramdisk_addr;        /* 20 */
    unsigned second_size;         /* 24 */
    unsigned second_addr;         /* 28 */
    unsigned tags_addr;           /* 32 */
    unsigned page_size;           /* 36 */
    unsigned dt_size;             /* 40  <-- LK puts the dt.img size here */
    unsigned unused;              /* 44 */
    unsigned char name[16];       /* 48 */
    unsigned char cmdline[512];   /* 64 */
    unsigned char id[8];          /* 576 */
};
```

The G4's own stock boot image confirms it: byte 40 of `aicp/boot.img` is
`0x0012d000` = 1232896, which is exactly the size of the QCDT table
appended after the ramdisk.

### 2.2 The hand-off to the kernel is the same as on ABL

LK jumps to the kernel through `scm_elexec_call()`
(`platform/msm_shared/scm.c`):

```c
param.el1_x0 = dtb_offset;      /* the DTB pointer */
param.el1_elr = kernel_entry;
```

So **x0 holds the DTB pointer**, exactly like ABL. `shim/start.S` needed no
change for that. (The V60 port, which is also ABL, needed no change either
— the sweet shim was already right about this.)

### 2.3 The addresses: a trap

`copy_dtb()` copies the selected DTB to `tags_addr`. The H815's entry is
**307200 bytes** long, so with LG's stock `tags_addr` of `0x100` the copy
lands on top of a kernel at `0x8000` — 274 KiB of the kernel would be
overwritten.

LG's own stock boot image has exactly that overlap and still boots, so its
LK must override the header addresses (the `msm8996` LK project does
exactly that with `ABOOT_IGNORE_BOOT_HEADER_ADDRS` and
`ABOOT_FORCE_KERNEL64_ADDR`/`ABOOT_FORCE_TAGS_ADDR`).

We cannot know which of the two the G4's LK does, so
`tools/mkbootimg_lg.py` picks an arrangement that is correct either way:

```
kernel   0x00008000 .. 0x0003f000
dtb      0x00400000 .. 0x0044b100     <- tags_addr
ramdisk  0x01000000 ..
```

No two regions overlap, and everything is below any plausible `MEMBASE`
(LK's own base), which is what `check_aboot_addr_range_overlap()` wants.

## 3. The device tree table

`dtb/h815.dt.img` is LG's own QCDT v3 table, reused verbatim. It holds ten
entries pointing at four distinct device trees:

| entry | platform | variant | dtb offset | board |
| --- | --- | --- | --- | --- |
| 0 | 0xfb | 0x364 | 0x1000 | |
| 1 | 0xfb | 0x464 | 0x1000 | |
| 2 | 0xfb | 0x564 | 0x4c000 | |
| 3 | 0xfb | 0x664 | 0x97000 | |
| **4** | **0xfb** | **0xb64** | **0xe2000** | **H815** |
| 5 | 0xfc | 0x364 | 0x1000 | |
| 6 | 0xfc | 0x464 | 0x1000 | |
| 7 | 0xfc | 0x564 | 0x4c000 | |
| 8 | 0xfc | 0x664 | 0x97000 | |
| **9** | **0xfc** | **0xb64** | **0xe2000** | **H815** |

`0xb64` is the H815's `qcom,board-id`; both entries that carry it point at
the same DTB, so the selection cannot go wrong. That DTB is byte-identical
to `dtb/boot.dtb`:

```
sha1(msm8992-lg-h815-boot.dtb, padded to 307200) = 6985b6f2
sha1(QCDT entry 0xe2000, 307200 bytes)         = 6985b6f2
```

`tools/mkbootimg_lg.py` can also build a one-entry table from a single DTB
(`--dtb` instead of `--dt-img`), but reusing LG's table is safer: LK's
matching logic was written for it.

## 4. The hardware constants that changed

Every one of these is in `shim/g4.h`, with the device-tree line it came
from.

| Constant | sweet (SM8150) | G4 (MSM8992) |
| --- | --- | --- |
| GICD | 0x17a00000 | **0xf9000000** |
| GICR | 0x17a60000 | **0xf9002000** |
| framebuffer | 0x9c000000, 1080x2400 | **0x03400000, 1440x2560** |
| IMEM restart reason | 0x146aa65c | **0xfe80f65c** |
| PSHOLD | 0x0c264000 | **0xfc4ab000** |
| watchdog | — | **0xf9017000** |
| virtual timer INTID | 19 | 19 (same) |
| storage | UFS at 0x01d84000 | **eMMC at 0xf9824900** |

## 5. The framebuffer

The G4 has **no `simple-framebuffer` node**. The panel is driven by the
MDP in **command mode** (`qcom,mdss-dsi-panel-type = "dsi_cmd_mode"`),
and the bootloader leaves a `cont_splash_mem` window at 0x03400000
(12 MiB) that the display controller keeps scanning out.

That is why:

- the shim looks for the framebuffer in the device tree at runtime
  (`find_framebuffer()`): a `simple-framebuffer` node if there is one,
  otherwise the `cont_splash_mem` carve-out under `/memory`;
- the pixel format is a build-time choice (`FB_BPP` in `shim/console.h`),
  because the format is not in the device tree. The panel's
  `qcom,mdss-dsi-bpp = <0x18>` says 24 bpp, and 1440x2560x3 = 10.5 MiB
  fits the 12 MiB window while 1440x2560x4 = 14.1 MiB does not. 24 is the
  default; if the screen shows garbage, change `FB_BPP` to 32 or 16 and
  rebuild.

Command mode also means that writing to RAM is not enough to make
something appear: the DSI controller has to be programmed to send the
buffer. That is the Phase 4 display driver.

## 6. Bugs found and fixed while porting

1. **`#ifdef SHIM_HW_DIAG` was true even for `-DSHIM_HW_DIAG=0`.**
   `#ifdef` tests *definition*, not value. The QEMU build therefore still
   read the watchdog register at 0xf9017000 and took a data abort:
   `FAR 00000000F9017008`. Fixed by using `#if SHIM_HW_DIAG`.

2. **The shim was not position-independent.** `mmu.c` computed the image
   range from the *link-time* addresses (`_head` is 0 in the linker
   script), but the bootloader loads the image wherever it likes. On QEMU
   the image lands at 0x40200000, so the page tables covered the wrong
   memory. Fixed by having `start.S` record the runtime address of
   `_start` in `shim_runtime_base` and adding it to the link-time offsets.

3. **`memset` was missing.** A freestanding build has no libc; GCC turns
   `struct fb_search s = {0};` into a `memset` call. Added `shim/lib.c`.

4. **BSD `awk` has no `strtonum`.** `shim/build.sh` used GNU awk to
   convert the `_end` symbol to a number. Replaced with shell arithmetic
   (`size=$((0x$size))`).

5. **The console overwrote long labels.** `con_str()` wrote the value at
   column 8 whatever the label length, so a 19-character property name
   was overwritten by its own value. Now the value starts after the label.

## 7. Testing without the phone

```sh
SHIM_HW_DIAG=0 CON_SCALE=2 ./shim/build.sh
python3 tools/qemu-g4.py
python3 tools/fb2text.py shim/out/qemu-fb.ppm --scale 2
```

The QEMU test builds a small device tree with a `simple-framebuffer` node
in RAM, runs the shim as an arm64 Linux image, and dumps guest RAM
through a memory-backend file. Expected output:

```
SHIM    diagnostics
IMAGE   lo=40200000 hi=40238000
MMU     rc=FFFFFFFE l3=FFFFFFFF      <- -2: QEMU's cortex-a53 has no 16K granule
PARANGE pa=00000002 tgran4=00000000
TGRAN   16k=00000000 64k=00000000
GRANULE 16k=00000000 tg1=00000000
```

`rc=FFFFFFFE` is expected and harmless: the shim checks
`ID_AA64MMFR0_EL1.TGran1` before it builds 16 KiB page tables and skips the
MMU test when the granule is unavailable. **On the real phone this line is
the answer to the page-size question** that Phase 2 depends on.

Note on the labels: `PARANGE pa=` is the physical-address-range field
(bits [3:0]), **not** a granule. The granule fields are `TGran4`
(bits [15:12]), `TGran16` (bits [19:16]) and `TGran64` (bits [23:20]), and
each is a **bitmask**: bit 0 = 4 KiB, bit 1 = 16 KiB, bit 2 = 64 KiB. So
`TGRAN 16k=00000002` means "16 KiB only", `00000006` means "4 KiB and
16 KiB", `00000007` means "all three".

## 8. Testing on the phone

```sh
# the phone must be in fastboot: power off, then hold Volume Up + Power
fastboot devices
fastboot boot shim/out/shim-boot.img
```

`fastboot boot` is temporary: nothing is written to any partition. If the
phone shows nothing, take a photograph of the screen; the shim's text can
be read from it with `tools/fb2text.py` (or by eye).

To get the text back as text, run the shim in a QEMU-like environment and
dump the framebuffer; on the phone the persistent log (Phase 2's
`tools/sweet-log-pull.sh`) is the equivalent once XNU is running.

## 9. What the first device boot is expected to tell us

The shim dumps the live device tree at the end of its run, because LK
rewrites `/memory` and `/chosen` before the jump:

- the **real RAM map** (`/memory`, which is `<0 0 0 0>` in the stored
  device tree) — this decides where XNU can live;
- the **carve-out list** under `/memory`;
- `linux,initrd-start`/`end`, which the payload loader uses;
- `ID_AA64MMFR0_EL1.TGran1`, i.e. whether 16 KiB pages exist;
- the **PSCI version**, if the trust zone answers at all.
