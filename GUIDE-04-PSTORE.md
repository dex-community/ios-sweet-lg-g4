# GUIDE-04 — The ramoops channel: getting the shim's log off the phone

The shim runs **instead of** the Android kernel, so it has no filesystem, no
network and no way to hand a log to the host. This guide builds the channel
that does work on this phone: the **ramoops** region, a piece of DRAM that
survives the reset and that the next kernel boot reads back as
`/sys/fs/pstore/console-ramoops-0`.

It replaces the two channels that do not work:

| channel | why it fails on the G4 |
|---|---|
| the eMMC card (log sectors) | the card does not answer to CMD0/CMD1: that is the open bug |
| scratch RAM at 0x50000000 | readable only with `/dev/mem`, which answers ENXIO |

Everything here was verified against the phone's own kernel sources and
against the phone's own pstore file.

---

## 1. What the ramoops region is, on this phone

From the device tree the phone boots
(`/Users/io/Dev/ios/lg g4/msm8992-lg-h815-boot.dts:13066`):

```
ramoops {
        compatible = "ramoops";
        android,ramoops-buffer-start = <0xff00000>;
        android,ramoops-buffer-size   = <0x100000>;
        android,ramoops-console-size  = <0x20000>;
        android,ramoops-pmsg-size     = <0x20000>;
        android,ramoops-record-size   = <0xffffa>;
        android,ramoops-dump-oops     = <0x01>;
        android,ramoops-ecc-size      = <0x10>;
};
```

and from the phone's own kernel log (`adb shell dmesg | grep ramoops`):

```
cma: Found ramoops_region@0, memory base 0x000000000ff00000, size 1 MiB
ramoops: ftrace not configured
console [pstore0] enabled
ramoops: attached 0x100000@0xff00000, ecc: 16/0
```

The region is 1 MiB of ordinary DRAM at **0x0ff00000**. It is *above* the
150 MiB `secure_region` (0x00000000-0x09600000) that the XPU protects from
the non-secure EL1 world where the shim runs, so the shim may write there.
The recovery and Android both have the ramoops driver, with the same layout
(both show a 116488 byte `console-ramoops-0`).

## 2. How the kernel lays the region out

`fs/pstore/ram.c` (the phone's kernel is LG's 3.10.108; the code is the
same in the 3.18 tree under `G4-Titan-Kernel-titan_bpf2/fs/pstore/`):

1. the **record** zones (one per crash dump) come first:
   `max_dump_cnt = (size - console - pmsg) / record_size
   = 0xc0000 / 0xffffa = 0` → **no record zone at all**;
2. then the **console** zone, 0x20000 bytes;
3. then the **pmsg** zone, 0x20000 bytes.

**CORRECTION (2026-11-09).** The two sections above (§1) describe the
device tree of the **Android** kernel, and they are right for it: Android
asks for a record size of 0xffffa, which does not fit in the space left
over, so Android has **no record zone** and puts the console zone at the
start of the region, **0x0ff00000**.

The **recovery** lays the same region out differently. Its `/proc/iomem`
lists three `persistent_ram` regions:

```
0ff00000-0ff7ffff   (512 KiB)  the record zone
0ff80000-0ff9ffff   (128 KiB)  the CONSOLE zone   <-- what the recovery reads
0ffa0000-0ffbffff   (128 KiB)  the pmsg zone
```

So the console zone the recovery reads is at **0x0ff80000**, not at
0x0ff00000. The shim therefore writes the record in **both** candidate
places, the recovery's first (`shim/pstore.c`, `RAMOOPS_BASE` =
0x0ff80000, `RAMOOPS_BASE2` = 0x0ff00000): the recovery is the first
kernel that boots after the shim and it empties the zone it thinks is a
record zone, so a record left only at 0x0ff00000 would not survive it.

**The truth is in `shim/pstore.c`, not in this guide**: the two constants
at the top of that file are what the writer actually uses.

Inside a zone (`fs/pstore/ram_core.c`):

```
struct persistent_ram_buffer { u32 sig; u32 start; u32 size; u8 data[]; };
```

`sig` is `PERSISTENT_RAM_SIG` = 0x43474244 ("DBGC"). `start` and `size`
describe a ring: the data is written at `start`, `start` moves on, `size` is
how much of the ring holds data. The kernel reads the ring back from
`start`, wrapping at the end (`persistent_ram_save_old`).

The zone is protected by a **Reed-Solomon code**: every 128 byte block of
the data has 16 parity bytes, and the 12 byte header has 16 more. The
numbers for the console zone:

```
data area   = 0x20000 - 12 - ((911 + 1) * 16) = 116468 bytes
parity area = 911 * 16 = 14576 bytes
header      = 16 bytes
```

`116468 + 20` (the ECC notice the kernel appends) = **116488**, exactly the
size of the `console-ramoops-0` file the phone produces. That is the check
that the layout above is the right one.

Addresses of the console zone (for this phone). The **recovery** reads the
one at 0x0ff80000; the shim writes both (see the correction in §2):

```
                  recovery reads      shim writes
header       0x0ff80000               0x0ff80000 (and 0x0ff00000)
data         0x0ff8000c               0x0ff8000c (and 0x0ff0000c)
parity       0x0ff9c6e0               0x0ff9c6e0 (and 0x0ff1c6e0)
parity (hdr) 0x0ff9fe90               0x0ff9fe90 (and 0x0ff1fe90)
```

## 3. Why the parity has to be written too

The kernel's decoder (`persistent_ram_decode_rs8`) checks every block
against its parity. If the parity is missing or wrong it does not simply
complain: it computes a syndrome, decides the block is damaged and
**"corrects" it**, which puts the *old* bytes back. The header is the worst
case: changing `start` and `size` is a two byte error, and a two byte error
is always correctable, so a header written without its parity would be
reverted and the kernel would read the previous contents instead of the new
record.

So the writer has to produce the parity as well.

## 4. The implementation

| file | what it does |
|---|---|
| `shim/pstore.c`, `shim/pstore.h` | writes the record into the console zone |
| `shim/rs8.c`, `shim/rs8.h` | the Reed-Solomon encoder (8 bit symbols, 16 parity) |
| `tools/read-pstore.py` | finds the record in the pstore file and checks its CRC |
| `tools/fetch-pstore.sh` | pulls the pstore file from the phone and decodes it |
| `tools/pstore-test.sh` | checks the writer against the kernel's own encoder and decoder |
| `tools/rs8-test.sh` | checks the encoder against the kernel's own, byte for byte |

`shim/rs8.c` is the kernel's encoder (`lib/reed_solomon/reed_solomon.c` and
`encode_rs.c`), reduced to what the ramoops uses: GF(2^8) with the
polynomial 0x11d, 16 roots, first root 0, generator 1.

The record itself is plain text, the format the kernel's console uses:

```
[G4SHIMPS1] len=<bytes in decimal> crc=<crc32 in hex>
<the log>
[G4SHIMPS1-END]
```

The CRC is what tells a record that survived the trip from the phone to the
host from one that a parity block mangled on the way.

### The two writes

`pstore_console_write()` is called at two moments:

* at the end of the eMMC bring-up, with the MMU still off, so that the
  result of the bring-up is on its way out even if something later fails;
* in `reset()`, with the whole log.

The write is attempted **at most twice**: if the second one faults, the
exception handler calls `reset()` again, and a third attempt would fault
again and loop forever. The counter in `shim/pstore.c` is what stops that.

## 5. Building and checking it, without the phone

```sh
cd /Users/io/Dev/ios/ios-sweet-lg-g4

# The encoder against the kernel's own (byte for byte).
tools/rs8-test.sh

# The writer against the kernel's own encoder and decoder: the record has
# to come back whole, and every block has to pass the kernel's decoder
# without it changing a byte.
tools/pstore-test.sh
```

Both print `OK`.

## 6. Using it on the phone

```sh
# 1. build the shim (the phone build, with the ramoops region at 0xff00000)
cd shim && RAW_TIMING=1 FINE_TIMING=1 DIAG_VARIANTS=1 DIAG_TIMING=1 ./build.sh && cd ..

# 2. run it
MODE=diag IMG=shim/out/shim-boot.img tools/run-shim.sh

# 3. read the log back (the phone is in the recovery now)
tools/fetch-pstore.sh research/raw-2026-11-05/pstore-run1.bin
```

The output is the shim's log, for example:

```
EMMC start
PV0 dc=00000000 e0=00000000
PV0 e4=00000000 e8=00000000
PV0 hc29=00000000 step=0000001E
...
VAR n=00000000 stage=00000000
VAR n=00000001 stage=00000003
```

`PVn` are the readings of the PWRCTL registers during the try of variant
`n` (see `shim/emmc_lk_bringup.inc`), `VAR` lines say how far each variant
got.

### If the record does not come back

* `no shim record in ...` → the shim did not write, or the zone the kernel
  read is not the one the shim wrote. Check the `PSTORE hdr=` line in the
  log: it says which address the shim used.
* `crc WRONG` → the record was written but the parity did not survive.
  The host cannot tell whether the text or the parity is wrong, so the raw
  file has to be kept (`fetch-pstore.sh` does).

## 7. The same thing under QEMU

QEMU has no DRAM at 0x0ff00000, and its `virt` machine has only 128 MiB of
RAM at 0x40000000, so the model build points the writer at 0x44000000
(`shim/build.sh`):

```sh
cd shim && SHIM_HW_DIAG=1 SHIM_EMMC_MODEL=1 DIAG_VARIANTS=1 DIAG_TIMING=1 ./build.sh && cd ..
python3 tools/qemu-emmc.py --mode ack --diag --secs 90
python3 - <<'EOF'
data = open('shim/out/qemu-emmc/qemu-ram.bin', 'rb').read()
open('/tmp/zone.bin', 'wb').write(data[0x4000000:0x4020000])
EOF
python3 tools/read-pstore.py /tmp/zone.bin
```

This checks the whole path (the writer, the ring, the parity and the
reader) without the phone. It says nothing about the phone's hardware.
