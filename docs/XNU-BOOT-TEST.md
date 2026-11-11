# The XNU boot test on the LG G4 H815

Everything here is `fastboot boot`: nothing is ever written to the phone,
the `boot` partition is not touched, and the only image that is loaded is the
one that is named in this document.

## The three images

| image | what it is | why |
| --- | --- | --- |
| `shim/out/shim-boot-g4.img` | the LK boot image with the shim and the XNU payload in its ramdisk | what XNU boots from. `tools/build-g4-image.sh` builds it and checks that the ramdisk in the image is byte for byte the payload |
| `/tmp/device-test-boot.img` | the custom TWRP: the CI H815 kernel (with `CONFIG_DEVMEM`) plus the TWRP ramdisk and `dt.img` | **the only image on this phone that can read the RAM**. TWRP itself has no `/dev/mem` |
| `shim/out/g4.bin` | the payload (kernel + ADT + boot args) | the addresses of the kernel symbols are computed from its header |

`/tmp/device-test-boot.img` is built by `tools/device-test.sh` (it packs the
gzip kernel, the TWRP ramdisk and `dt.img`). It is not in the repository:
it is 24 MB and it is derived from a CI artifact.

## The cycle

```sh
# 1. the CI gives the kernel
gh run download <RUN_ID> -n xnu-kernel -D xnu/out-<RUN_ID>

# 2. the payload and the LK boot image
CMDLINE="debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=emmc0" \
  tools/build-g4-image.sh <RUN_ID> g4

# 3. the whole test, in one command
tools/xnu-boot-test.sh <RUN_ID> g4
```

`tools/xnu-boot-test.sh` does:

1. `fastboot boot shim/out/shim-boot-g4.img` — the LK bootloader loads the
   shim, the shim brings the eMMC host up (core reset, legacy clock path,
   400 kHz, CMD0..CMD16), writes its log and hands over to XNU;
2. it waits for the phone to settle — XNU either boots, or it panics and the
   phone reboots into the recovery;
3. `fastboot boot /tmp/device-test-boot.img` — the custom TWRP. **This step
   is not optional**: XNU says what it says in the RAM, and the RAM can only
   be read with a kernel that has `/dev/mem`;
4. it reads the RAM: the log of the eMMC driver, the console ring of the
   kernel and the log of the shim.

Everything ends up in one file: `/tmp/xnu-boot-test-<date>.txt`.

If the phone ends up running XNU and answers neither adb nor fastboot, reboot
it into the bootloader (power + volume down) and run the script again with
`--collect`, which only does the reading.

## Where XNU says things

| what | where | how to read it |
| --- | --- | --- |
| the eMMC driver | `emmc_log_buf` in the BSS of the kernel | `tools/read-xnu-log.py` |
| the console of the kernel | the 4 MiB ring `sweet_log_ring` in the BSS | `tools/read-xnu-log.py` |
| the shim | `0x50000000` in the RAM | `tools/read-shim-log.py` |
| the ramoops | `0xff00000` | `/sys/fs/pstore/console-ramoops-0` (only the shim writes it) |

The physical address of a kernel symbol comes from the header of the payload:

```
pa = va - virt_base + phys_base
```

which is the same mapping `phystokv()` uses
(`osfmk/arm64/arm_vm_init.c`). `virt_base` and `phys_base` are at 0x10 and
0x38 of the payload header.

**The console of the BSD layer does not exist yet when the eMMC driver
runs.** The driver is called from `IOFindBSDRoot`, which is in
`IOKitBSDInit`, long before `bsd_init` puts the console up: the hook on
`_cnputs` that fills the ring is installed far later. That is why the driver
keeps a copy of its own words in the BSS: the ring is empty when the driver
fails, and the driver is exactly what has to be seen.

## The RAM is not scrubbed

A reset does not scrub the DRAM on this SoC, so what the kernel wrote is
still there after the phone comes back up. Two things can still overwrite it:

* the payload. The shim loads it after it writes its own log, so a payload
  that reaches `0x50000000` (the scratch address of the log of the shim)
  wipes it. The XNU payload is 17 MB;
* the kernel of the recovery, which takes the pages it needs from the RAM.
  The ring and the log of the driver are in the bank at `0x80000000`, the
  low bank at `0x10000000` is taken first, so they survive.

The address of the scratch log of the shim, `0x50000000`, is in the bank at
`0x10000000` (`/proc/iomem` on the phone): the recovery can take those
pages, so the log of the shim is not always there after an XNU boot.
