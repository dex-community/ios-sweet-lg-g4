# GUIDE-00 — Setup for the LG G4 (H815) iOS 16 port

Everything needed to get from a clean checkout to a bootable shim image.
Nothing here needs the phone.

## 1. What this repository is

`ios-sweet-lg-g4/` is a fork of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet),
which ports Apple's XNU kernel (xnu-8796, the iOS 16 generation) to the
Xiaomi Redmi Note 10 Pro (`sweet`, SM8150) and runs the unmodified iPhone X
iOS 16.7.16 userland on it.

This fork does the same for the **LG G4 (H815)**, an MSM8992 phone with an
**LK** bootloader instead of Qualcomm's ABL. The upstream README explains
what the pieces do; this guide only covers what differs.

```
ios-sweet-lg-g4/
├── shim/            bare-metal boot shim, packed as an Android boot image
│   ├── g4.h         all G4 hardware constants, each with a source reference
│   ├── shim.c       entry, exceptions, GIC/timer probes, device-tree dump
│   ├── console.c    text console on the bootloader framebuffer
│   ├── fdt.c        just enough flattened-device-tree parsing
│   ├── loader.c     finds the XNU payload in the ramdisk and jumps to it
│   ├── mmu.c        EL1 stage-1 MMU (16 KiB granule)
│   ├── lib.c        memset/memcpy for the freestanding build
│   └── build.sh     builds the shim and wraps it in an LK boot image
├── tools/
│   ├── mkbootimg_lg.py   builds an LK-style boot image (dt.img appended)
│   ├── verify_bootimg.py re-parses that image the way LK does
│   ├── qemu-g4.py        boots the shim under QEMU and saves the screen
│   ├── fb2text.py        turns a framebuffer back into text
│   └── mkadt.py          builds the Apple Device Tree (Phase 2)
├── xnu/patches/     the XNU patch series (Phase 2 onwards)
├── dtb/             the phone's device trees (not in git)
└── docs/g4-hardware/ hardware references extracted from the device tree
```

## 2. Prerequisites

| Tool | Why | macOS | Debian/Ubuntu |
| --- | --- | --- | --- |
| `aarch64-elf-gcc` | cross-compile the shim | `brew install aarch64-elf-gcc` | `gcc-aarch64-linux-gnu` |
| `aarch64-elf-objcopy`, `-nm`, `-readelf` | the same package | included | `binutils-aarch64-linux-gnu` |
| `python3` | the tools | preinstalled | `python3` |
| `dtc` | device-tree work | `brew install dtc` | `device-tree-compiler` |
| `qemu-system-aarch64` | shim test | `brew install qemu` | `qemu-system-arm` |
| `fastboot`, `adb` | talking to the phone | `brew install android-platform-tools` | `android-sdk-platform-tools` |

`shim/build.sh` finds the cross tools automatically: it prefers
`aarch64-elf-*` and falls back to `aarch64-linux-gnu-*`. Override with
`CC=`, `OBJCOPY=`, `NM=`, `READELF=` if needed.

## 3. The assets that are not in git

`.gitignore` keeps `dtb/`, `backup/` and `recovery/` out of the repository
because they contain device-specific data. Put these in place once:

| Path | Where it comes from |
| --- | --- |
| `dtb/h815.dt.img` | the `dt.img` (QCDT v3) from the phone's stock boot image. In this workspace: `lg g4/aicp/unpacked_dt.img` |
| `dtb/boot.dtb` | the H815 device tree on its own: `lg g4/msm8992-lg-h815-boot.dtb` |
| `recovery/recovery.img` | TWRP or OrangeFox, to read the persistent kernel log |
| `backup/` | partition dumps (`persist`, `modemst1/2`, `fsg`, `fsc`, `boot`, `dtbo`, `vbmeta`) — they hold calibration data |

Copy them in:

```sh
cd ios-sweet-lg-g4
mkdir -p dtb
cp "../lg g4/aicp/unpacked_dt.img"   dtb/h815.dt.img
cp "../lg g4/msm8992-lg-h815-boot.dtb" dtb/boot.dtb
```

### Where `dtb/h815.dt.img` comes from

It is the device-tree table LG appends to the stock boot image. To extract
it from a boot image of your own:

```sh
# a boot image is: header | kernel | ramdisk | dt.img
python3 - <<'PY'
import struct
d = open('boot.img','rb').read(64)
ks, ka, rs, ra, ss, sa, ta, ps, dt = struct.unpack('<9I', d[8:44])
off = ps + roundup(ks,ps) + roundup(rs,ps) + roundup(ss,ps)
open('h815.dt.img','wb').write(open('boot.img','rb').read()[off:off+dt])
PY
```

`dt_size` is at **offset 40** of the header, which is LG's own convention —
see `GUIDE-01-SHIM.md`.

## 4. The IPSW

The iOS userland comes from the iPhone X IPSW:

```
iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw
```

It is not in git (it is Apple's copyrighted content). Its size is about
6.2 GB. `tools/mkiosroot.sh` and `tools/sweet-root-tweaks.py` build the
HFS+ root volume from it; that part is unchanged from upstream and needs
Linux with `apfs-fuse` and `hfsprogs` (Phase 3).

## 5. Build the shim

```sh
cd ios-sweet-lg-g4
./shim/build.sh                       # no payload: the shim runs diagnostics
./shim/build.sh /path/to/payload.bin  # with an XNU payload in the ramdisk
```

It produces:

| File | What it is |
| --- | --- |
| `shim/out/Image` | the raw shim, with the arm64 Linux image header |
| `shim/out/shim-boot.img` | the LK boot image, ready for `fastboot boot` |
| `shim/out/host.ppm` | a host render of the console layout, to check the font |
| `shim/out/shim.elf` | the linked ELF, for symbols |

## 6. Check the boot image before touching the phone

```sh
python3 tools/verify_bootimg.py shim/out/shim-boot.img
```

This re-implements LK's parsing (`app/aboot/aboot.c`,
`platform/msm_shared/dev_tree.c`) and asserts that the H815 device tree is
the one that would be selected, and that the kernel, the device tree and
the ramdisk do not overlap. It must print `PASS`.

## 7. Run the shim under QEMU

```sh
SHIM_HW_DIAG=0 CON_SCALE=2 ./shim/build.sh
python3 tools/qemu-g4.py
python3 tools/fb2text.py shim/out/qemu-fb.ppm --scale 2
```

`SHIM_HW_DIAG=0` builds a variant that touches no phone-specific MMIO (the
watchdog at 0xf9017000 and the GIC at 0xf9000000 do not exist in QEMU).
`CON_SCALE=2` uses a smaller font so the text fits the test screen.

The decoder prints what the shim drew. On the phone the same text is read
from a photograph of the screen.

## 8. What is left for the other phases

- **Phase 2** needs a Linux or macOS machine that can build XNU. Upstream
  builds it in GitHub Actions with
  [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build);
  `xnu/patches/` has to gain an `msm8992` machine configuration.
- **Phase 3** needs the IPSW and `apfs-fuse`/`hfsprogs`.
- **Phase 4** needs the phone, to read the live device tree and probe the
  touch controller.

## 9. Conventions used in this fork

- Every hardware constant in `shim/g4.h` carries the `file:line` of the
  device tree it came from.
- Facts that were read from the device tree are separated from facts that
  were inferred, in `docs/g4-hardware/HARDWARE-G4.md`.
- Failed attempts are recorded with their exact error in `NOTES.md`.
- Nothing is flashed permanently: only `fastboot boot`.
