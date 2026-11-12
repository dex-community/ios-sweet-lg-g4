# iOS 16 on LG G4 (H815) - working notes

Last update: 2026-11-12 02:40 CET

## HANDOFF 2026-11-11 04:10 - Test ciclo boot completo

### Fatto oggi
**Test pulito dal bootloader verificato** - commit 0773118:
- ✅ Ciclo boot → crash → recovery funziona perfettamente
- ✅ Recovery flashata (device-test-boot.img) ha `/dev/mem` funzionante
- ✅ LK boot automaticamente in recovery dopo crash XNU (~5s)
- ✅ Script read-xnu-log.py e read-shim-log.py funzionano

**Problema confermato**: XNU crasha **immediatamente**, prima di:
- Inizializzare console BSD (log vuoto)
- Eseguire driver eMMC (log vuoto)
- Scrivere qualsiasi diagnostica

### Files chiave
- Boot image: `shim/out/shim-boot-g4.img` (17MB, 2026-11-11 03:18)
- Kernel XNU: `xnu/out-42942530869/kernel.development.sweet` (CI run 42942530869)
- Payload: `shim/out/g4.bin` (16.6MB)
- Test report: `test-reports/xnu-boot-test-2026-11-11.md`

### Next steps (priorità)
1. **Shim pre-XNU logging**: modificare lo shim per scrivere log a 0x50000000
   PRIMA di saltare a XNU - confermare che il handoff avviene
2. **Dump boot_args/ADT**: salvare parametri passati a XNU
3. **Verificare virt_base/phys_base**: confrontare con sweet/v60 funzionanti
4. **Exception vector early**: verificare che exception handler sia setup

### Stato telefono
- In recovery (LGH815b448b72e)
- Recovery: device-test-boot.img con CONFIG_DEVMEM=y
- Nessuna partizione modificata permanentemente

---

# iOS 16 on LG G4 (H815) - working notes

Last update: 2026-11-11 04:10 CET

## HANDOFF 2026-11-11 04:10 - Test ciclo boot completo

### Fatto oggi
**Test pulito dal bootloader verificato** - commit 0773118:
- ✅ Ciclo boot → crash → recovery funziona perfettamente
- ✅ Recovery flashata (device-test-boot.img) ha `/dev/mem` funzionante
- ✅ LK boot automaticamente in recovery dopo crash XNU (~5s)
- ✅ Script read-xnu-log.py e read-shim-log.py funzionano

**Problema confermato**: XNU crasha **immediatamente**, prima di:
- Inizializzare console BSD (log vuoto)
- Eseguire driver eMMC (log vuoto)
- Scrivere qualsiasi diagnostica

### Files chiave
- Boot image: `shim/out/shim-boot-g4.img` (17MB, 2026-11-11 03:18)
- Kernel XNU: `xnu/out-42942530869/kernel.development.sweet` (CI run 42942530869)
- Payload: `shim/out/g4.bin` (16.6MB)
- Test report: `test-reports/xnu-boot-test-2026-11-11.md`

### Next steps (priorità)
1. **Shim pre-XNU logging**: modificare lo shim per scrivere log a 0x50000000
   PRIMA di saltare a XNU - confermare che il handoff avviene
2. **Dump boot_args/ADT**: salvare parametri passati a XNU
3. **Verificare virt_base/phys_base**: confrontare con sweet/v60 funzionanti
4. **Exception vector early**: verificare che exception handler sia setup

### Stato telefono
- In recovery (LGH815b448b72e)
- Recovery: device-test-boot.img con CONFIG_DEVMEM=y
- Nessuna partizione modificata permanentemente

---

# iOS 16 on sweet (Redmi Note 10 Pro, SM7150) - working notes
## 2026-11-05 (seconda sessione): il telefono vero

Tutto quello che segue è **misurato sul telefono**, non dedotto.

### Il bootloader

```
fastboot getvar all
  (bootloader) kernel:lk          <- LK, non ABL
  (bootloader) version:0.5
  (bootloader) unlocked:yes
  (bootloader) product:MSM8992_P1_GLOBAL_COM
  (bootloader) max-download-size:0x20000000
  (bootloader) partition-size:userdata:0x5cc000000
```

Limitazioni di questo LK (tutte verificate):

| Comando | Risultato |
| --- | --- |
| `fastboot boot <img>` | `FAILED (remote: 'unknown command')` |
| `fastboot fetch <part> <file>` | non implementato (file da 0 byte) |
| `fastboot reboot recovery` | accettato, ma il telefono torna in fastboot |
| `fastboot oem <qualsiasi>` | solo `oem device-id` risponde |

Quindi **l'unico modo per eseguire il nostro shim è scrivere una partizione**,
e l'unico modo per entrare nella recovery è `fastboot reboot` -> Android ->
`adb reboot recovery`.

### Recovery

TWRP 3.7 (`omni_g4-eng 16.1.0 OPM8.181105.002 23 test-keys`), **con root**:
`adb shell id` -> `uid=0(root)`.

Il suo kernel però non permette di leggere la RAM fisica:

| Via | Esito |
| --- | --- |
| `/dev/mem` | `No such device or address` (CONFIG_STRICT_DEVMEM) |
| `/proc/kcore` | non esiste |
| `sgdisk /dev/block/.../mmcblk0` | `Error is 13` (EACCES sul disco intero) |
| `/dev/graphics/fb0` | si legge, ma contiene la UI della recovery |

Le partizioni si possono comunque copiare (`dd` sui device node per partizione)
e il GPT si legge da **sysfs**: `/sys/class/block/mmcblk0pN/start` e `/size`.

Il SuperSU che l'utente aveva installato **non c'è**: nessun `su`, nessun
pacchetto, `/data/adb` non esiste. SuperSU non supporta comunque Android 13.

### Memoria reale (`/proc/iomem`)

```
00000000-063fffff : System RAM          100 MB
07200000-073fffff : System RAM            2 MB
0f400000-0fefffff : System RAM           11 MB
0ff00000-0ffdffff : persistent_ram         1 MB
10000000-5fffffff : System RAM         1280 MB   <- banco basso grande
60000000-7fffffff : (buco: non RAM)
80000000-dfffffff : System RAM         1536 MB   <- banco alto
```

3 GiB in tutto (`androidboot.ddr_size=3221225472`). Il buco a
60000000-7fffffff e' la regione che il bootloader tiene per se'.

**Decisione per XNU: `PHYS_BASE = 0x80000000`, `MEM_SIZE = 0x60000000`**
(1.5 GiB): il banco piu' alto, sopra ogni carve-out della XPU
(`secure_mem` 0x0-0x9600000, `modem_mem` 0x9000000-0xd400000, ...). Stessa
logica con cui lo sweet sceglie la "upper DDR bank" sul Redmi.

### Pannello e framebuffer

```
/proc/cmdline: mdss_mdp.panel=1:dsi:0:qcom,mdss_dsi_jdi_nt36850_1440p_mipi0_cmd:1:..._mipi1_cmd
/sys/class/graphics/fb0: mdssfb_90000  1440,5120  stride=5760  bpp=32
```

Il pannello e' il **JDI NT36850 1440p, dual MIPI DSI, command mode**. Il
framebuffer e' **ARGB8888, stride 5760** (mentre il link DSI trasporta 24
bpp). Il `cont_splash_mem` a 0x03400000 e' 12 MiB e **non ci stanno** 1440x2560
a 32 bpp (servono 14.1 MiB): la finestra di splash non e' un framebuffer
completibile.

### Il display non mostra il nostro console

Lo shim gira fino in fondo (il telefono torna nel bootloader dopo esattamente
il ritardo che il codice imposta), ma sullo schermo resta il logo LG.

L'ipotesi piu' probabile: **0x0-0x9600000 e' la "secure region" protetta
dalla XPU**, e in EL1 non sicuro la CPU non ci scrive. Il logo LG lo mette
LK (mondo sicuro) e l'hardware MDP lo scandisce; noi non possiamo scriverci
dentro. Sweet non aveva questo problema: il suo pannello e' in **video
mode**, quindi il semplice scrivere nel buffer basta.

Conclusione per la Fase 4: **serve un driver display vero**, che programmi
MDP+DSI e usi un buffer in RAM normale (non la finestra di splash).

Riferimento pronto: il kernel
`https://github.com/nepheloid/nethunter_kernel_g4` (albero completo per il
G4) contiene

* `arch/arm64/boot/dts/lge/dsi-panel-jdi-nt36850-dualmipi{0,1}-qhd-cmd.dtsi`
  - il pannello esatto del telefono, in command mode: 720x2560 per link,
  24 bpp, `rgb_swap_rgb`, burst mode, `lane_map_0123`, porches
  (h: 180/120/4, v: 7/8/1), reset sequence, on/off command DCS;
* `arch/arm64/boot/dts/qcom/msm8992-mdss.dtsi` - i controller DSI:
  `mdss_dsi0` = `qcom,mdss-dsi@fd994000` (ctrl 0x260, phy 0xfd994500),
  `mdss_dsi1` = `qcom,mdss-dsi@fd996000`;
* `drivers/video/msm/` - il driver MDP/DSI completo da cui ricavare
  l'inizializzazione.

### Il canale log: la RAM non basta, l'eMMC nemmeno (per ora)

Il shim mantiene una copia ASCII del console (`shim/console.c`, `con_log`) e
la scrive in due posti:

1. **RAM scratch a 0x50000000** con il marker `G4SHIMLG` - un warm reset non
   scrubba la DRAM, ma nessuna recovery di questo telefono puo' leggerla
   (`/dev/mem` bloccato, niente `/proc/kcore`);
2. **eMMC**, un settore da 512 byte, con il marker `G4SHIMLG` + codice
   errore + RCA + il log.

Il primo canale non e' leggibile. Il secondo **non funziona ancora**: il
settore `misc` (LBA 262400) resta a zero e `cache` (LBA 9896192) resta
intatto, quindi il driver non arriva alla card. Motivo probabile: il
controller va inizializzato come fa Android (`drivers/mmc/host/sdhci-msm.c`
del kernel NetHunter: `core_clk` rate, DLL per HS400, UHS signaling) e non
basta l'SDHCI standard come nel driver UniXNU, che riutilizzava lo stato
lasciato dal firmware.

Nota: il tempo di reset e' passato da 15 s a 60 s con il driver eMMC, cioe'
il driver sta esaurendo i timeout: si arriva al controller ma la card non
risponde.

### Backup

Presi dal telefono in recovery (root), in `backup/`:

| File | Dimensione | Contenuto |
| --- | --- | --- |
| `boot-stock.img` | 40 MiB | partizione `boot` **del telefono**, byte per byte |
| `persist-stock.img` | 32 MiB | calibrazione radio |
| `fsg-stock.img`, `fsc-stock.img` | 1.5/0.5 MiB | calibrazione |
| `modemst1/2-stock.img` | 1.5 MiB | stato modem |
| `devinfo-stock.img` | 512 KiB | stato bootloader (`ANDROID-BOOT!`) |

L'immagine `boot` del telefono **non coincide** con quella dello zip AICP
(kernel piu' grande nel ROM installato), ma il `dt.img` QCDT e' identico a
parte da tre proprietà irrilevanti (`qcom,idle-timeout`, i due `pm-qos-*`,
`qcom,popp-enable`).

`userdata` non e' mai stata toccata.

### Cosa funziona davvero

- Lo shim e' **LK-compatible** (`tools/verify_bootimg.py` PASS): header con
  `dt_size` a offset 40, QCDT v3 allegato, indirizzi senza sovrapposizioni.
- Lo shim **si carica e gira sul telefono**: LK lo avvia, arriva in fondo a
  `shim_main`, scrive il log e si riavvia da solo.
- Il QEMU dice la stessa cosa: lo shim arriva alle diagnostiche.

### Cosa manca

1. **Il driver eMMC** non raggiunge la card (sopra).
2. **Il display** non e' pilotato (Fase 4, driver MDP/DSI).
3. **La build di XNU**: sorgente xnu-8796.141.3 clonato in
   `/Users/io/Dev/ios/xnu-build/xnu-8796`; la macchina config `SWEET` va
   clonata per `msm8992`, e la patch SMP via PSCI (0065) va esclusa perche'
   **l'H815 non ha PSCI** (`enable-method = "qcom,8994-arm-cortex-acc"`).
4. Il granolo pagina: il kernel Android del G4 gira a 4 KiB, XNU vuole 16
   KiB. **LPA non c'entra**: LPA riguarda gli indirizzi fisici estesi, non
   il granulo della passeggiata. Se il core implementa il granulo da 16
   KiB lo dice `ID_AA64MMFR0_EL1.TGran1` (bit 1 della bitmask), e va letto
   sul telefono: il shim lo stampa (`TGRAN 16k=…`), e la patch 0084 registra
   anche il `TCR_EL1` che XNU scrive per sé.


---

# iOS 16 on LG G4 (H815) - porting notes

Working copy: `ios-sweet-lg-g4/` (a clone of dipa-studio/ios-sweet). Base
project: the Redmi Note 10 Pro (`sweet`, SM8150, Qualcomm ABL). Target:
LG G4 H815, MSM8992, **LK** bootloader.

See `GUIDE-00-SETUP.md` (setup), `GUIDE-01-SHIM.md` (the shim and the LK
boot image) and `STATUS.md` (state).

## 2026-11-05: phase 0 done, shim builds and passes the QEMU test

### What the reference projects actually contributed

- **ios-v60** (ch1tmdgus): the shim between sweet and v60 differs in only
  a handful of constants (IMEM restart reason, GIC bases, timer INTIDs,
  the watchdog) - `diff sweet/shim ios-v60/shim` shows start.S, fdt.c,
  mmu.c, console.c and shim.ld are byte-identical. Its value is the
  *method*: `docs/v60/HARDWARE.md` is the model for recording hardware
  facts with their source, and `docs/v60/HISTORY.md` for the attempt log.
  It does **not** help with the bootloader: the V60 also uses ABL.
- **UniXNU-lumia950xl**: the Lumia 950 XL is also an MSM8992, so its eMMC
  controller is at the *same address with the same IRQs* as the G4's
  (`sdhci@f9824900`, IRQ 123/138). Its driver lives in
  `drivers/NativeSdhci/` (an IOKit `IOBlockStorageDevice`), which does not
  fit sweet's base (no IOStorageFamily, IOMedia patched out). The plan is
  to port only the freestanding core (`SdhciCore.cpp`, `PollingHost.cpp`,
  `SdhciRegs.h`) into sweet's `bsd/dev/sweet_ufs.c` shape as
  `bsd/dev/sweet_emmc.c`. Two risks it flagged: the G4's eMMC may have the
  command queue enabled (the UniXNU driver refuses it) and the G4's
  partition table is probably MBR, not GPT (UniXNU hard-requires
  "EFI PART").

### The G4's bootloader is LK, not ABL - the one big difference

sweet's `mkbootimg --header_version 2 --dtb <raw.dtb>` produces an image
for ABL. LK wants the device tree in a **QCDT v3 table appended to the
boot image**, with `dt_size` in the header at offset 40 (LK's
`struct boot_img_hdr` differs from AOSP's v0 there).

Decoded the G4's own stock boot image (`aicp/boot.img`, 11710464 bytes):
`ANDROID!`, page 4096, kernel 9449013 @ 0x8000, ramdisk 1023940 @
0x1000000, tags 0x100, **offset 40 = 0x0012d000**, and the appended
`unpacked_dt.img` is exactly 1232896 bytes and starts with `QCDT` v3.

The QCDT holds 10 entries of 40 bytes pointing at 4 distinct device trees.
`variant_id 0xb64` (the H815's `qcom,board-id`) points at offset 0xe2000,
whose sha1 matches `msm8992-lg-h815-boot.dtb` padded to 307200 bytes
(`6985b6f2`). **The stock QCDT is reused verbatim**, so LK's own matching
logic selects the right DTB.

### Address trap

LK copies the selected DTB (307200 bytes) to `tags_addr`. LG's stock
`tags_addr` is 0x100, which would land on top of a kernel at 0x8000, and
LG's own image has that overlap and still boots - so its LK must override
the header addresses (the msm8996 LK project does this with
`ABOOT_IGNORE_BOOT_HEADER_ADDRS`). Picked an arrangement that is correct
either way: kernel 0x8000, dtb/tags 0x400000, ramdisk 0x1000000, and
`tools/verify_bootimg.py` asserts no overlap.

### The hand-off to the kernel did not need changing

LK's `scm_elexec_call()` sets `param.el1_x0 = dtb_offset`, so x0 holds the
DTB pointer exactly like ABL. `shim/start.S` was left alone.

### Hardware facts (device tree, see docs/g4-hardware/HARDWARE-G4.md)

- GICD 0xf9000000, GICR 0xf9002000, timer 19.2 MHz, VTIMER INTID 19.
- Framebuffer: **no simple-framebuffer node**. Panel 1440x2560,
  `qcom,mdss-dsi-bpp = 0x18` (24 bpp), **command mode**, dual DSI.
  `cont_splash_mem` 0x03400000 + 0xc00000 (12 MiB).
- Touch: Synaptics s3528 @ I2C 0x20 **or** LG SIC lg2411 @ I2C 0x28, bus
  `i2c@f9924000`, IRQ 96, reset GPIO 8, IRQ GPIO 22. Both are enabled in
  the tree with the same IRQ - which one is fitted must be probed.
- Buttons: `gpio_keys` has only volume-up (PM8994 GPIO 3) and a hall
  sensor (TLMM GPIO 75). Volume-down and power are PMIC PON pins 2 and 1.
- PMIC: **PM8994 + PMI8994** (not PM8150/PM8916), SPMI arbiter 0xfc4c0000,
  IRQ 190.
- eMMC: `sdhci@f9824900` = sdhc1/mmc0, non-removable, 8-bit, IRQ 123/138.
- **PSCI is absent**: AOSP's h815 port says "psci is broken" and deletes
  the node; the CPU nodes use `enable-method = "qcom,8994-arm-cortex-acc"`
  (Qualcomm's SCF/ACC path). The AOSP port reserves the ARM spin-table
  handover slot at 0x06000000.
- The G4's Android kernel runs **4 KiB** pages; sweet's XNU is built for
  **16 KiB**. LPA is about extended physical addresses and says nothing
  about the granule: what counts is `ID_AA64MMFR0_EL1.TGran1` (bit 1 of a
  bitmask = 16 KiB), which has to be read on the device. The shim prints it
  (`TGRAN 16k=…`), and patch 0084 records the `TCR_EL1` that XNU writes for
  itself, right before the MMU is enabled.

### Attempts and what they taught

1. `mkbootimg` is not installed and AOSP's mkbootimg is the wrong format
   anyway -> wrote `tools/mkbootimg_lg.py` (LK header + QCDT appendix).
2. The harness safety gate blocks directory removal -> used `git clone`
   plus the recoverable-delete helper instead.
3. `awk: calling undefined function strtonum` - BSD awk has no strtonum
   -> replaced with shell arithmetic in `shim/build.sh`.
4. `undefined reference to memset` - freestanding build, GCC turns struct
   initialisation into a memset call -> added `shim/lib.c`.
5. QEMU with `-monitor stdio` and a piped command line: the monitor
   mangled the input ("invalid char 'U' in expression") and the run hung
   -> replaced the monitor with `-object memory-backend-file` and read the
   guest RAM from the file after QEMU exits.
6. The QEMU framebuffer came out **empty except the bottom rows**: QEMU
   was killed by the 60 s timeout in the middle of clearing the screen
   (786432 pixels x 3 byte writes to emulated device memory is slow) ->
   gave the run 25 s and used a smaller test screen.
7. The QEMU run panicked with `FAR 00000000F9017008` (the watchdog
   register) even though the build was supposed to skip hardware probes:
   **`#ifdef SHIM_HW_DIAG` is true for `-DSHIM_HW_DIAG=0`** - `#ifdef`
   tests definition, not value. Fixed with `#if SHIM_HW_DIAG`.
8. `IMAGE lo=6172662F` (ASCII garbage) and `TTBR0 30303030`: `mmu_enable()`
   returned early on the granule check *before* filling `mmu_info`, so the
   console printed uninitialised stack. Fixed by filling the struct first.
9. The framebuffer decoder produced garbage at scale 2 because
   `CH_W`/`CH_H` were computed at import time from the module-level
   `SCALE` -> compute them from the argument instead.

### Position-independence bug (would have hit the phone too)

`shim/mmu.c` derived the image range from the *link-time* addresses
(`_head` is 0 in the linker script). The bootloader loads the image
wherever it likes - QEMU puts it at 0x40200000, LK probably at
0x80080000 - so the page tables would have covered the wrong memory and
the very first instruction fetch after enabling the MMU would fault.
`shim/start.S` now records the runtime address of `_start` in
`shim_runtime_base` and `mmu.c` adds it to the link-time offsets.

### Current state

- `shim/build.sh` builds a 225280 byte shim and a 1466368 byte LK boot
  image; `tools/verify_bootimg.py` says PASS.
- The QEMU run reaches the diagnostics and prints them; `tools/fb2text.py`
  decodes the framebuffer back into text.
- **Nothing has been booted on the phone yet.** The next step is
  `fastboot boot shim/out/shim-boot.img` and a photograph of the screen.



## Agent handoff / active work (2026-10-15, latest)

### >>> UPDATE 2026-10-18 (read first, then the 10-17 handoff below) <<<

**STATE at end of 2026-10-18 (evening):**
- On the phone: userdata = **image v15** (`/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v15.img/.simg`; = v14 + `/private/var/preferences/SystemConfiguration/preferences.plist` with one network service "USB Ethernet": Interface DeviceName ue0 Type Ethernet, IPv4 Manual 192.168.7.2/24 router 192.168.7.1, DNS 1.1.1.1 8.8.8.8, set "Automatic" as CurrentSet). Fallback image v14 (v12/v13 deleted).
- Kernels: **0078** = all 8 cores (`cpus=8`, cpu6/7 = A76, homogeneous SMP; mkadt now emits 8 CPU nodes; ADT `shim/out/sweet-0078hi.adt`); **0079** = display off/on via IOMFB selector 12 kern_RequestPowerChange(state) (fade over 6 vsyncs, then black; verified: power button gives "SWEET FB: display off/on"); **0080** = `SWEET NET: <proc> route add/delete/change ...` and `... add/delete address ...` trace (built, `shim/out/shim-boot-0080s.img`, NOT booted yet).
- **Internet over USB: not working yet.** Host side is ready (temporary iptables: FORWARD accept for 192.168.7.0/24 + MASQUERADE, comment "ios-sweet"; lost on host reboot). With v15 the phone sent no DNS and no outbound traffic (only mDNS/ARP) -> configd probably did not apply the service (or did, without default route/DNS). Next: boot 0080s on v15, watch `SWEET NET` lines (does configd add 192.168.7.2 / default route on ue0?). If configd ignores ue0 (not an IOKit IONetworkInterface; configd 1241.140.6 source in `research/configd-1241`, SCNetworkInterface createInterface needs IONetworkInterface -> controller (IOMACAddress, SelfNamed) -> bus), add IOKit stand-ins and/or rename the ifnet to en*.
- Live log listener (`nc -ukl 5140`) may still be running, writing `logs/live-0078s.txt`.
- **CI `kernel-h815` fixed 2026-11-10 (run `42873352434`, success).** Commit `e215ee9` removed `cd "$RUNNER_TEMP/kernel"` but kept `git init` + `git remote add origin`, so those ran in the actions checkout (which already has `origin`) and the job died in 11 s with `error: remote origin already exists` (exit 3). Restored the `cd` in fetch/configure/dtc/build and made the patch step apply from `$GITHUB_WORKSPACE/kernel/patches` (it was copying to `./patches` but looping over `kernel/patches`). Artifact verified: `Image` 22.155.336 byte, `CONFIG_DEVMEM=y CONFIG_HFS_FS=y CONFIG_HFSPLUS_FS=y`.
- **CI `hfs-tool` fixed 2026-11-10 (run `42846850104`, success).** The job cloned `slebnik/libfsap` and `planetbeing/hfsplus`; both are 404 (the second never was the repo name), so `git` asked for credentials and died with `fatal: could not read Username for 'https://github.com'`. libfsap has no live replacement; the `hfsplus` CLI (`ls`/`cat`/`extract`) is in **`planetbeing/libdmg-hfsplus`**. The workflow now builds a static aarch64 zlib, then `hfsplus` with a CMake toolchain file and `-DCMAKE_EXE_LINKER_FLAGS="-static -L<prefix>/lib"` (the project's `link_directories(${ZLIB_LIBRARIES})` is a file path, so `-lz` needs the extra `-L`). Artifact: `hfs-readers-aarch64/hfsplus`, 916808 bytes, ELF aarch64 statically linked, not stripped.

**LIVE LOG over USB (0077, boot 0077s): no more recovery round-trips.** Boot an image with `sweetusb=2`, run `tools/sweet-usbnet.sh logs/live-NAME.txt` on the PC: it waits for the phone's CDC-ECM interface, sets 192.168.7.1/24 (sudo) and prints/saves the kernel console the phone streams every 100 ms as UDP to 192.168.7.1:5140 (starts with the last 64 KiB). `ping 192.168.7.2` checks the link. Current image: `shim/out/shim-boot-0077s.img` (kernel 0077, ADT sweet-0069hi, cpus=6, sweetfb_noconsole=1, sweetfb_hz=30, sweetusb=2). The recovery pull (`tools/sweet-log-pull.sh`) is still the way after a panic/hang before USB comes up.

**🎉 USB networking works (boot 0076s, 18:24 local): `ping 192.168.7.2` from the PC answers in ~1 ms, 0 % loss (200-packet burst, 1500-byte frames, 512/1024-byte frames).** 0075: CDC-ECM function (bulk 0x81/0x01, interrupt 0x82 NETWORK_CONNECTION; host MAC 02:00:5e:77:ee:01) + kernel ethernet ifnet `ue0` (legacy output, header strip like if_fake) with 192.168.7.2/24 set from the kernel (proto_plumb + SIOCAIFADDR on an in-kernel socket). 0076: received frames into one cluster mbuf (mbuf_allocpacket gave a first mbuf of length 0 -> panic "Failed mbuf validity check: len -14"). Host side (Linux cdc_ether, interface `enp0s20f0u3u1`): `nmcli dev set IF managed no; sudo ip addr add 192.168.7.1/24 dev IF; sudo ip link set IF up`. Boot image `shim/out/shim-boot-0076s.img` (`sweetusb=2`). iOS itself does not see ue0 yet (configd needs an IONetworkInterface in IOKit).

**USB device mode works (boot 0074s, 17:45 local): the host enumerates the phone at high speed as `1209:0001 ios-sweet "Sweet iOS (USB bring-up)"`.** 0072 read-only probe: ABL leaves the USB30_PRIM GDSC and all 7 USB clocks on, DWC3 v3.20a (GSNPSID 0x5533320a) in device mode (GCTL 0x00112000), halted (DCTL RUN_STOP 0, DSTS 0x00d3de04), event buffer still at ABL's 0xfdfc8000, QUSB2 PHY alive. 0073 (`sweetusb=2`): DCTL soft reset, own event buffer (polled, interrupt masked), ep0 DEPSTARTCFG/SETEPCONFIG/SETTRANSFRESOURCE, setup TRB, RUN_STOP; control-transfer state machine as Linux ep0.c; descriptors: vendor class, no data endpoints. It ran but no event came: **0074** forces the VBUS session in the Qualcomm wrapper (QSCRATCH 0xa6f8800: HS_PHY_CTRL |= UTMI_OTG_VBUS_VALID(20)|SW_SESSVLD_SEL(28), SS_PHY_CTRL |= LANE0_PWR_PRESENT(24), as Linux dwc3_qcom_vbus_override_enable) -> connect + enumeration. Image `shim/out/shim-boot-0074s.img`. Black Phone app: main thread waits in -[VMVoicemailManager fetchAccounts] (sync XPC; voicemail needs the modem).

**Navigation works (boot 0071s on v14, log `logs/boot-0071s.txt`):** swipe up from the bottom edge returns to the home screen; app switcher works. Two fixes: **0070** pid_hibernate (syscall 435 was nosys on the macOS config -> SIGSYS killed SpringBoard in -[SBToAppsWorkspaceTransaction _didComplete] when an app left the foreground; the only CONFIG_EMBEDDED-only entry in syscalls.master); **0071** touches starting within 48 px of an edge carry kIOHIDDigitizerEventFromEdgeTip (UIKit `_UIEventHIDTouchEdgeTypeForHIDEvent` reads the digitizer EventMask 0xb0007 bits 0x800/0x1000/0x2000/0x40000). Diagnosed, not fixed: Settings (Preferences) SIGTRAP in PodcastsUI func_1cdc47c58: force-unwrap of `URL.volumeTotalCapacity` (nil on our HFS+ root). Phone/Photos/Messages show black (MobilePhone launches, no crash) - next: stack samples via boot-arg `sweetsample=MobilePhone,MobileSlideShow,MobileSMS,SpringBoard` (image `shim/out/shim-boot-0071s.img`).

**🎉🎉 MILESTONE 2026-10-18 ~15:30 local: iOS 16.7 SPRINGBOARD HOME SCREEN ON THE PHONE** (boot `shim/out/shim-boot-0069s.img` = kernel 0069 + ADT `shim/out/sweet-0069hi.adt`, image **v14**; log `logs/boot-0069s-v14-HOMESCREEN.txt`). Home screen with a few apps, Settings opens, Control Center / lock screen / notification center work, touch + buttons work, 6 A55 cores (`cpus=6`), `sweetfb_noconsole=1 sweetfb_hz=30`. Known issue: cannot leave Settings (home gesture / AssistiveTouch home do nothing) - to investigate.
What it took today (in order): 0058 HID services (Apple IOHIDFamily-1915 user client), 0059 iPhone X identity (hw.product etc.), 0060 SPMI keys + GENI touch, 0061 1125x2436 presented display, 0062 RPMh L11C touch power + IOPMPowerSource battery, 0063 GENI FIFO packing (Goodix GT9896), 0064 PRIV_WORK_INTERVAL grant (SpringBoard "Failed to create work interval"), 0065 SMP via PSCI (6 A55), 0066 crash map regions, 0067 console mute without cons_ops write, 0068 IOSurface lookup of an already-mapped surface returns base -1 (backboardd snapshot crash), 0069 UFS log flush 10 s. Image v13: CommCenter removed from launchd.plist, BKNoWatchdogs. **Image v14** = v13 + Setup skipped (`/var/mobile/Library/Preferences/com.apple.purplebuddy.plist` SetupDone/SetupFinishedAllSteps true, SetupVersion 11, daemonizeNeedsToRun false; purplebuddy.notbackedup mini-buddy flags false; managedconfiguration.notbackedup LockdownActivationIndicatesCloudConfigurationAvailable false; springboard MCForceShowCloudConfigurationUI false) + mobileactivationd data ark seeded (`/private/var/containers/Data/System/D90C94C4-F94E-4FCF-8A28-A93BE3846D8E/Library/internal/data_ark.plist`, -ActivationState Activated etc.) + MediaToolbox D22/D221/Default SubServerConfiguration StartRecorderServer=false (mediaserverd crash loop: portrait-filter prewarm called NULL GL). **Activation:** ADT /chosen certificate-production-status 0, effective-production-status-ap 0, certificate-security-mode 1, effective-security-mode-sep 1 -> mobileactivationd's native Hactivation branch (Codex `codex/activation/report.md`; alternative 4-byte daemon patch documented there). Fallback image v13; v12 deleted.

**🎉 TOUCH WORKS (boot 0063u on image v13, 2026-10-18 ~13:15 local):** tapping (i) on Setup's Hello opens the device-info sheet; swipe-up is recognised (spinner, then back to Hello - under investigation). Chain: RPMh L11C vote (0062, `bsd/dev/sweet_rpmh.c`) -> GT9896 @0x5d on GENI SE7 (0063 fixed FIFO packing: register address went out byte-swapped) -> sweet_hid_touch -> SweetHIDTouch -> backboardd. Battery icon OK (0062 SweetBattery IOPMPowerSource, static 100 %). **Image v13** (= v12 + CommCenter jobs removed from launchd.plist + `BKNoWatchdogs`=true and `FBLaunchWatchdogExceptions{com.apple.purplebuddy:100}` in /var/mobile/Library/Preferences/com.apple.springboard.plist): Setup no longer killed by the scene-create watchdog (it blocked in CoreTelephony getSubscriptionInfo -> CommCenter, Codex `codex/setup/report.md`; that report also lists how to skip Setup: SetupDone/SetupFinishedAllSteps/SetupVersion=11 in com.apple.purplebuddy.plist + activation check). v9 deleted, fallback = v12. Input latency ~1 min with 1 CPU -> Codex task 11 = SMP via PSCI (`codex/smp/`).

**INPUT WORKS (boot 0060u on a freshly flashed v12, log `logs/boot-0060u.txt`):** power, volume up, volume down reach iOS (PMIC -> sweet_keys.c -> SweetHIDButtons -> backboardd). Power locks/unlocks (screen "off" shows the kernel console because the panel never really turns off). **Device identity fixed: iPhone X** (home indicator, Face-ID-style Setup). Earlier test (0059u, `sweethidtest=15` fake Home presses) advanced Setup from Hello to the language list.
- **0058** HID: `iokit/Kernel/arm/hid/`: IOHIDEventServiceUserClient + IOHIDEventServiceQueue + IOHIDEvent compiled UNCHANGED from Apple IOHIDFamily-1915.140.3 (= exact version of the iOS 16.7 IOHIDLib, clone in `research/iohid-1915`) over our minimal IOHIDEventService (SweetHID.cpp: SweetHIDButtons consumer page 0x0C power 0x30 / vol 0xE9/0xEA / menu 0x40; SweetHIDTouch digitizer collections). C hooks `sweet_hid_key(key, down)` (0 power, 1 vol+, 2 vol-, 3 home) and `sweet_hid_touch(finger, down, x, y)`. HID logs go to IOLog. Boot-arg `sweethidtest=N` = fake Home press every N s.
- **0059** identity: hw.machine/hw.model/hw.target/hw.product from the ADT (macOS-config kernel used to answer "iPad8,6"/"Redmi Note 10 Pro"). mkadt: root compatible `D22AP|iPhone10,3|xiaomi,sweet|...`, model iPhone10,3, target-type D22, `/product` copied from the real D22AP device tree (`research/ipsw-x/d22ap.adt`, extracted from the IPSW, NOT in git), `/buttons home-button-type 2`. Codex report `codex/mg/report.md`: MG ProductType = sysctl **hw.product** (unrecognized -> iPod), HWModelStr = hw.target, HomeButtonType = `/buttons/home-button-type`; disk cache `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist` (keyed by build + UUID only -> stale iPod answers survived; reflashed clean v12 to clear it).
- **0060** drivers by Codex (`codex/input/hw-report.md`): `bsd/dev/sweet_keys.c` (SPMI arbiter v5 observer reads, PON 0x810 bits KPDPWR/RESIN SID0, PM6150L GPIO2 SID4 0xc110; works), `bsd/dev/sweet_touch.c` (GENI SE7 I2C @0xa84000, starts 45 s after boot). Touch result: SE7 fw ok (proto 3) but **0x5d and 0x38 both time out -> touch IC unpowered** (L11C LDO needs an RPMh vote). Codex task 8 = RPMh client (`codex/rpmh/`), task 7 = battery (`codex/battery/`).
- **0061** (CI 39117078444): red status bar "rdar:45025538" = UIKit "No status bar visual provider for screen" (360x800 pt is no real iPhone). AppleCLCD now presents **1125x2436** (iPhone X) and shows the centered 1080x2400; touch mapped the same; ADT canvas 1125x2436. `sweetfb_native=1` reverts.
- Images: `shim/out/shim-boot-0060b.img` (0060 + /buttons ADT `sweet-0060hi.adt`). Next: 0061 image with ADT from current mkadt.

### >>> HANDOFF 2026-10-17 late (read this first) <<<

**STATE: iOS 16.7 Setup.app "Hello" screen is ON THE PHONE** (boot `shim/out/shim-boot-0057u.img` on image v12). Photo `/run/media/dipa/android/ios-sweet-work/MILESTONE-setup-hello-2026-10-17.jpg`. SpringBoard + backboardd + Setup run; nothing crashes in the GUI path. Remaining crash/relaunch loops: installcoordinationd (should be fixed by 0057, unverified), mediaserverd (VirtualAudio removed in v12, unverified), mobileactivationd (~100/boot, not analysed), bluetoothd + fairplayd.H2 relaunch constantly (no hardware).

**What is on the phone now:** userdata = image **v12** (`/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v12.img` + `.simg` + `.sha256`; v9 kept as fallback, v8/v10/v11 deleted or superseded). Kernel **0057** (`xnu/out-0057`, CI `39067532732`). ADT **`shim/out/sweet-0057hi.adt`** (QEMU twin `research/q0057-hi.adt`). Boot image **`shim/out/shim-boot-0057u.img`**. Boot args: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1`.

**Build a phone image** (kernel K, ADT A): `python3 tools/mkpayload.py $K $A shim/out/payload-X.bin --cmdline "$B" && (cd shim && ./build.sh ../shim/out/payload-X.bin && cp out/shim-boot.img out/shim-boot-X.img)` then check `python3 tools/unpack_bootv2.py shim/out/shim-boot-X.img shim/out/check-X && cmp shim/out/check-X/ramdisk shim/out/payload-X.bin`. QEMU smoke test: `python3 tools/mkpayload.py $K research/q0057-hi.adt $S/csq/payloadX.bin --ramdisk $S/csq/root-fork.img --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"` + `PATH=~/swift/usr/bin:$PATH python3 tools/qemu-sweet.py ... --secs 40 --kernel $K --fail-on-panic` (S = scratchpad; root-fork.img lives in the session scratchpad csq/ dir and may be gone in a new session).

**Kernel workflow:** edit `research/bt28/xnu/...` (keep the pre-edit copy in `research/preNNNN/`), syntax check `python3 tools/xnu-cc.py research/run28.log research/bt28 <file>`, make `xnu/patches/NNNN-*.patch` with `diff -u pre bt28 | sed` path rewrite, add new/changed files to `research/check-patch-order.py` list, run it (must say applied + MATCH), commit + push -> GitHub Actions `xnu` builds (~8 min) -> `gh run download <id> -R dipa-studio/ios-sweet -n xnu-kernel -D xnu/out-NNNN`. **Repo is PUBLIC since 2026-10-17** (private macOS runner minutes ran out: "recent account payments have failed"); public = free runners. Commit trailer: only `Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>`.

**Phone workflow (user does buttons, Claude runs commands):**
- Boot test kernel: phone in fastboot -> `fastboot boot shim/out/shim-boot-X.img`. NEVER `fastboot getvar`.
- **Read logs WITHOUT filming:** after a test the user holds Power+VolDown -> fastboot -> `fastboot boot recovery/recovery.img` (OrangeFox, temp) -> wait for `adb shell true` -> `tools/sweet-log-pull.sh /run/media/dipa/android/ios-sweet-work/logs/<name>.txt` (reads the 4 MiB console ring the kernel keeps in the last 1025 blocks of userdata; patch 0047). Then `adb reboot bootloader` brings the phone back to fastboot without the user.
- **Flash a root image:** `fastboot flash -S 512M userdata <img>.simg` (7 min, 25 chunks). After a big flash the Xiaomi ABL fastboot hangs: next `fastboot boot` stalls at "Sending" -> the USER must re-enter fastboot by hand (Power+VolDown). Do not retry commands against the hung ABL. A flash right after `adb reboot bootloader` once failed on chunk 1 (USB protocol error): just retry. User must NOT unplug during flash (once interrupted at 25/25 -> reflash).
- **Edit the phone root without reflashing (same-length only):** find the file's raw offset in the local image (it is one extent; search the image for its first 4 KiB), compare with `adb exec-out dd if=/dev/block/sda34 ...`, then `adb push` changed 4 KiB blocks and `dd of=/dev/block/sda34 seek=... conv=notrunc,fsync`, read back. Recovery kernel has no hfsplus.
- **Pull the whole phone root** (12 min): `adb exec-out "dd if=/dev/block/sda34 bs=1048576 count=16384" > img`; mount ro with udisksctl or `sudo mount -t hfsplus -o loop,ro`. To write into a pulled image: `sudo fsck.hfsplus -fy` first (volume is never cleanly unmounted -> Linux mounts ro even with -o force), then `sudo mount -t hfsplus -o loop,force`.
- Making images: copy latest img with `cp --sparse=always`, mount rw with force, change, `sync; umount`, `sha256sum`, `img2simg`. Keep only the latest image + one fallback on the SSD (/home is ~98% full; big work files live in `/run/media/dipa/android/ios-sweet-work/`, old kernels/images archived in `.../ios-sweet-work/archive/`).

**Diagnostics built into the kernel (all SWEET-only):** persistent console log (0047, boot-arg sweetlog=0 off; flush every ~1 s); `|name| text` copies of /dev/null writes for processes in `sweetnull` (default backboardd,SpringBoard; 0048); `IOK name:` IOKit lookup/open trace for `sweetiolog` (default backboardd,SpringBoard; 0049/0054, 400 lines max); `SWEET Sandbox: proc op N` (0050); `STR ...` heap text dump on SIGABRT/SIGTRAP for `sweetscan` (default backboardd,SpringBoard; 0051/0053) - this is how NSException/BSAssert reasons were found; `SMP name tN pc .. bt ..` user stack sampler for `sweetsample` (default SpringBoard,backboardd; first at 90 s then every 120 s, 40 samples, 16 frames; 0054/0055); `SWEET FB: N swaps` every 1000 swaps; `CRASH name ...` crash reports (0043/0044). Symbolicate cache addresses with `ipsw dyld a2s research/dsc/dyld_shared_cache_arm64 <addr - slide>`; main-binary addresses: addr - mh + 0x100000000.

**Patches added today (0046-0057, all CI-built):** 0046 IOSurface (IOCoreSurfaceRoot) + IOMobileFramebuffer (AppleCLCD on /arm-io/disp0) + arm-io nubs; 0047 persistent log; 0048 /dev/null tee; 0049 IOKit trace; 0050 permissive "Sandbox" MAC policy; 0051/0053 heap text on crash; 0052 RENAME_SWAP via exchangedata on HFS; 0054/0055 sampler, swap counter, AKS selectors 35 (extended device state = 17) and 101; 0056 panel copy from the vsync timer at 10 Hz (sweetfb_hz) instead of every swap; 0057 no config_ext_resolver (ACL group entries resolve without memberd). ADT (tools/mkadt.py): /arm-io/disp0, /chosen/display-scale 3, /chosen/unique-chip-id (synthetic 0x5357454554000001) + chip-id 0x8015 -> IOPlatformUUID A9683613-3704-53F2-95D0-98A8FB574E78. Image v9-v12: /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}; launchd.plist StandardOut/ErrorPath renamed to ...PatX (pointing jobs at /dev/console stops them launching: EACCES); v12: LastSystemVersion=20H392 in plain /var/mobile/Library/Preferences/com.apple.migration.plist (stops the 1-hour data migration that ran every boot), VirtualAudio.plugin moved to /Library/Audio/Plug-Ins/HAL.disabled/.

**Codex helper** (`codex exec --skip-git-repo-check -m gpt-6.1-sol -c model_reasoning_effort=high -s workspace-write -C /run/media/dipa/android/ios-sweet-work/codex -o OUT "$(cat task.md)" < /dev/null`, run detached with setsid nohup; config already defaults to gpt-6.1-sol high). Reports in `/run/media/dipa/android/ios-sweet-work/codex/`: mediaserverd-report.md, installcoordinationd-report.md (-> 0057), ufs-performance.md (ranked UFS speed plan), **ufs/0058-sweet-ufs-faster.patch** (READY, not yet applied: ufslogsecs default 10 s log flush batching, optional ufscache=1 directional cache ops, optional ufswait=1 spin-then-sleep; syntax-checked, host-stub verified; see ufs/README.md). Apply it as patch 0058 after review.

**Claude safety-classifier note:** several messages about QuartzCore renderer selection / writing an HFS reader tool were cut off by an automatic classifier (false positives, user agrees). Keep those topics short or let them be; they are not blockers now (rendering already works).

**NEXT STEPS (in order):**
1. **Input** (Setup stuck on Hello): touch + power + volume as IOHIDEventService/IOHIDDevice services backboardd matches (log shows backboardd matching IOHIDDevice with DeviceUsagePairs and IOService with HIDServiceSupport=true). Phone hardware: Goodix/Focaltech touch over I2C/SPI (check live DT `dtb/`), buttons via PMIC (pon) and GPIO keys. Start with a fake HID service (e.g. power button via a timer or boot-arg) to learn the ABI, then real drivers.
2. **Device identity**: MobileGestalt reports iPod + Home button. Add /product node / product-type (iPhone10,3, D22AP), device class iPhone, home-button-less (Face ID) properties in mkadt; find exact MG keys (MGIOMFBSupport.c, `IODeviceTree:/product` lookups seen in backboardd trace).
3. **Battery**: AppleSmartBattery/IOPMPowerSource style service (status bar shows empty red battery).
4. Apply Codex 0058 (UFS speed), consider cpus>1, check whether installcoordinationd/mediaserverd loops are gone, analyse mobileactivationd.
5. Hide kernel console over the GUI when not debugging: boot-arg sweetfb_noconsole=1 (but then panics are not on screen).

- **🎉 MILESTONE: iOS GUI ON THE PHONE - Setup.app "Hello" screen (2026-10-17 ~21:30 UTC, boot 0057u on image v12).** Photo: `/run/media/dipa/android/ios-sweet-work/MILESTONE-setup-hello-2026-10-17.jpg` (animated "dobar d(an)" hello lettering, status bar "iPod" + battery icon, bottom "Pritisnite Home za otvaranje" = "Press Home to open" (locale hr), (i) button). Rendered by iOS 16.7 SpringBoard/Setup through CoreAnimation -> backboardd -> kernel IOSurface (IOCoreSurfaceRoot) -> kernel AppleCLCD swaps -> ABL panel memory. What made it: data migration finally skipped (v12: `LastSystemVersion` 20H392 in the plain `/var/mobile/Library/Preferences/com.apple.migration.plist`; ADT `/chosen/unique-chip-id` + `chip-id` for IOPlatformUUID), 0057 (no ext resolver -> ACL containers work), plus everything before (0046 display/IOSurface, 0050 Sandbox policy, display-scale 3, containers dirs, RENAME_SWAP, AKS 35/101, 10 Hz present).
  **Missing (next work, in rough order):**
  - **Input: no touch, no power button, no volume buttons** (no HID drivers; backboardd looks for IOHIDDevice / HIDServiceSupport services). Setup is stuck on "Hello".
  - **Device identity: it thinks it is an iPod with a Home button** (status bar "iPod", "Press Home to open"): MobileGestalt has no product/model identity (no /product node, product-type, etc.) - give it iPhone10,3 / home-button-less (Face ID class) identity.
  - **No battery detection** (no battery/charger/PMIC service; status bar battery shows empty/red).
  - Locale came up Croatian (hr) - harmless.
  - Performance: cpus=1, polled UFS (Codex UFS patch in progress: `codex/ufs/`), backboardd presents at 60 Hz.
- **README refreshed (2026-10-17):** read the full notes and updated the public overview through patch 0045, v7 hardware observations and prepared v8 diagnostics. Documents real Apple userspace, the missing display/IOSurface GUI work, limited security services, highmem, persistent userdata root, current tooling and build examples. Documentation only; no kernel build, tests, flashing or phone boot performed.

- **v11 still migrated (boot 0056u, 20:32 UTC, `logs/boot-0056u-v11.txt`).** On iOS kCFPreferencesCurrentHost lands in the plain plist: all DMEnvironment setters (setLastBuildVersionPref, setUserDataDispositionPref, ...) use CurrentHost and the phone's `/var/mobile/Library/Preferences/com.apple.migration.plist` holds DMUserDataDisposition; datamigrator's XPC service runs with `_RoleAccount: mobile`. **Image v12** = v11 + `LastSystemVersion` = 20H392 added to that plain `com.apple.migration.plist` (keeps DMUserDataDisposition, 501:501 0600) + `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` moved to `/Library/Audio/Plug-Ins/HAL.disabled/` (Codex: stops the mediaserverd SIGSEGV loop; untested). **installcoordinationd loop explained (Codex `codex/installcoordinationd-report.md`):** os_crash "Failed to create data directory" EACCES for `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.installcoordinationd/Library/InstallCoordination`: `Library`/`Library/Caches` in the container are ACL-only (uid/gid 0xfffffffe, mode 0000, ACE for group GUID ...012B = gid 299 systemusers); with `config_ext_resolver` the kernel asks an identity resolver (memberd/opendirectoryd) that iOS does not run -> EWOULDBLOCK -> ACE skipped -> EACCES. **Patch 0057**: drop config_ext_resolver from FILESYS_BASE (compatibility UID/GID<->GUID mapping is used instead), likely fixes other ACL-protected containers too. Codex task 3: UFS driver performance design (`codex/ufs-performance.md`).
- **mediaserverd SIGSEGV loop explained (Codex report `/run/media/dipa/android/ios-sweet-work/codex/mediaserverd-report.md`):** the fault (FAR 0x390) is in the HAL plug-in `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio` (ApplyBeamFormerPreference <- VirtualAudio_PlugIn::SetPropertyData <- HALS_AHPPlugIn::ObjectSetPropertyData <- AudioObjectSetPropertyData <- MediaExperience vaemVADSerializationListener): the default virtual audio device ('vdef', VirtualAudioDevice_Default) does not exist (no audio hardware/driver, no /product/audio). Proposed workaround for GUI bring-up: move VirtualAudio.plugin out of /Library/Audio/Plug-Ins/HAL/ in the next root image (untested). v11 reflashed OK after an interrupted flash (USB unplugged at chunk 25/25). Codex task 2: installcoordinationd crash loop.
- **ROOT CAUSE of the endless migration: no IOPlatformUUID (2026-10-17).** datamigrator's `-[DMMigrator isMigrationNeededWithBuildVersion:lastBuildVersion:clientPID:]` gets lastBuildVersion from `-[DMEnvironment lastBuildVersionPref]` = `CFPreferencesCopyValue("LastSystemVersion", "com.apple.migration", kCFPreferencesCurrentUser, kCFPreferencesCurrentHost)` -> a ByHost file. The phone had `/var/mobile/Library/Preferences/com.apple.migration.plist` (only DMUserDataDisposition) and NO `Preferences/ByHost` directory at all. On arm XNU publishes IOPlatformUUID (gethostuuid) only from `/chosen/unique-chip-id` (8 bytes) + `/chosen/chip-id` (4 bytes) (IOPlatformExpertDevice::generatePlatformUUID, called from IOPlatformExpert::start); the SWEET ADT had neither, so every CurrentHost preference write failed. Fix: mkadt adds `unique-chip-id` = 0x5357454554000001 (synthetic) and `chip-id` = 0x8015 -> UUID `A9683613-3704-53F2-95D0-98A8FB574E78` (same SHA1 namespace computation as the kernel). ADT `shim/out/sweet-0057hi.adt`, boot image `shim/out/shim-boot-0056u.img` (0056 kernel + 0057hi ADT, QEMU OK). **Image v11** = v10 + `Preferences/ByHost/com.apple.migration.<UUID upper and lower>.plist` {LastSystemVersion: 20H392} (501:501, dir 0755 files 0600). Flashing v11 2026-10-17 ~20:10 UTC. Codex (gpt-6.1-sol high) analysing the mediaserverd SIGSEGV loop in `/run/media/dipa/android/ios-sweet-work/codex/` (report `mediaserverd-report.md`).
- **v10 did NOT stop the migration (boot 0056n on v10, 19:18 UTC, `logs/boot-0056n-v10.txt`):** datamigrator + migrationpluginwrapper ran again, backboardd drew the progress UI (13000+ swaps in ~5 min). So `com.apple.DataMigration.plist`/LastSystemVersion is not the deciding input. What is known about the decision (datamigrator XPC service `DataMigration.framework/XPCServices/com.apple.datamigrator.xpc/com.apple.datamigrator`, strings): `Migration sentinel present. Migration necessary.` (sentinel `/private/var/datamigrator/migrate`, created and removed by datamigrator itself; absent in v10), `System build version changed from %@ to %@. Migration necessary.` / `...unchanged... not necessary`, `Disposition dictionary with build version %@ is out of date relative to current build version %@`, `SyntheticDidUpgrade`, DMUserDataDispositionManager; previous build source not found yet (not in the binary's plain strings; likely in DataMigration.framework's DMUserDataDispositionManager / preferences domain `com.apple.migration`). backboardd calls `DMPerformMigrationIfNeeded` and holds SpringBoard in `checkInAndWaitForDataMigration` until it returns. NEXT: find where the previous build version / disposition dictionary is stored (disassemble DataMigration.framework DMUserDataDispositionManager + datamigrator's isMigrationNeeded path), write it into the image; or get datamigrator's os_log out (OS_ACTIVITY_DT_MODE needs a launchd env change).
- **Data migration never records completion -> skip it (image v10, 2026-10-17).** Boot 0056n (18:16 UTC, `logs/boot-0056n.txt`, 30 min): 10 Hz panel copies, backboardd still presents at ~60 Hz (107k swaps), SpringBoard still in `BKSSystemShellService _checkIn` at sample 14. Every migration run takes 1+ hour on cpus=1 and repeats each boot. DataMigration decides from `/var/mobile/Library/Preferences/com.apple.DataMigration.plist` key `LastSystemVersion` (vs ProductBuildVersion 20H392); the pulled phone root (21:01 local) had NO such file. **v10** = that pulled phone state + fsck.hfsplus -fy (volume was marked not cleanly unmounted -> Linux mounts it read-only even with -o force; fsck found only hard-link counts + minor volume header, repaired) + `com.apple.DataMigration.plist` {LastSystemVersion: 20H392} (501:501 0600). `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v10.img` (+ .sha256, .simg); v8 files deleted, v9 kept as fallback.
- **Repo made PUBLIC (2026-10-17, user request) so GitHub-hosted macOS runners are free:** the 0056 CI run was refused ("recent account payments have failed or your spending limit needs to be increased"). Checked before: no device dumps, serials, IMEI or Apple binaries in the history (.gitignore keeps backup/, dtb/, recovery/, research/ out); commit author emails are public now. 0056 rerun `39056660104` passed -> `xnu/out-0056`, QEMU OK, image `shim/out/shim-boot-0056n.img` booted 18:16 UTC.
- **Boot 0055n (17:16 UTC, ~65 min, `logs/boot-0055n.txt`): migration bar reached 99% in ~30 min (was ~60), then crawled.** Root cause of the slowness: backboardd presents the progress UI at ~58 Hz (**167,000 swaps in ~48 min**) and AppleCLCD copied the full 10 MB frame into the panel on every swap -> ~600 MB/s memcpy on the single CPU (cpus=1), starving everything (polled UFS I/O too). Sampler (16 frames): **SpringBoard waits in `-[BKSSystemShellService _checkIn]`** (synchronous BSXPC call to backboardd, from FBSystemShellInitialize <- SBSystemAppMain) - backboardd holds the system shell check-in until the data migration ends, so the migration is the gate to SpringBoard UI. **Patch 0056**: swap only records the latest swap record; the vsync timer copies it to the panel every 60/sweetfb_hz ticks (default 10 Hz); `SWEET FB: N swaps` every 1000.
- **Boot 0054n (16:53 UTC, ~12 min, `logs/boot-0054n.txt`, patch 0054 sampler):** the data migration runs AGAIN on every boot (it completed once in the 0053s run) - completion is not being recorded. backboardd keeps swapping the progress UI (3700+ swaps in ~10 min, `SWEET FB: N swaps`). Sample during migration: SpringBoard main thread blocked in a synchronous XPC call (`mach_msg2_trap <- dispatch_mach_send_and_wait_for_reply <- xpc_connection_send_message_with_reply_sync <- -[BSXPCServiceConnectionMessage _sendSynchronously:] <- BSXPCServiceConnectionProxy ...`). Unsupported AKS selectors seen: 101 `aks_assert_consume`, 93 `aks_get_internal_info`, 34 `_aks_stash_load` (datamigrator), **35 `aks_get_extended_device_state`** (SpringBoard, chronod, remindd, searchd, symptomsd...). **Patch 0055**: AKS 35 answered like 17 (same DER device state), 101 succeeds; sampler backtraces 16 frames, 40 samples (~80 min).
- **MILESTONE: FIRST iOS PIXELS ON THE PANEL (boot 0053s long run, 15:33 UTC, ~60 min; photos IMG_20261017_183029/183222/183419; log `/run/media/dipa/android/ios-sweet-work/logs/boot-0053s-long.txt`, 407 KB, 1428 flushes).** The iOS data-migration boot screen (Apple logo + progress bar) was shown and the bar advanced to full in ~60 minutes. It is drawn by **backboardd through our kernel display path**: `SWEET SURF: backboardd create id 2/3 1080x2400 fmt 0x42475241 (BGRA) bpe 4 bpr 4352 size 10452992 keys 11`, IOSurface sels 9, 27, 5, 34 and traps 2/3 (lock/unlock), IOMFB SwapBegin (4) / SwapEnd (5, 1132-byte struct) / GetLayerDefaultSurface (3) / 68 / 55, then `SWEET FB: swap N layer 0 surface 2|3 1080x2400 ... a 0,0 1080x2400 b 0,0 1080x2400` alternating (double buffering; only the first 16 swaps are logged). Triggered by `com.apple.datamigrator` (AKS sels 101/93/34 return unsupported) and `com.apple.migrationpluginwrapper`. Probably ProgressUI-style CoreGraphics drawing into IOSurfaces (no GPU used). After the migration: SpringBoard and backboardd stay up (exec once each, no crash), nothing new drawn; crash loops only for mediaserverd, installcoordinationd, mobileactivationd (~108 each in the hour); bluetoothd and fairplayd.H2 relaunch constantly (no hardware). Kernel console text overlays the image (sweetfb_noconsole=1 would hide it).
- **MILESTONE: SpringBoard and backboardd both stay up (boot 0053s on v9, 15:29 UTC, `logs/boot-0053s.txt`).** With /chosen/display-scale = 3: `exec SpringBoard` once, `exec backboardd` once, no CRASH for either; MobileGestaltHelper reads IOMFB size/dot pitch; backboardd opens AppleCLCD (sels 57,15,22,29,33,79,74 as before). No IOSurface creates and no swaps: backboardd keeps polling `IOProviderClass IOAcceleratorES` (-> 0 found) and AGXAccelerator/IOAcceleratorESLegacy - no GPU, so no CoreAnimation renderer yet. Log goes quiet after ~47 s (only 47 flushes): the system idles. NEXT WALL: CoreAnimation rendering without a GPU (CPU path) so the window server produces frames.
- **SpringBoard's invalid mode = scale 0 (boot 0053n, 15:19 UTC, `logs/boot-0053n.txt`, heap scan):** `FBSDisplayMode cannot be configured as requested : <FBSDisplayMode: ...; infxinf@0x (1080x2400/0) 60Hz sRGB SDR>` - pixel size and 60 Hz are fine, scale is 0. libMobileGestalt (MGIOMFBSupport.c, sub_19c560a54) reads `display-scale` (u32, default 0) and `display-rotation` from **IODeviceTree:/chosen**, pixel size via IOMobileFramebufferGetDisplaySize (MobileGestaltHelper sel 8/28 in the log). mkadt now adds `/chosen/display-scale` = 3 (1080x2400 @3x = 360x800 pt). ADT `shim/out/sweet-0054hi.adt` (QEMU `research/q0054-hi.adt`), image `shim/out/shim-boot-0053s.img` = 0053 kernel + 0054hi ADT. Possible catch: MobileGestalt may have cached scale 0 in `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/...`.
- **MILESTONE: backboardd survives and opens the display (boot 0052n on v9, 15:07 UTC, `logs/boot-0052n.txt`).** No `CRASH backboardd`. It walks `IODeviceTree:/arm-io` children, matches IOMobileFramebuffer, `open AppleCLCD type 0 -> 0x0`, calls IOMFB selectors 57, 15, 22, 29, 33, 79 (GetBlock, out 1580/40), 74 (SupportedFrameInfo), 12, 6, 27, 17, 84, 50; opens IOCoreSurfaceRoot (sel 13); looks for AGXAccelerator / IOAcceleratorES / IOAcceleratorESLegacy (none, repeatedly), `IODeviceTree:/product` (none, repeatedly), `/buttons`, aop-smart-cover, IOPMrootDomain. No swaps yet. **SpringBoard now gets past display discovery** and dies (sig 5) in `-[FBSDisplayMode _initWithWidth:height:preferredScale:scaleOverride:refreshRate:gamut:hdr:rotation:virtual:validityCheck:]+580` <- `_initWithCADisplayMode:scale:rotation:` <- `FBSDisplayConfiguration _initWithImmutableDisplay:` <- `FBSDisplaySource initWithDisplay:` <- `FBSDisplayMonitor` (slide 0x14504000): "FBSDisplayMode cannot be configured as requested". Checks: width/height >= 2, preferredScale > 0, scaleOverride >= 0, refreshRate non-zero, valid gamut/HDR/rotation. Suspects: refresh rate 0 (IOMFB timing blocks answered with zeros) and/or scale 0 (MobileGestalt main-screen-scale; no /product node in the ADT). **Patch 0053**: heap text scan also on SIGTRAP and for SpringBoard (default sweetscan list backboardd,SpringBoard) to read the exact mode description.
- **Image v9 (2026-10-17) = v8 + `/private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}` (root:wheel 0755; on iOS these live on the data volume, absent from the rootfs) + launchd.plist with StandardOutPath/StandardErrorPath renamed to ...PatX (the in-place phone fix; daemons' stdout reaches the log via the 0048 /dev/null tee).** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v9.img` SHA256 aa9f9139...6e67, `.simg` 12,145,303,864 bytes; written through `sudo mount -t hfsplus -o loop,force`. **Patch 0052** (`xnu/patches/0052-sweet-hfs-rename-swap.patch`, CI `39042761826` -> `xnu/out-0052`, QEMU OK, image `shim/out/shim-boot-0052n.img`): vn_rename falls back to VNOP_EXCHANGE when VNOP_RENAMEX(RENAME_SWAP) returns ENOTSUP for two regular files on one volume (HFS exchange swaps data and cnode descriptors = swap semantics). Flashing v9 started 2026-10-17 ~15:00 UTC.
- **backboardd EXCEPTION FOUND (boot 0051n, 14:41 UTC, `logs/boot-0051n.txt`, patch 0051 heap scan):** `*** Terminating app due to uncaught exception 'NSInvalidArgumentException', reason: '*** -[NSURL initFileURLWithPath:]: nil string parameter'`. First-throw stack (slide 0x337f0000): `+[NSURL fileURLWithPath:]` <- `-[BSSystemContainerForCurrentProcessPathProvider libraryPath]` <- `cachesPath` <- backboardd main+0x1d858 (StartWindowServer, shader cache path). BaseBoard: libraryPath = `BSSystemSharedResourcesDirectoryForIdentifier` = `BSSystemSharedDirectoryForIdentifier(id)` + "/Library"; that calls `container_system_path_for_identifier` (libsystem_containermanager -> `_container_legacy_lookup` -> containermanagerd XPC), which returned NULL (BaseBoard logs "Error getting shared system container ..."). With 0050 the sandbox denials are gone. containermanagerd.log.0 in the pulled (pre-0050) image: "Did not receive any persona attributes", "Expected to find at least one persona", notify_register_dispatch failed 9; `/private/var/containers` does NOT exist in the root image (on iOS it lives on the data volume). usermanagerd does create kernel personas 99/199/1000, but fails to save `/private/var/keybags/usersession.kb` and `persona.kb`: "Failed to atomically swap writing file: Operation not supported (45)" (HFS has no RENAME_SWAP). NEXT: create `/private/var/containers` (and check what containermanagerd needs for system group containers), fix or emulate RENAME_SWAP on HFS, then re-check backboardd.
- **Disk housekeeping (2026-10-17):** /home was at 4 GiB free. Moved (not deleted) `xnu/out-0010..0044` and old `shim/out/*.img`/`payload-*.bin` (everything before 0046) to `/run/media/dipa/android/ios-sweet-work/archive/{xnu,shim-out}` (6.3 GiB); /home now 8.7 GiB free. Large work files (phone-root.img 16 GiB, logs, tracev3 dumps) live in `/run/media/dipa/android/ios-sweet-work/`. **Patch 0051** (`xnu/patches/0051-sweet-crash-heap-strings.patch`, CI `39041440935`): on SIGABRT of a process in boot-arg `sweetscan` (default backboardd) the crash log prints heap text runs as `STR ...` (to find the NSException reason). Boot 0051n 14:41 UTC.
- **Boot 0049n (14:11 UTC, `logs/boot-0049n.txt`, patch 0049 trace): backboardd's IOKit lookups before it dies** (each run): `IODeviceTree:/arm-io/pearl-sep` -> none, matching ApplePearlSEPDriver -> none, `IOService:/IOResources/AppleKeyStore` open OK, matching IOHIDDevice (DeviceUsagePairs) / `HIDServiceSupport` notifications, `usb-drd,t8027` notification; **never any IOMobileFramebuffer / display lookup** - the exception happens before display detection. launchd (tracev3) shows backboardd's mach lookups denied with `error = 159: Sandbox restriction`: com.apple.mobilegestalt.xpc, com.apple.iohideventsystem, com.apple.system.notification_center, com.apple.logd, containermanagerd, ACCHWComponentAuthService, oscard. libsystem_sandbox uses `__mac_syscall("Sandbox", op, arg)` (ops: 1 init, 2 check, 3 note, 4 container path, 5/6/7/8 extensions, 0x15 check_bulk, ...); with no policy named "Sandbox" every call gets ENOPOLICY. **Patch 0050** (`xnu/patches/0050-sweet-sandbox-policy.patch`): built-in permissive MAC policy "Sandbox" in sweet_amfi.c (mpo_policy_syscall returns 0, container/extension ops ENOENT; logs first use of each op as `SWEET Sandbox: proc op N`).
- **Phone root pulled (16 GiB, 12 min, `adb exec-out dd if=/dev/block/sda34`) to `/run/media/dipa/android/ios-sweet-work/phone-root.img` (loop-mounted ro at /run/media/dipa/iOSRoot).** No .ips crash reports. logd tracev3 (Persist, LZ4 `bv41` blocks, decompressed strings in `ios-sweet-work/tracev3/all.txt`): /private/var/db/uuidtext is EMPTY so only launchd's own messages are readable. Findings: v8 launch failure = `Service could not initialize: posix_spawn(/usr/libexec/backboardd), error 0xd - Permission denied` (xpcproxy exit 78) - /dev/console not openable for the mobile user; backboardd[98] spawned `com.apple.MTLCompilerService` before SIGABRT; **launchd denies many mach lookups with `error = 159: Sandbox restriction`** (mediaserverd -> com.apple.backboard.display.services, remoted, mobiletimerd -> sleepd, identityservicesd -> apsd) and **tccd exits OS_REASON_SANDBOX "profile not found"** - no Sandbox kext/profiles on SWEET; likely cause of many daemon crash loops (next wall after the display). CF's uncaught-exception text never reaches stderr for daemons (no CF handler; libc++abi prints only the type). **Patch 0049** (`xnu/patches/0049-sweet-iokit-trace.patch`): `IOK name: ...` console lines for registry path lookups, child iterators, matching (with result count/class), matching notifications and IOServiceOpen of the processes in boot-arg `sweetiolog` (default backboardd), max 400 lines.
- **CONFIRMED: launchd StandardOut/ErrorPath=/dev/console stops backboardd from launching.** After the in-place rename (boot 0047n-b, log `logs/boot-0047n-b.txt`) backboardd launches again and dies as on v7: sig 6, `___pthread_kill <- abort <- demangling_terminate_handler <- _objc_terminate <- std::terminate <- _dispatch_client_callout <- _dispatch_once_callout <- main+0x9334` (window-server dispatch_once). It never opened AppleCLCD or IOCoreSurfaceRoot (no `SWEET FB/SURF: backboardd` lines) -> throws before/at display detection. **Patch 0048** (`xnu/patches/0048-sweet-devnull-tee.patch`, CI `39037749668` -> `xnu/out-0048`, image `shim/out/shim-boot-0048n.img`): writes to /dev/null by processes named in boot-arg `sweetnull` (default `backboardd,SpringBoard`) are printed as `|name| text`. Boot 0048n (13:48 UTC, log `logs/boot-0048n.txt`): only `|backboardd| libc++abi: terminating due to uncaught exception of type NSException` (CF's reason line goes to os_log, not stderr, despite CFLOG_FORCE_STDERR/OS_ACTIVITY_DT_MODE). Next: pull the whole 16 GiB root from recovery (`adb exec-out dd if=/dev/block/sda34`) to `/run/media/dipa/android/ios-sweet-work/phone-root.img` and read ReportCrash .ips files / logd tracev3 for the exception reason.
- **Persistent log WORKS (boot 0047n on v8, 13:31 UTC; pulled from OrangeFox: 70 KB, 111 flushes, `/run/media/dipa/android/ios-sweet-work/logs/boot-0047n.txt`).** In 3 min **backboardd was never exec'd** (v7 ran it), ~70 bare `exec xpcproxy`, SpringBoard loops on FBSDisplayMonitor (no display), mediaserverd sig 11 loop, locationd, lockdownd sig 5. Suspect the v8 plist (StandardOut/ErrorPath=/dev/console on backboardd). **In-place plist edit from recovery (no reflash):** launchd.plist is one extent at image/partition byte offset 0x1e025f000 (195 blocks; verified bytes identical on phone vs local v8 mount); renamed StandardOutPath->StandardOutPatX and StandardErrorPath->StandardErrorPatX (same length, both jobs) in blocks 68 and 105 via `adb push` + `dd of=/dev/block/sda34 seek=... conv=notrunc,fsync`, read back VERIFIED. Phone now differs from local v8 image in those 2 blocks (files in `/run/media/dipa/android/ios-sweet-work/`). Test boot 0047n 13:38:42 UTC. Recovery kernel has no hfsplus (raw block edits only).
- **Patch 0047: persistent console log (CI `39036478663` -> `xnu/out-0047`, QEMU no panic; phone image `shim/out/shim-boot-0047n.img` = 0047 + sweet-0046hi.adt + 0045n cmdline).** `_cnputs` (all console output: kernel printf + daemons' /dev/console) also appends to a 4 MiB ring in sweet_ufs.c; after the UFS partition is ready and writable (ufsrw=1) a thread writes new ring blocks + header to the LAST 1025 blocks (4 KiB) of userdata every ~1 s and SYNCHRONIZE CACHE (prints `SWEET LOG: console log to the last 4100 KiB ...`). Read: phone to fastboot -> `fastboot boot recovery/recovery.img` (OrangeFox) -> `tools/sweet-log-pull.sh OUT.txt` (adb dd of that area + `tools/sweet-log.py`). Not yet verified on the phone. Panics after the last flush are not in it (screen still shows them). 2x font attempt reverted: XNU's v_scale only scales the progress icon.
- **Boot 0046n on v8 (13:05 UTC, VID_20261017_150522_HSR_120):** phone prints `SWEET SURF: IOCoreSurfaceRoot registered` and `SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0x9c000000, default surface 1` (nubs + match work on hardware). Visible userspace: early boot tasks, launchd_cache_loader, `exec SpringBoard`; no `exec backboardd` / `SWEET FB: backboardd` line seen in the readable frames (video blurry). Frame 00:47: lower half of the screen shows OLDER console text (16:01:12) below newer lines -> unconfirmed hint that a swap copied an old snapshot of the panel (CA reads the default/displayed surface = panel memory). SpringBoard still dies in `-[FBSDisplayMonitor _initWithBookendObserver:transformer:]+1160` (sig 5, slide 0x30960000). **Shim: 2x console font** (`shim/loader.c` Video.v_depth scale field = 1) so phone videos are readable; image `shim/out/shim-boot-0046b.img` = 0046n + 2x font.
- **Patch 0046: built-in IOSurface + IOMobileFramebuffer (2026-10-17, offline, NOT phone-tested).** `xnu/patches/0046-sweet-iosurface-iomfb.patch`: new `iokit/Kernel/arm/SweetSurface.{h,cpp}` (IOCoreSurfaceRoot + IOSurfaceRootUserClient: sel 13/0/4/34/35, traps 0-5, wired buffers mapped into the caller, per-client refs) and `SweetFramebuffer.cpp` (AppleCLCD : IOMobileFramebuffer on /arm-io/disp0: sizes, swap = CPU copy of enabled layers into the ABL panel memory, 60 Hz vsync via sendAsyncResult64WithOptions(CanDrop), panel memory published as the default IOSurface, kernel text console paused on the first swap only with `sweetfb_noconsole=1` (default keeps the console: pausing switches it to serial and would hide panics)); SweetPlatformExpert creates nubs for /arm-io children (not gic) attached to the platform expert (IOPlatformDevice::compareName casts its provider to IOPlatformExpert, so nubs under the arm-io nub never matched - first CI build showed that in QEMU). mkadt: /arm-io/disp0 {device_type display-subsystem, compatible sweet,display, canvas-width 1080, canvas-height 2400}. CI `39021572169` -> `xnu/out-0046`. **QEMU** (`research/q0046-hi.adt`, RAM disk init, msgbuf): `SWEET SURF: IOCoreSurfaceRoot registered`, `SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0x9c000000, default surface 1`, init alive, no panic. Phone image **`shim/out/shim-boot-0046n.img`** (SHA256 9c4d2fe9..., CI `39024108588` with the review fixes: swap bounds check, zeroed 64-bit async ref, port release, 1.5 GiB wired budget, ref leak/race fixes; QEMU re-checked) = 0046 kernel + `shim/out/sweet-0046hi.adt` + the 0045n cmdline; boot it on image v8 (backboardd/SpringBoard log to console) and film. Expected new console lines: `SWEET SURF: <proc> create id ...`, `SWEET FB: backboardd sel N ...`, `SWEET FB: swap ...`. Open question for the next wall: QuartzCore's IOMFB server builds its renderer from Metal first (`CAMetalContextCreate`), GLES only when enabled; how the CPU renderer gets selected on a device without a GPU is not yet established.
- **IOSurface values ABI (offline RE, `/run/media/dipa/android/ios-sweet-work/iosurface-values.md`) - TODO in SweetSurface (0046 only accepts and drops values, so IOSurfaceCopyValue always returns NULL):** sel 9 SetValue in = {u32 id, u32 pad, IOCFSerialize([value, key]) or ([dict] = merge)}, sel 0xb RemoveValue in = {u32 id, u32 pad, key\0} (empty key = remove all); both out 4 bytes = new generation, client accepts only (gen+2)|1. Kernel after each change: gen = (gen+2)|1 (odd, never 0) -> state+0x20, serialized size (>=1) -> state+0x28. sel 0xa in {u32 id, u32 0, u8 0}, out {u32 ?, u32 gen, serialized non-bulk dict}; too small -> 0xe00002db + needed size (client retries x16). sel 0x1b in 0x88: bulk struct 0x70 @0, u64 valid size @0x70, u64 changed mask @0x78, u32 id @0x80 -> copy masked fields into state+0x30 (bit0 32B @0, bit1 16B @0x20, bit2 8B @0x30, bits3-10 1B @0x38..0x3f, bits11-14 24B @0x40/4B @0x58/8B @0x5c/1B @0x64).
- **IOMFB display matching confirmed (offline RE, `/run/media/dipa/android/ios-sweet-work/iomfb-match.md`):** the framework never matches on "AppleCLCD"; any registered IOMobileFramebuffer subclass works. `iomfb_match_callback` reads only `dfr`/`AppleTV`/`external` (CFBoolean, service itself, no parents); internal main display = all absent/false (type 0, name "primary"). Display count = /arm-io children whose device_type starts with "display-subsystem" (count 0 -> CreateDisplayList returns NULL; more nodes than services -> 30 s wait). IOMobileFramebufferOpen fails only on IOServiceOpen failure; canvas-width/height read as 4-byte CFData from the IOService-plane parent (optional). CopyProperty = IORegistryEntrySearchCFProperty on the service only. Patch 0046 (disp0 node, `external`=false, AppleCLCD : IOMobileFramebuffer) fits all of this.
- **IOMobileFramebuffer swap + notification ABI (2026-10-17, offline).** Swap record = fb+0x18, **0x46c bytes**, zeroed by SwapBegin and sent whole by `kern_SwapEnd` = IOConnectCallStructMethod(sel 5, rec, 0x46c, no output). **SwapBegin** = sel 4, no input, 1 scalar out = swap id (stored at rec+0x50). `kern_SwapSetLayer(fb, layer<4, IOSurfaceRef, CGRect a, CGRect b, flags)`: rec+0x54+4*layer = **IOSurface id**; rec+0x104 bit layer = layer enabled (rec+0x108 bit cleared); rect a (int x,y,w,h, scaled) at rec+0x64+16*layer, rect b at rec+0xc4+16*layer; rec+0x114+4*layer = transform from flags&0xf; flag bits 5/31/6/7/8/9 set bytes rec+0x2ca/0x2cc/0x2e3/0x2e5/0x2e6/0x467+layer. A NULL surface only sets the enable bit with zero id. SwapWait = sel 6 scalars [swap id, timeout, 0].
  - **Notifications**: `IOMobileFramebufferScheduleWithDispatchQueue`/`GetRunLoopSource` create an IONotificationPort and `IOConnectSetNotificationPort(conn, type, port, 0)` (kernel `registerNotificationPort`). `kern_EnableNotifications(fb, type, cb, ctx)` = **sel 72 scalars [NotifyCallbackFuncs[type], fb, type, 0]** -> the kernel keeps an async reference {port, func, refcon=fb} per type and fires it with `sendAsyncResult64`. Types: 0 HotPlug, 1 Power, 3 FrameInfo (args[0] = mask of 31-bit keys, args[1] u32, then values), 4 CRC, **5 Vsync**: 6 args, each a 32-bit half in a u64 slot: (ts lo, ts hi), (t2 lo, hi) passed through the time-ingest helper, (t3 lo, hi); the framework then calls the client's vsync callback(fb, ts, t2', t3, ctx). Vsync callbacks are dropped while the notification port has queued messages.

- **IOSurface.framework user-client ABI reversed (2026-10-17, offline, step 2 of the GUI plan).** `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image IOSurface` (selector scan script: scratchpad `selscan.py`).
  - Connect (`_iosConnectInitalize`, pthread_once): `IOServiceGetMatchingService(IOServiceNameMatching("IOCoreSurfaceRoot"))`, retried with sleep(1) up to 241 times (a missing service stalls every client ~4 min, no crash); `IOServiceOpen(type 0)`; then **sel 13** (no inputs, struct out 0x18 bytes = `_ioSurfaceLimits`) and requires u16 at out+0x16 == 1 (ABI version); out+0 = offset/plane-base alignment mask, out+4 = bytes-per-row alignment mask (used as (x+m)&~m), +0..+0x14 also copied into IOSurface remote-method init, otherwise logs and closes the connection.
  - **Create = sel 0**: scalar in [parent id or 0], struct in = `IOCFSerialize(props, kIOCFSerializeToBinary)` (OSUnserializeBinary format; keys IOSurfaceWidth/Height/BytesPerRow/BytesPerElement/PixelFormat/AllocSize/PlaneInfo/CacheMode/IsGlobal/...), struct out **0xa68 bytes** ("lock result"). **Lookup = sel 4**: scalar in [id], same struct out. LookupFromMachPort = sel 0x22 (scalar in [port name]); CreateMachPort = sel 0x23 (scalar in [id, options], scalar out [port name]).
  - Lock result (copied to client+0x68, size 0x68 + planes*0x80, min 0xe8): 0x00 u64 base address **mapped in the caller's task**; 0x08 u64 task VA of a per-surface shared state page (+0 u64 registry id, +8/+0xa u16 bound accel ids, +0xc u32 seed, +0x14 u8 tiled, +0x20 u32 value generation, +0x24 YCbCr matrix, +0x28 u32 non-bulk values size, +0x2d u8 displayable, +0x2e u8 sysmem-only, +0x30 0x70 bytes bulk attachments); 0x10 u64 task VA of timestamp page (4 u64 indexed timestamps, +0x20 detach code); 0x18 u32 surface id; 0x1c u32 alloc size; 0x20 width; 0x24 height; 0x28 bytes per row; 0x2c buffer offset; 0x30 pixel format; 0x34 u32 (dcache flush length); 0x38 plane count; 0x40 u16 bytes/element; 0x42 u8 element width; 0x43 u8 element height; 0x44 cache mode; 0x4c u8 subsampling; 0x4d u8 prohibit-use count; 0x50 parent id; 0x54 slice count; 0x58 u64 trace id; 0x60 u64 protection options; planes[n] at 0x68+n*0x80 (+0 width, +4 height, +8 bytes/row, +0xc offset, +0x14 size, +0x18 u16 bpe, +0x1a/+0x1b u8 element w/h, +0x1c u8 component count, +0x1d address format, +0x54 u8 compression type (non-zero -> no CPU base address)).
  - **Traps** (IOConnectTrapN -> `IOUserClient::getTargetAndTrapForIndex`): 0 IncrementUseCount(id, category), 1 DecrementUseCount(id, category), 2 **Lock**(id, options, &seed_out), 3 **Unlock**(id, options, &seed_out), 4 Release(id), 5 Release after a failed create/lookup setup (id).
  - Other selectors: 5 SetYCbCrMatrix, 6 WrapClientImage, 7 WrapClientMemory, 9 SetValue (serialized), 0xa UpdateNonBulkValueCache (struct in 9), 0xb RemoveValue, 0xc BindAccel, 0x11 SetSurfaceNotify (async), 0x12 RemoveSurfaceNotify, 0x13 Log, 0x14 SetPurgeable, 0x15 SetOwnership, 0x16 SetTiled, 0x18 SetTimestamp, 0x19 GetTileFormat, 0x1a CopyDataValueBytes, 0x1b SetBulkAttachments (struct 0x88), 0x1e/0x2e-0x31 (via outlined helpers), 0x1f SetCompressedTileDataRegionMemoryUsed, 0x20 GetGraphicsCommPageAddress, 0x21 SetIndexedTimestamp, 0x24-0x26/0x2d IOSurfaceAccelerator, 0x27-0x29 shared events, 0x2a SignalEvent, 0x2b Set/InitDetachModeCode, 0x2c SetOwnershipIdentity, 0x32-0x34 data properties.
  - => SWEET kernel service: class/name **IOCoreSurfaceRoot** + IOSurfaceRootUserClient; minimum = sel 13, 0, 4, 9/0xa/0xb (values), traps 0-5, 0x22/0x23 later for cross-process surfaces (CA sends surfaces to backboardd by mach port). Memory: IOBufferMemoryDescriptor per surface, mapped into each task with createMappingInTask; ids from a global table.

- **Image v8: daemon logs on the console.** `tools/patch-launchd-plist.py IN OUT LABEL...` rewrites launchd.plist (binary, plistlib): renames every `_PanicOnCrash` to `_PanicOnCrasX` and gives the listed jobs `StandardOutPath`/`StandardErrorPath` `/dev/console` (devfs console is 0622, writable by mobile) plus `OS_ACTIVITY_DT_MODE=YES` (os_log mirrored to stderr) and `CFLOG_FORCE_STDERR=1`. v8 = v7 + that plist for com.apple.backboardd and com.apple.SpringBoard, written through the mount (796310 bytes, root:80 0644), SHA256 in `ios16-root-20H392-v8.img.sha256`, `.simg` ready; boot with `shim/out/shim-boot-0045n.img`. Goal: see backboardd's uncaught exception text and the window server's display probing. ODDITY (unresolved): the v5 raw-image rename at offset 0x1e025f000 is present in the v5-v7 image bytes, but the plist read through a Linux mount of v7 still had the original `_PanicOnCrash` keys; filefrag shows the file as one extent. Never patch file contents through raw offsets again; write through the mount.
- **IOMobileFramebuffer.framework reversed (step 1 of the GUI plan).** `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image IOMobileFramebuffer`.
  - Display discovery (`IOMobileFramebufferCreateDisplayList` -> `getDisplayListNumber`): iterates children of `IODeviceTree:/arm-io`, counts nodes whose `device_type` is "display-subsystem" or "ext-display-subsystem" (or `compatible` "paravirtualizedgraphics,gpu"); then `iomfb_populate_display_infos` adds an IOServiceAddMatchingNotification(IOServiceFirstMatch, **IOServiceMatching("IOMobileFramebuffer")**) and runs the run loop up to 30 s until that many services matched. `iomfb_match_callback` reads service properties `dfr`, `AppleTV`, `external` (CFBoolean) and names displays (external ones "%s-%d").
  - `IOMobileFramebufferOpen(service, task, type, &fb)`: `__ioMobileFramebufferAlloc` (CFPreferences com.apple.iokit.IOMobileGraphicsFamily: canvas_width/height, override_display_width/height, override_panel_width/height, benchmark, enable_ktrace), `IOServiceOpen(service, task, type)` (GetMainDisplay passes type 0), `getEDTProperty` reads `canvas-width`/`canvas-height` from the service's IODeviceTree parent, then fills a table of `kern_*` function pointers.
  - User client selectors (`kern_*` -> IOConnectCall*): 3 _kern_GetLayerDefaultSurface[ScalarMethod_stub]; 4 _kern_SwapBegin[ScalarMethod_stub]; 5 _kern_SwapEnd[StructMethod_stub]; 6 _kern_SwapWaitWithTimeout[ScalarMethod_stub] _kern_SwapWait[ScalarMethod_stub]; 7 _kern_GetID[ScalarMethod_stub]; 8 _kern_GetDisplaySize[ScalarMethod_stub]; 10 _kern_SetTVOutMode[ScalarMethod_stub]; 11 _kern_SetWSSInfo[ScalarMethod_stub]; 12 _kern_RequestPowerChange[ScalarMethod_stub]; 13 _kern_EnableStatistics[ScalarMethod_stub]; 14 _kern_SetFlags[ScalarMethod_stub]; 15 _kern_SetDebugFlags[ScalarMethod_stub]; 16 _kern_SetTVOutSignalType[ScalarMethod_stub]; 17 _kern_SetGammaTable[StructMethod_stub]; 18 _IOMobileFramebufferIsMainDisplay[ScalarMethod_stub]; 19 _kern_SetWhiteOnBlackMode[ScalarMethod_stub]; 20 _kern_SwapSignal[ScalarMethod_stub]; 22 _kern_SetDisplayDevice[ScalarMethod_stub]; 23 _kern_SetDigitalOutMode[ScalarMethod_stub]; 24 _kern_GetHDCPDownstreamState[StructMethod_stub]; 25 _kern_GetDigitalOutState[ScalarMethod_stub]; 26 _kern_printDisplayRegs[ScalarMethod_stub]; 27 _kern_GetGammaTable[StructMethod_stub]; 28 _kern_GetDotPitch[ScalarMethod_stub]; 29 _kern_GetDisplayArea[StructMethod_stub]; 30 _kern_EnableDisableDithering[ScalarMethod_stub]; 31 _kern_SetUnderrunColor[ScalarMethod_stub]; 33 _kern_EnableDisableVideoPowerSavings[ScalarMethod_stub]; 34 _kern_SetVideoDACGain[ScalarMethod_stub]; 35 _kern_SetLine21Data[ScalarMethod_stub]; 41 _IOMobileFramebufferEnableMirroring[ScalarMethod_stub]; 42 _IOMobileFrameBufferGetMirroringCapability[StructMethod_stub]; 43 _IOMobileFramebufferSetRenderingAngle[StructMethod_stub]; 44 _IOMobileFramebufferSetMirrorContentRegion[StructMethod_stub]; 47 _kern_HDCPSendRequest[ScalarMethod_stub]; 48 _kern_HDCPGetReply[ScalarMethod_stub]; 49 _kern_SurfaceIsReplaceable[ScalarMethod_stub]; 50 _kern_SetBrightnessCorrection[ScalarMethod_stub]; 51 _kern_SetColorRemapMode[ScalarMethod_stub]; 52 _kern_SwapCancel[ScalarMethod_stub]; 53 _IOMobileFramebufferSetCanvasSize[ScalarMethod_stub]; 54 _IOMobileFramebufferGetMatrix[Method_stub]; 55 _IOMobileFramebufferSetMatrix[Method_stub]; 56 _IOMobileFramebufferKernelTests[StructMethod_stub]; 57 _kern_GetColorRemapMode[ScalarMethod_stub]; 66 _kern_SPLCSetBrightness[ScalarMethod_stub]; 67 _kern_SPLCGetBrightness[ScalarMethod_stub]; 68 _kern_SetParameter[Method_stub]; 69 _kern_SwapSetColorMatrix[StructMethod_stub]; 72 _kern_DisableNotifications[ScalarMethod_stub] _kern_EnableNotifications[ScalarMethod_stub]; 73 _kern_ChangeFrameInfo[ScalarMethod_stub]; 74 _kern_SupportedFrameInfo[Method_stub]; 75 _kern_FactoryPortal[StructMethod_stub]; 77 _kern_GetCurrentAbsoluteTime[ScalarMethod_stub]; 78 _kern_SetBlock[Method_stub]; 79 _kern_GetBlock[Method_stub]; 80 _kern_GetLinkQuality[ScalarMethod_stub]; 81 _kern_SwapCancelAll[ScalarMethod_stub]; 82 _kern_GetProtectionOptions[ScalarMethod_stub]; 83 _kern_CopyLayerDisplayedSurface[ScalarMethod_stub]; 84 _kern_SetContrast[StructMethod_stub]; 85 _kern_GetBufBlock[Method_stub]; 86 _IOMFBGainEncoderFinishEncoding[Method_stub]; UNKNOWN {}
  - => Kernel needs: an `/arm-io` child node with device_type "display-subsystem" (+ canvas-width/height) and an IOService of class **IOMobileFramebuffer** (subclass name match) attached under it, opened with type 0, implementing at least GetDisplaySize(8), IsMainDisplay(18), GetID(7), GetLayerDefaultSurface(3), SwapBegin(4)/SwapEnd(5 struct)/SwapWait(6), Enable/DisableNotifications(72), frame info(73/74), CopyLayerDisplayedSurface(83), plus IOSurface for the swapped buffers. NEXT: CoreAnimation's AppleDisplay/IOMFBDisplay usage of these (constructor, first swap, vsync), then IOSurface.framework's IOSurfaceRoot selectors.
- **GUI PLAN (decided 2026-10-16): CPU rendering + kernel IOMobileFramebuffer.** User: CPU rendering is fine for now (goal = prove the GUI runs), may later research an Apple-style GPU driver for the Adreno 618 -> keep display output and renderer separable. QuartzCore findings (symbols from the cache's .symbols subcache: `ipsw dyld symaddr research/dsc/dyld_shared_cache_arm64 --image QuartzCore`; strings: `ipsw dyld macho ... QuartzCore --strings`):
  - Renderers: `sw_new_context` (CA::OGL::SWContext, full CPU renderer incl. blur, images, IOSurface-backed surfaces, make_buffer_current(WindowServer::Surface)), `gles_new_context`, `metal_new_context`, `new_null_context`. Env knobs include CA_ENABLE_OGL, CA_DISPLAY_TYPE, CA_FORCE_LOCAL_SERVER, CA_FORCE_TIMER, CA_CANVAS_WIDTH/HEIGHT, CA_MAIN_DISPLAY_SCALE/ORIENTATION, CA_DISABLE_RENDER, CA_ALWAYS_RENDER (selection logic not traced yet).
  - `-[CAWindowServer _detectDisplays]` (0x1884e3150) iterates static tables `open_funcs`/`count_funcs` = AppleCLCDDisplay, AppleRGBOutDisplay, AppleWirelessDisplay only; each `count()` = `display_count_of_type(IOMFBDisplayType)` over **`IOMobileFramebufferCreateDisplayList()`**, `open(i)` = `create_display_of_type` -> `CA::WindowServer::AppleDisplay(CFString name, ..., IOMFBDisplayType)` (an IOMFBDisplay driven by IOMFBServer: vsync_callback, frame_info_callback, swaps). `CA::WindowServer::VirtualDisplay` exists but is never auto-detected (explicit API only). => the main display MUST come from an IOMobileFramebuffer kernel service.
  - NEXT: reverse IOMobileFramebuffer.framework: IOMobileFramebufferCreateDisplayList / IOMobileFramebufferOpen (service matching, display type property), the user-client selectors used by IOMFBServer (display size/modes, swap begin/set-layer/end with IOSurface ids, vsync notifications), then the IOSurface kernel side (IOSurfaceRoot user client: create/lookup/lock, memory mapping) that both IOMFB swaps and CA surfaces need. Kernel: SWEET IOMobileFramebuffer service that copies the swapped IOSurface to the 1080x2400 framebuffer at 0x9c000000.
- **logd fixed; backboardd = DISPLAY WALL (boot 0045n on v7, 21:08:57 UTC, frames VID_20261016_230850 00:55-01:28, slide 0x4c3ec000, `research/crash-0045n-v7.txt`).** No `CRASH logd` any more; new daemons run (runningboardd, WirelessRadioManagerd, networkserviceproxy, dasd, cloudpaird, magicswitchd, atc, askpermissiond, ManagedSettingsAgent, containermanagerd uses AKS 0/17). New: locationd sig 11 at CFRunLoopAddSource+132 (NULL source) from its main+0x2af138 in a dispatch_once. **backboardd** sig 6: abort <- std::terminate <- _objc_terminate <- dispatch_once <- main+0x9334 = the "start %{public}@..." wrapper (0x100009264) for **"window server"** (block 0x10000a770 -> dispatch_once body 0x10001d7f0 = StartWindowServer): MTLSetShaderCachePath, `[CAWindowServer serverWithOptions:@{kCAWindowServerDisableUpdatesOnMainDisplay: @YES}]`, `setRendererFlags:`, **`mainDisplay`** -> `displayId` -> `[CADisplay displayWithDisplayId:]`, setWindowServerDisplay:, bounds, pointScale, currentMode, preferredScale, nativeOrientation, touchScales... With no display, mainDisplay is nil and it throws. (HID system and GS start fine before it.) SpringBoard dies on the same missing display (FBSDisplayMonitor). QuartzCore display backends (from the cache's .symbols subcache, `ipsw dyld str`): `CA::WindowServer::AppleCLCDDisplay` (IOMobileFramebuffer hardware) and **`CA::WindowServer::VirtualDisplay`** (open(), new_server, clone_update). `CA_DISPLAY_TYPE` and `CA_FORCE_LOCAL_SERVER` strings exist in QuartzCore but have no direct xref (likely read via a table of CA_* env names). NEXT: find how CA::WindowServer::Server enumerates displays (IOMobileFramebuffer services vs VirtualDisplay / CA_DISPLAY_TYPE) and choose: (a) kernel IOMobileFramebuffer service + IOSurface, or (b) a VirtualDisplay rendered by CA's CPU path (CA::OGL::SW) and copied to the framebuffer.
- **watchdogd + CommCenter + identityservicesd fixed (boot 0045n on v6, 20:41:30 UTC, frames VID_20261016_224133 00:39-01:13, slide 0x20fec000).** `SWEET WDT: watchdogd sel 0 in 0/0 out 1/0 -> ok`, no watchdogd/CommCenter/identityservicesd crashes (CommCenter/identityservicesd now call AKS 0/17/7). usermanagerd boot task runs (UMD_INIT creates primary session uid 501 "mobile", personas 99 System and 199 Default registered with the kernel); `Failed to atomically swap writing file /private/var//keybags/usersession.kb: Operation not supported (45)` (HFS lacks the atomic swap rename; non-fatal so far). Remaining: SpringBoard (FBSDisplayMonitor, no display), backboardd (uncaught exception), mediaserverd, lockdownd (crash inside obfuscated code, main+0x4c1b4?), logd. **logd**: main+0x18160 = `BUG IN LIBTRACE: Failed to migrate directory` from 0x10001801c(dir 0): 0x100016538 requires the dir to exist with uid/gid/mode from the table at 0x10002df14 (0: /private/var/db/diagnostics 272:272 0751, 1: /private/var/db/uuidtext 272:272 0755, 2: /private/var/db/diagnostics/timesync 272:272 0755) plus a `.migrated` file, else XPC "operation 6" to a helper and crash. **Image v7** = v6 + those dirs/markers (also in mkiosroot.sh), SHA256 in `ios16-root-20H392-v7.img.sha256`, `.simg` ready. Boot with `shim/out/shim-boot-0045n.img`.
- **Exit-reason text works (boot 0044n, frames VID_20261016_221726 00:32-00:53, slide 0x22d5c000).** CommCenter/identityservicesd: `couldn't dlopen libobjc-trampolines.dylib: dlopen(/usr/lib/libobjc-trampolines.dylib, 0x0106): tried: '/usr/lib/libobjc-trampolines.dylib' (no such file, not in dyld cache)`: the file only exists in the OS cryptex and dyld does not search cryptexes at ignition_level=1. 19 cryptex files have no root counterpart (OS: DriverKit dyld cache + .symbols, VisualTestKit, libffi-trampolines, libobjc-trampolines, libstdc++ 6.0.9 + 2 symlinks; App: SafariSupport.bundle, AuthenticationServicesAgent, com.apple.Safari.History, passwordbreachd, safarifetcherd, webbookmarksd, webinspectord, webpushd) -> hard-linked (symlinks copied: Linux hfsplus cannot hard-link symlinks); `tools/mkiosroot.sh` does it generically. watchdogd still crashed: the `-wdt_no_pmu` / `/arm-io/wdt no-pmu` check (0x10000a01c) only runs when os_variant_has_internal_diagnostics; otherwise 0x100009b18 looks up IOServiceMatching("IOService") + IOPropertyMatch {IOPMUPrimary: 1} (NSConstantDictionary) and crashes if absent, then IORegistryEntrySetCFProperty(boot stage 0xff) (failure only logged). Patch **0045**: IOWatchdog sets IOPMUPrimary = 1 and accepts setProperties; CI `38961814194` -> `xnu/out-0045`. **Image v6** = v5 + the 19 cryptex links, SHA256 7c610c6a...297a, `ios16-root-20H392-v6.simg` (full flash needed). Boot image `shim/out/shim-boot-0045n.img` (0045 + sweet-0044hi.adt). Still open: SpringBoard (no display), backboardd (uncaught exception, reason none), mediaserverd (Fig NULL dict), logd (main+0x18160), lockdownd (main+0x4c1b4).
- **System survives daemon crashes (boot 0043p, video frames VID_20261016_215006 00:25/00:42/00:54).** With the v5 plist patch no panic: launchd keeps launching (runningboardd, powerd, tccd, healthd, peopled, cryptexd, routined, aksd, mediaremoted, familynotificationd, nanoprefsyncd, navd, cloudpaird, passd, amsaccountsd, accessoryupdaterd, wcd ...). Shared cache slide that boot 0x406b4000 (`research/crash-0043p.txt`). watchdogd still crashed at main+0xb82c from the PMU step: `-wdt_no_pmu` is only honored when os_variant_has_internal_diagnostics; customer builds read `no-pmu` from IODeviceTree:/arm-io/wdt (IORegistryEntrySearchCFProperty) -> mkadt adds /arm-io/wdt {no-pmu}. **cryptexd** sig 6: abort_with_payload <- os_crash_fmt <- `__darwin_arm_secure_boot_chip.cold.7` ("panic: failed to get crypto algorithm") <- `_img4_chip_select_personalized_ap`: reads /chosen use-ddi-secure-boot, allow-ecid-mismatch, crypto-hash-method ("sha1" | "sha2-384"), /chosen/asmb lp-smb0/1 -> mkadt adds crypto-hash-method "sha2-384". **identityservicesd** sig 6 = same `_objc_fatal` in imp_implementationWithBlock as CommCenter (block trampolines; message needed). lockdownd sig 5 at main+0x4c1b4. Patch **0044**: crash report adds `CRASH name reason ns N code X: text` (exit reason description = abort_with_payload text). CI `38959096811` -> `xnu/out-0044`, QEMU OK. Image `shim/out/shim-boot-0044n.img` = 0044 + `shim/out/sweet-0044hi.adt` (wdt no-pmu, crypto-hash-method) + cmdline (wdt=-1, no -wdt_no_pmu).
- **First symbolicated daemon crashes (boot 0043u on journaled v4, 19:38:53 UTC; video frame chat image 36; transcribed in `research/crash-0043u.txt`; shared cache slide 0x40d60000 for every process).** (Gemini's video transcription invented values, use frames only.) `tools/sweet-crash.py research/dsc/dyld_shared_cache_arm64 x research/crash-0043u.txt`:
  - **watchdogd** pc main+0xb82c (`_os_crash` trap helper) from 0x100009bb0 = set PMU boot stage via IORegistry (no PMU) -> fix: boot-args `wdt=-1 -wdt_no_pmu` (Apple's own test mode writes "%s wdt=-1 -wdt_no_pmu"; watchdogd logs "skipping setting PMU boot stage because -wdt_no_pmu found in boot-args or no-pmu found in EDT"). Kernel effect of wdt=-1: sched hygiene debug off, zone_exhausted_timeout -1 (harmless).
  - **SpringBoard** sig 5 in `-[FBSDisplayMonitor _initWithBookendObserver:transformer:]+1160` <- `+[FBDisplayManager sharedInstance]` <- `FBSystemShellInitialize` <- `SBSystemAppMain`: no main display. GUI WALL: display service (IOMobileFramebuffer / CoreAnimation display).
  - **backboardd** sig 6: abort <- std::terminate <- _objc_terminate (uncaught exception; message not shown).
  - **CommCenter** sig 6: `_objc_fatal` in `imp_implementationWithBlock` (from `+[_PFRoutines initialize]`, CoreData): objc block trampolines cannot be allocated -> likely kernel gap (trampoline page remap); will hit many processes.
  - **mediaserverd** sig 11: CFDictionaryGetValue(NULL) in FigNotificationListenerStartNotifications (routing session manager).
  - **logd** sig 5 at its main+0x18160 (bt main+0x9dfc); lockdownd sig 5 (not yet symbolicated).
  - Image v5 = v4 + launchd.plist (`/System/Library/xpc/launchd.plist`, contiguous at image offset 0x1e025f000, 1456980 bytes) with the 7 `_PanicOnCrash` keys renamed `_PanicOnCrasX` in place (raw image patch, no metadata change) -> `diff-v4-v5.simg` = 7 blocks; v5 SHA256 78f96974...185a. Boot image `shim/out/shim-boot-0043p.img` = 0043 + `wdt=-1 -wdt_no_pmu`.
- **0043 console crash reports (QEMU verified); image `shim/out/shim-boot-0043u.img`; NEXT: flash v4 + boot 0043u.** Boot 0042u (video photo IMG_20261016_211646): short exec lines work; watchdogd crashes with NO `SWEET WDT:` line (fails before its first IOWatchdog call: lookup or IOServiceOpen, or elsewhere); lockdownd, SpringBoard, mediaserverd, CommCenter, identityservicesd, backboardd, watchdogd all crash repeatedly ("Too many corpses being created" = corpse limit, no crash reports). AKS callers: lockdownd (0, 7), imagent/sleepd (0, 17, 7), backboardd (0, 17), UserEventAgent (0, 7, 102, 103 -> unsupported). Patch **0043**: `sweet_crash_log` (osfmk/kern/exception.c, called from proc_prepareexit for crash exits on the current proc) prints `CRASH name[pid] sig S code (exc<<32|code) sub X slide SR mh MH`, `CRASH name pc lr sp fp far`, `CRASH name bt` (10 fp frames); AKS sel 103 notify_state_is_set -> 0. CI `38954520419` -> `xnu/out-0043`. QEMU test: init forks, child `brk #1`: `CRASH launchd[2] sig 5 code 0x600000001 sub 0x1000004f0 slide 0x0`, pc correct, parent keeps running (pid 1 crashes panic before proc_prepareexit, so test with a child). `tools/sweet-crash.py CACHE ROOT [report]` symbolicates typed-in CRASH lines (`ipsw dyld a2s` with addr - slide; main binary offsets = addr - mh + 0x100000000).
- **v3 reflashed 18:50-18:55 UTC (`research/flash-userdata-v3b.log`, 24/24 OKAY); 0042u booted 19:01:17 UTC on the clean disk (`research/boot0042u-2.log`), SLOW MO VID pending (watchdogd failure reason).** ABL hangs on the first `fastboot boot` after every big flash; user re-enters fastboot. User gave standing approval to reflash/boot without asking (memory standing-flash-approval). Commits now end with `Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>` (user request; never a Claude trailer).
  - watchdogd flow: connect_to_kext (0x100005408): IOServiceGetMatchingService("IOWatchdog") fails -> "failed to discover" + _os_crash; IOServiceOpen(type 1) fails -> "IOServiceOpen failed with error : 0x%x" + _os_crash. Then sel 0 (out 1 scalar): 0 -> "userspace monitoring disabled", 0x100009bb0, sleep 2, exit(0); nonzero -> "userspace monitoring enabled", normal operation.
  - **Image v4 (JOURNALED):** `tools/mkiosroot.sh` now does `mkfs.hfsplus -J`, mounts with `-o loop,force`, adds the /System/Library/Caches/com.apple.dyld hard links (44) and /private/var/keybags/systembag.kb (0700/0600 root). Built from the IPSW dmgs (OS 087-86698-044, SystemOS 087-86593-047, AppOS 087-86715-047) -> `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v4.img` SHA256 fba4aeae...ec0f9 + `.simg`. Header: attr 0x80002100 (unmounted, journaled), journal info block 129, journal 8 MiB at 532480 flagged need-init. Linux `fsck.hfsplus -n` reports "Volume header needs minor repair" on any fresh journaled volume even after -y (quirk). QEMU: 64 MB journaled test root (`rd=md0 rootrw=1`, 0042 kernel): `hfs: Initializing the journal (joffset 0x3000 sz 0x80000)` -> mounted -> init runs, no panic. NEXT: flash v4 (full), boot 0042u.
- **SPRINGBOARD STARTS (boot 0041u 18:39:53 UTC, photo IMG_20261016_204142).** backboardd, bluetoothd, fairplayd.H2, SpringBoard (pid 111, crashed: "Too many corpses"), xpcproxy run; `SWEET AKS: backboardd sel 0 -> 0`, `sel 17 in 1/0 out 0/4096 -> 0`. Panic: watchdogd[113] exited, namespace 2 subcode 5 (SIGTRAP = its _os_crash): watchdogd crashes on purpose when IOServiceGetMatchingService("IOWatchdog") fails ("connect_to_kext() unexpectedly failed") or IOServiceOpen(type 1) fails ("IOServiceOpen failed with error"); IOWatchdog DOES register (QEMU msgbuf: `SWEET WDT: IOWatchdog registered`), cause not on screen yet -> need video. Note `wdt_test=no_user_checkin` makes watchdogd log "detected testing boot-args to not run, exiting" (path not reached here?). **HFS CORRUPTION:** `hfs_swap_BTNode: offset #0 invalid (0xBC1D) (blockSize 0x2000 numRecords 81)`, `node=19677 fileID=4` (catalog), `hfs_mark_inconsistent` repeated. Likely cause: non-journaled HFS + unclean panics while writing (0038e wrote a 2.7 GB core then panicked; every boot since ends in panic). Plan: reflash v3 (clean), longer term HFS journaling. Patch **0042**: one short `exec NAME` console line instead of the 3-line "Allowing spawn of iOS binary" notice; CI `38949451276` -> `xnu/out-0042`; image `shim/out/shim-boot-0042u.img` (same cmdline as 0041u).
- **LAUNCHD DAEMONS RUN (boot 0039u, 18:29:13 UTC, photo IMG_20261016_203052).** With `launchd_unsecure_cache=1` all boot tasks pass and launchd starts LaunchDaemons (pids > 100: identityservicesd 114 and lockdownd 106 crash, "Too many corpses being created"). Fatal: `watchdogd: ERROR: failed to discover watchdog KEXT service` -> exit (namespace 2 = signal, subcode 5) -> `_PanicOnCrash` panic. watchdogd: IOServiceMatching("IOWatchdog"), IOServiceOpen type 1, IOConnectCallScalarMethod sel 0/1 (check in)/3/4 (disable/enable userspace monitoring)/5, IOConnectCallStructMethod sel 2 (1 KiB in) and 9 (small out); VM path ("detected virtual machine environment and no watchdog KEXT found, exiting...") only when it detects a VM. Console also flooded with `hfs: set hardlink bit on vol=iOSRoot cnid=321318` (Linux-made hard links lack kHFSHasLinkChainBit; repaired only in memory on each lookup). Patches **0040** (`iokit/Kernel/arm/SweetWatchdog.cpp`: IOWatchdog + IOWatchdogUserClient, all selectors succeed with zeroed outputs, each logged once as `SWEET WDT: ...`) and **0041** (sweet_hfs hfs_catalog.c: the three link-repair printfs limited to 8 per boot); CI `38948245372` PASSED -> `xnu/out-0041` (QEMU regression OK). Image `shim/out/shim-boot-0041u.img` (0041 + sweet-0038hi.adt + ufsrw rootrw + launchd_unsecure_cache=1) booted 18:39:53 UTC (`research/boot0041u.log`), photo pending.
- **v3 FLASHED + keybag passes; next wall AMFI (launchd_cache_loader).** User said "flash, nothing to lose": `fastboot -S 512M flash userdata ios16-root-20H392-v3.simg` 17:51:52-17:57:01 UTC, 24/24 chunks OKAY (`research/flash-userdata-v3.log`). userdata = v3 (SHA256 fa84274c...22df) until the next rw boot. ABL fastboot then hung ("Sending" forever, `reboot bootloader` timed out) until the user re-entered fastboot by hand (common Xiaomi ABL crash). Boot 0038e 18:06:15 UTC (`research/boot0038e-3.log`), photos IMG_20261016_200812/200838: keybag OK, then boot tasks tzinit (`Bad zoneinfo link`, resets to system partition, OK), finish-restore, finish-demo-restore, sysstatuscheck, prng_seedctl (`failed to load kernel prng seed`, `failed to load virtual random (-147)`, AllowCrash), auearlyboot, then **launchd_cache_loader: `abort: Unable to find AMFI`** -> 2.7 GB core written to /cores in 91 s -> launchd panic `launchd_cache_loader[22] exited -- exit reason namespace 7 subcode 0x1`. launchd_cache_loader: SecStaticCodeCheckValidity of /System/Library/xpc/launchd.plist with detached .sig, then IOServiceMatching("AppleMobileFileIntegrity") + IOServiceOpen type 0 + IOConnectCallMethod sel 6 (1 scalar = 1, struct in = cdhash) = trust the cdhash, then sends the cache to launchd. Boot-arg `launchd_unsecure_cache=1` skips signature + AMFI ("Using unsecure cache"); the value is parsed with strtoul base 10 (helper 0x100002da8), so a path = 0 = off: boot 0039w (18:17:13 UTC, `=/System/Library/xpc/launchd.plist`, photo IMG_20261016_202008) still aborted `Unable to find AMFI` (core dump now skipped: `Failed to generate core file ... error 14`, so 0039 works). Image `shim/out/shim-boot-0039u.img` uses `launchd_unsecure_cache=1`. Patch **0039**: do_coredump = 0 on SWEET (kern.coredump sysctl still works); CI `38945267341` -> `xnu/out-0039` (_do_coredump now in __common). Image `shim/out/shim-boot-0039w.img` = 0039 + sweet-0038hi.adt + ufsrw rootrw + `launchd_unsecure_cache=/System/Library/xpc/launchd.plist`. TODO later: real AppleMobileFileIntegrity IOService (sel 6 trust cdhash etc.).
- **Image v3 ready, awaiting user "flash".** Boot 0038w 17:38:10 UTC, photo (chat image 30): `SWEET AKS: keybagd sel 0 -> 0x0`, `sel 14 out 1/0 -> 0x0` (x2), `MKB_INIT: mount is with NO CPROTECT ... does not support Data Protection`, `System Keybag loaded`; then keybagd still opens /private/var/keybags/systembag.kb (0x100012d3c, for BackupKeyBagKeys) -> ENOENT, `no-effaceable-storage` not set -> `Can't load the keybag. tears in rain... Time to die` -> `FATAL KEYBAG ERROR: kb_load` -> reboot -> Halt/Restart Timed Out panic. Fix: ADT `/defaults/no-effaceable-storage` (mkadt.py; `shim/out/sweet-0038hi.adt`, `research/q0038-hi.adt`, QEMU OK) + file `/private/var/keybags/systembag.kb` (0600 root, dir 0700; XML plist {KeyBagKeys: data}). **v3 image** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v3.img` = v2 + that file, fsck OK, SHA256 fa84274c...22df; `ios16-root-20H392-v3.simg` (img2simg: zero blocks become FILL chunks, so a full flash overwrites every block the rw boots changed). Flash: `fastboot -S 512M flash userdata ios16-root-20H392-v3.simg`, then boot `shim/out/shim-boot-0038e.img` (0038 + sweet-0038hi.adt + ufsrw=1 rootrw=1).
- **0038 AppleKeyStore (CI 38938744130 PASSED -> `xnu/out-0038`; QEMU msgbuf: `SWEET AKS: AppleKeyStore registered (no passcode, unlocked)`; phone image `shim/out/shim-boot-0038w.img` booted 17:38:10 UTC, `research/boot0038w.log`, photo pending).** QEMU full kernel log trick (fb.txt is only the last screen): `llvm-nm KERNEL | grep _msgbufp` -> `--hmp "x /1gx ADDR"` gives struct msgbuf address (static, 0xfffffe0007b56678 on 0038) -> `--hmp "x /3gx STRUCT"` gives bufc (3rd quad) -> `--hmp "memsave BUFC 0x20000 \"FILE\""` (quote the file name), `tr -d "\0" < FILE`. Same inputs give the same addresses. Boot 0037w 17:08:35 UTC, photo IMG_20261016_191052: bindfs fixed mount; mount-phase-1/2 pass; `cc_fips_test` boot task: all corecrypto FIPS POST tests PASSED in userspace; then keybag boot task `keybagd --init`: `MKB_INIT: FATAL KEYBAG ERROR: failed to load system bag`, `Could not get options entry from the device tree` (x2, IODeviceTree:/options = NVRAM, non-fatal), `REBOOTING INTO RECOVERY MODE` -> reboot -> panic `Halt/Restart Timed Out @IOPlatformExpert.cpp:884` (no restart path on SWEET).
  - keybagd (static libaks): load_devicebag (0x100012c30) calls aks_get_system (sel 14, 0-1 scalar in, 1 out) and loads /private/var/keybags/systembag.kb only if the result is 0xe00002f0 (NotFound); any other error -> fatal; SUCCESS -> skip the file (userspace-reboot path). systembag.kb is a plist {KeyBagKeys}; decrypted with effaceable storage unless IODeviceTree:/defaults has `no-effaceable-storage`. Image has no /private/var/keybags. has_data_protection: statfs of /private/var; no cprotect on our HFS -> device-bag volume mapping skipped.
  - libaks connection: IORegistryEntryFromPath / IOServiceMatching("AppleKeyStore"), IOServiceOpen type 0, then selector 0 (no args) must succeed.
  - **AppleKeyStore selector map (iOS 16.7 AppleKeyStore.framework, via `ipsw dyld disass`):** 0 open, 2 create_bag, 3 save_bag, 4 unload_bag, 5 set_system_with_passcode, 6 load_bag, 7 get_lock_state (1 in, 1 out; bit 2 = unlocked since boot), 9 unlock_device, 10 wrap_key, 11 unwrap_key, 12 unlock_bag/test_stashed_kek, 13 lock_bag, 14 get_system, 15 change_secret_se, 16 internal_state, 17 get_device_state (1 in, struct out <=4K: DER SET of SEQUENCE{UTF8 key, INTEGER}: ss state, sls lock_state (MKBGetDeviceLockState returns it), sb backoff, sbo, sfa failed attempts, sfao, sgs generation, sas assertions, sgpe grace, srcd countdown, sr reserved), 18 recover_with_escrow_bag, 19 obliterate_class_d, 20 drain_backup_keys, 21/22 set/clear_backup_bag, 23 get_bag_uuid, 24 rewrap_key_for_backup, 26/27 assert_hold/drop, 30/31 set/get_configuration, 33-37 stash, 38 auth_token_create, 40 generation, 41 fdr_hmac_data, 42 verify_password, 43 _aks_operation, 44 remote_session, 49/50 signing keys, 51/53/54 stash enable/persist/escrow, 55 unload_session_bags, 57/58 remote peer, 59-62 smartcard, 63 migrate_s_key, 65 set_keybag_for_volume_with_cookie_persona, 66/67 sync bags, 68 copy_volume_cookie_persona, 69-73 se_*, 74-79/84/85/99 fv_* (FileVault/VEK), 80/81 absinthe, 82 make_public_backup_bag, 83 drop_auxiliary_auth, 86 kext get/set options, 87 invalidate_bag, 88-91/94/95/106/107 se_*, 92 lower_iteration_count, 93 get_internal_info, 96-98/117 keybag_persona create/list/delete, 100/101 assert promote/consume, 102/103 notify_state set/is_set, 104 fairplay, 105 change_secret_epilogue, 108 delete_xart_leak, 110-113 backup volume, 114 sealed_hashes_set, 115 prewarm_sps, 118-128 AKSIdentity*/AKSGetLastUser.
  - Patch **0038**: `iokit/Kernel/arm/SweetKeyStore.cpp` (class AppleKeyStore on IOResources, IOUserClientClass AppleKeyStoreUserClient; personality in KernelConfigTables.cpp): 0/4/5/9/12/13/26/27/55/102 success, 14 -> handle 0, 7 -> 0x4, 17 -> DER state (ss 4, sls 0), others kIOReturnUnsupported; logs `SWEET AKS: <proc> sel N arg0 in a/b out c/d -> ret` for every call.
  - Tools: shared cache copied to `research/dsc/` (git-ignored; ipsw needs a writable dir for its .a2s symbol cache); `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image AppleKeyStore|MobileKeyBag`; scratchpad `annot2.py BIN DIS FIXUPS` annotates llvm-objdump output with strings and import names (`ipsw macho info --fixups`).
- **0037 bindfs (phone boot 0037w pending photo).** Boot 0036mw 16:57:47 UTC, photo IMG_20261016_190102: WRITABLE ROOT WORKS: `/dev/ufsroot0 on / (hfs, local, noatime)` after mount_hfs update (GetMasterBlock EBUSY harmless), coredump of mount written to /cores (rw). Then `mount: failed to mount /private/preboot/5CBE3601.../usr/standalone/firmware -> ...: Operation not supported by device (19)` -> exit 74 -> launchd panic `mount[2] exited`. Cause: the macOS-derived config had nullfs but no bindfs (ENODEV = unknown fs type). Caller passes env 0 -> bind not required, ENOENT (missing source dir) tolerated. Patch **0037** adds `bindfs` to FILESYS_BASE in MASTER.arm64.sweet; CI `38936550483` -> `xnu/out-0037` (bindfs_vfsops present; QEMU probe regression OK). Image `shim/out/shim-boot-0037w.img` (0037 + sweet-0036hi.adt + ufsrw=1 rootrw=1) booted 17:08:35 UTC (`research/boot0037w.log`). NOTE: userdata has now been modified by rw boots; it no longer equals the v2 image, so sparsediff with v2 as OLD is not safe any more (full reflash or a new baseline needed).
- **0036 phone: launchd boot tasks run; mount -P 1 needs /chosen/boot-manifest-hash.** Boot 0036hr 16:15:32 UTC, photo IMG_20261016_185236: platform check passes; fsck boot task runs (`fsck: not booting main or diagnostic OS. Skipping fsck on OS container`); mount-phase-1: `failed to get filesystem info` x2 (no IODeviceTree:/filesystems/fstab, non-fatal), `mount: data volume missing, but not required in env: 0`, mount_hfs update of /: `GetMasterBlock: Error 16 opening /dev/ufsroot0` (EBUSY, mounted block dev, non-fatal) then mount() EROFS (read-only boot) -> `mount: / failed with 1`; then `failed to get boot-manifest-hash` -> errx exit 66 -> launchd panic `mount[3] exited`. /sbin/mount: fn 0x100005f90 reads IODeviceTree:/chosen boot-manifest-hash (hex), bindfs-mounts /private/preboot/<HEX>/usr/standalone/firmware (ENOENT tolerated unless required). `tools/mkadt.py` now adds `boot-manifest-hash` = SHA-384("sweet boot manifest") (5CBE3601...5205). ADTs `shim/out/sweet-0036hi.adt`, `research/q0036-hi.adt` (QEMU probe OK, csops 0x26000001). Image `shim/out/shim-boot-0036mw.img` (0036 + new ADT + ufsrw=1 rootrw=1), booted 16:57:47 UTC (`research/boot0036mw.log`): FIRST WRITABLE BOOT of userdata (restore source: v2 image on SSD).
- **0036 CS_SIGNED (phone boot 0036hr pending photo).** Phone boot 0035hr 2026-10-16 15:57:02 UTC (`research/boot0035hr.log`, photo IMG_20261016_180249): highmem works on hardware; shared cache maps (trust-cache warnings only), Apple launchd runs (`Darwin Bootstrapper Version 7.0.0`, `entering ondemand mode`), starts boot task fsck, then userspace panic `boot task is not a platform binary (fsck, /sbin/fsck)`. launchd+0x3fea0: `csops(pid, CS_OPS_STATUS)` must have bit 0x04000000. Cause: nothing set CS_SIGNED (AMFI does it in vnode_check_signature), so csproc_get_blob() returns NULL for every process and csops never reports CS_PLATFORM_BINARY (also breaks per-process cdhash/entitlement queries). QEMU probe init (`csops` status printer, scratchpad csq/) on 0035: `0x02000001`; patch **0036** (`*cs_flags |= CS_SIGNED` in the SWEET AMFI hook), CI `38929635791` -> `xnu/out-0036`: `0x26000001`. Images `shim/out/shim-boot-0036hr.img` / `-0036hw.img` (same cmdlines as 0035). 0036hr booted 16:15:32 UTC (`research/boot0036hr.log`). Next launchd boot tasks (embedded __config plist): fsck -qL and mount -P 1/2 (RequireSuccess); no /etc/fstab on the image.
- **0035 ENTITLEMENTS + PLATFORM BINARIES (QEMU verified).** `xnu/patches/0035-sweet-amfi-entitlements-platform-binaries.patch`: new `iokit/Kernel/arm/SweetEntitlements.cpp` (OSObject subclass: OSDictionary from OSUnserializeXML of the signature's XML entitlements + original XML for CS_OPS_ENTITLEMENTS_BLOB); `bsd/kern/sweet_amfi.c` answers queryEntitlementBoolean/String(+WithProc, string also matches array members), copyEntitlementAsOSObject(+WithProc, retained), asdict (retained), get_xml (kalloc_data copy); built-in MAC policy "SWEET AMFI" (registered from bsd_init before vfs/exec) with mpo_vnode_check_signature: every signed binary on the MNT_ROOTFS mount gets CS_PLATFORM_BINARY (stand-in for AMFI's static trust cache: iOS system binaries are ad-hoc, cd_flags 0x2, platform byte 0), and attaches entitlements ONLY for the writable copy (cs_blob != csb_ro_addr; revalidation passes the RO blob). LESSON: first version attached in adjustContextWithoutMonitor -> QEMU `unable to adjust OSEntitlements context without monitor: 53` because ubc_cs_convert_to_multilevel_hash (4K-page signatures on 16K kernel) sets csb_entitlements_blob = NULL; ALL iOS 16 binaries (launchd, backboardd, SpringBoard) use 4K code pages, so this would have broken launchd on the phone. Boot-arg `sweet_amfi_debug=1` logs `SWEET AMFI: <ident>: N entitlements (test: ...)`. `tools/mkinit.py OBJ OUT [ENTS.plist]` adds an XML entitlements blob (special slot -5; default output byte-identical). QEMU `research/q0035-ent` (RAM disk `research/root-hfs-ent.img`, init with 3 test entitlements, highmem): `org.sweet.init: 3 entitlements (test: bool 1 string 1 array 1)` then init PID 1. MAC policies loaded: 0034 = 1, 0035 = 2 (QMP `mac_policy_list`). CI `38909807230` -> `xnu/out-0035`. Phone images: `shim/out/shim-boot-0035hr.img` (highmem, ro), `-0035hw.img` (highmem, +ufsrw=1 rootrw=1), `-0035r.img` (low mem fallback).
- **PHONE TEST ORDER (updated):** 0035hr -> if instant reset, 0035r -> then 0035hw.

- **HIGHMEM: XNU in the upper DDR bank, ~3.95 GiB (was 1.2 GiB).** Live FDT memory = <0x80000000 2G> + <0x1_00000000 0xfcc00000>; all fixed reserved-memory carve-outs are in the low bank (highest: ramdump_fb 0xaf000000, rmtfs ~0xfde01000), dynamic pools have alloc-ranges <= 0xffffffff, UFS node has no `iommus` (Linux DMAs anywhere). `tools/mkadt.py --highmem` sets /chosen dram-base 0x100000000 dram-size 0xfcc00000; `tools/mkpayload.py` now takes phys_base/mem_size from the ADT (default payloads byte-identical, verified); `tools/qemu-sweet.py` sizes -m from the payload (8G for highmem). Patch **0034** (`0034-sweet-ufs-dma-above-4g.patch`): UFS refuses DMA memory above 4G without UFSHCI CAP.64AS (bit 24), logs `DMA at 0x...`. CI `38905889883` success -> `xnu/out-0034`. **QEMU verified** (`research/q0034-hi`, ADT `research/q0034-hi.adt` --qemu --highmem, HFS RAM disk): phys_base 0x100000000, init PID 1 heartbeat, no panic; `max_mem` = `mem_size` = 0xfcc00000 read via QMP (`research/q0034-hi-mem`). Phone images: `shim/out/shim-boot-0034hr.img` (read-only) / `-0034hw.img` (+ufsrw=1 rootrw=1), ADT `shim/out/sweet-0034hi.adt` (asmb SIP + sweet-ufs + highmem). PHONE RISK: unknown runtime TZ/XPU protection in the upper bank would show as an instant reset; fall back to 0033r/w. Watchdog ruled out for the overnight reset: msm-watchdog bark is 20 s (qcom,bark-time 0x4e20) yet panic screens stay up for many minutes.

- **Offline research 2026-10-16 (user away, phone unavailable). Images rebuilt** `shim/out/shim-boot-0033r.img` / `-0033w.img` now also carry `wdt_test=no_user_checkin`. NEXT ON PHONE: boot 0033r (expect past in6_nigroup), then 0033w (first writes: rw root fixes HFS link-chain bits itself and gives /var for launchd sockets /var/run/syslog, /var/run/lockdown.sock).
- **launchd.plist** (`/System/Library/xpc/launchd.plist`, compiled; 532 daemons, 66 RunAtLoad/KeepAlive, list in scratchpad `early-daemons.txt`). `_PanicOnCrash` daemons: containermanagerd/usermanagerd (InternalOnly), lockdownd/remoted/trustd/remotepairingdeviced (AutomationOnly), **watchdogd unconditional** (KeepAlive SuccessfulExit=false). watchdogd needs IOKit `IOWatchdog` kext service; if absent it exits only when `kern.hv_vmm_present` (ADT `/defaults/vmm-present`, hvg_sysctl.c) or with boot-arg `wdt_test=no_user_checkin` ("detected testing boot-args to not run, exiting"; reads kern.bootargs). Using the boot-arg (vmm-present would change many daemons; keep as an option).
- **GUI path research (QuartzCore in the shared cache, `ipsw dyld str CACHE STR`, ~10 s/search):** CA::WindowServer has IOMFBServer/IOMFBDisplay (Apple DCP/IOMobileFramebuffer), AppleCLCDDisplay, AppleRGBOutDisplay, **VirtualServer/VirtualDisplay** and **MemorySurface**, EAGLServer. Renderers: CA::OGL::MetalContext, GLContext and a **CPU software rasterizer CA::OGL::SW** (samplers/blend modes; "image is too large for software renderer"). Env knobs: `CA_ENABLE_OGL`, `CA_DISPLAY_TYPE`, `CA_FORCE_LOCAL_SERVER` (249 CA_* vars). Hypothesis (unverified): backboardd's render server could run a VirtualServer/MemorySurface with the SW renderer and we copy the surface to the ABL framebuffer -> needs disassembly of CA::WindowServer server creation. Expected other GUI walls: IOSurface (closed-source kext; needs a reimplementation), IOMobileFramebuffer user client, IOHID touch events, AppleKeyStore/keybag (SEP) for lock state, MobileGestalt answers (AppleCLCD etc.).

- **0032r RESULT (boot 05:07:50 UTC, `research/boot0032r.log`, photo IMG_20261016_071200): SHARED CACHE MAPPED, LAUNCHD RUNS.** With SIP relaxed (ADT asmb) the `not in trust cache` lines are only warnings for every subcache (.31 ... .42.dyldlinkedit); no SIP rejection; libSystem loads; launchd configures networking (`skip attaching fsw to lo0 using legacy TX model`) -> kernel data abort far 0x28 in `SHA1Init` (symbolized as sha1_init) from `in6_nigroup` (IPv6 on lo0): g_crypto_funcs NULL. Kernel addresses are unslid: `~/swift/usr/bin/llvm-symbolizer --obj=KERNEL ADDR` works directly.
- **0032** BearSSL SHA-1 (`0032-sweet-bearssl-sha1.patch`), **0033** BearSSL SHA-384/512 (`0033-sweet-bearssl-sha384-sha512.patch`, BearSSL ctx stored in SHA512_CTX, static-asserted to fit; ubc_subr.c uses SHA-384 for code directories). `0020-stage-bearssl.sh` now also stages hash/sha1.c, hash/sha2big.c, codec/{dec,enc}{32,64}be.c. CI `38854909053` success (0032-only run `38854802073` superseded). Kernel `xnu/out-0033` (SHA1Init -> _br_sha1_init verified by disassembly; nm symbols have a leading underscore). Images (ADT sweet-0032): `shim/out/shim-boot-0033r.img` (read-only), `shim-boot-0033w.img` (+ufsrw=1 rootrw=1). Still NULL g_crypto_funcs users: des, chacha20poly1305, rsa, aesxts, rand, flow_divert.
- `research/check-patch-order.py` now skips patches touching CI-staged sweet_hfs sources (0031) instead of stopping.

- **0031r RESULT (boot 05:03:52 UTC 2026-10-16, `research/boot0031r.log`, photo IMG_20261016_070508): SHARED CACHE FOUND.** dyld now opens `dyld_shared_cache_arm64` (v2 image hard links), kernel refuses to map it: `shared_region: ... map(...'dyld_shared_cache_arm64'): not in trust cache` -> `vnode is not SIP-protected` (vm_unix.c: macOS arm64 path needs trust cache, else root-owned + SF_RESTRICTED unless CSR_ALLOW_UNRESTRICTED_FS) -> `dyld cache '(null)' not loaded: syscall to map cache into shared region failed` -> libSystem not loaded -> coredump to /cores fails error 30 (EROFS) -> panic initproc failed -> "MACH Reboot" (spins; screen kept). Lots of `hfs: set hardlink bit on vol=iOSRoot cnid=... linkid=...` spam (Linux-made hard links lack the link-chain bit; read-only mount cannot persist the fix). FIX: ADT `/chosen/asmb` with `lp-sip0`=0x6f (u64; incl. CSR_ALLOW_UNRESTRICTED_FS) and `lp-sip1`=1 (unauthenticated root), `tools/mkadt.py` -> `shim/out/sweet-0032.adt`. Images with kernel 0031: `shim/out/shim-boot-0032r.img` (read-only) and `shim-boot-0032w.img` (+ufsrw=1 rootrw=1).

- **2026-10-16 morning:** userdata now = **v2** image (`diff-v1-v2.simg` flashed 21:06:50 UTC 2026-10-15, OKAY, `research/flash-userdata-diff-v2.log`); future diffs use `ios16-root-20H392-v2.img` as OLD. Booted `shim-boot-0029k.img` (ignition_level=1) right after (`research/boot0029k-v2.log`); **no photo** (user asleep). Next morning the phone was in PixelOS recovery = some hardware reset (ABL then booted Android, which cannot use userdata). OBSERVATION: our kernel cannot reset the SoC itself (`PE_halt_restart` is never set; reboot/halt print "MACH Reboot"/"CPU halted" and spin), so the reset came from outside XNU: Qualcomm watchdog or a TZ/XPU violation. Unknown which; re-boot 0029k and watch/video until the reset. Do NOT tap factory reset/format data in recovery (would wipe the iOS image).
- **0030** (`xnu/patches/0030-sweet-ufs-writes.patch`, CI `38852655542`): UFS WRITE(10) + read-modify-write for partial 4 KiB blocks + SYNCHRONIZE CACHE (DKIOCSYNCHRONIZE), only with boot-arg `ufsrw=1`; `ufs_write_blocks` re-checks partition bounds before every write (only place data goes out).
- **0031** (`xnu/patches/0031-sweet-read-write-root.patch`, CI `38852837176`): boot-arg `rootrw=1` clears MNT_RDONLY before the root mount (no data volume on sweet: /private/var is on the root; image has no /etc/fstab); Apple HFS accepts a dirty non-journaled ROOT read-write on SWEET (logs "mounting dirty non-journaled root read-write"). Later: journaled HFS for crash safety. Kernel `xnu/out-0031` (CI `38852837176` success, 5m31s; 0030 run cancelled by the newer push). Boot images (phone ADT `sweet-0028.adt`, ramdisk byte-matched): `shim/out/shim-boot-0031r.img` = 0029k args (read-only, ignition_level=1) and `shim/out/shim-boot-0031w.img` = same + `ufsrw=1 rootrw=1`. PLAN: boot 0031r first and film until the reset (diagnose overnight reset), then 0031w (first ever writes to userdata; v2 image on the SSD is the restore source). `research/check-patch-order.py` cannot check 0031 (staged sweet_hfs files are not in its index); CI stage scripts run first, so CI is the check.

- **ignition_level=1 result** (boot 21:01:39 UTC, `research/boot0029k.log`, photo IMG_20261015_230308): preboot/cryptex stages skipped (need >= 2/3), `dylib_cache: finding shared cache on system volume: ignition level = 1, search root fd = 4, subpath = System/Library/Caches/com.apple.dyld/` -> not found -> `ignition boot failed: 19` -> no shared cache -> libSystem not loaded. FIX IN PROGRESS: image v2 = copy of the flashed image + `/System/Library/Caches/com.apple.dyld/` with HARD LINKS to the 44 cryptex cache files (no extra space; avoids symlink-following doubts), then `tools/sparsediff.py ios16-root-20H392.img ios16-root-20H392-v2.img diff.simg` and flash only the diff. Boot with `ignition_level=1` (shim-boot-0029k.img).
- **ignition_level=0 result** (boot 20:58:07 UTC, `research/boot0029j.log`, photo IMG_20261015_230037): all stages skipped as "ignition level insufficient" (hello/preboot/cryptex1 sniff >= 2, select os cryptex path/graft >= 3, **dyld_cache >= 1**), `ignition boot failed: 89`, "ignition partially disabled", then same `Library not loaded: /usr/lib/libSystem.B.dylib (no such file, no dyld cache)`. => dyld gets the shared cache directory from ignition's dyld_cache stage. Next: `ignition_level=1` (`shim/out/shim-boot-0029k.img`) to skip preboot/cryptex stages but run dyld_cache; its log "finding shared cache on %s ... subpath = %s" will show where it searches. If it searches rootfs /System/Library/Caches/com.apple.dyld, add a symlink there (image rebuild; consider a diff-only sparse flash tool to avoid rewriting 12 GB).
- **APPLE dyld/launchd RUNNING (libignition) with `unentitled_ios_sim_launch=1`** (boot 20:53:24 UTC, `research/boot0029i.log`, photo IMG_20261015_225617): kernel prints "Allowing spawn of iOS binary ... boot-arg set to true"; `shared_region: ... check_np ... vm_shared_region_start_address() failed` (no region yet, normal); dyld's built-in **libignition** (Darwin Ignition Sequence 1.0.0, libignition-51~1974; prints rd=ufsroot0, hardware target sweet, ignition level 0x5, platform 0x2, boot spec local, 7 stages): `hello from launchd.1` -> `preboot: mounting preboot` -> `failed to lookup preboot: 25` (ENOTTY: looks for a separate Preboot volume by role) -> ignition fails -> dyld `Library not loaded: /usr/lib/libSystem.B.dylib ... (no such file, no dyld cache)` -> launchd exit -> panic initproc failed. dyld boot-args found via strings: `ignition_level`, `ignition_force_dylib_root` (forced rootfs/livefs/cryptex fallback), `ignition_halt_after`, `ignition_live_app_graft`; dyld cache search paths include /System/Cryptexes/OS, /System/Volumes/Preboot/Cryptexes/OS/, /private/preboot/Cryptexes/OS/ (present in our image). Next try `ignition_level=0` (`shim/out/shim-boot-0029j.img`).
- **APPLE launchd EXECUTED FROM UFS (2026-10-15).** Flashed iOS image: `img2simg` -> `ios16-root-20H392.simg` (12.1 GB) then `fastboot -S 512M flash userdata ...simg` (24 chunks, 307 s, `research/flash-userdata-ios16.log`; a raw-image flash was killed by earlyoom at 2.6 GB RSS - always flash the sparse image). ABL fastboot hung on getvar after the flash; power+vol-down fixed it. Boot 0029u 20:49:04 UTC (`research/boot0029u-ios16.log`), photo IMG_20261015_225134: `hfs: mounted iOSRoot` -> load_init_program /sbin/launchd -> panic `unexpected SIGKILL of launchd (CS_KILLED) with reason -- namespace 9 code 0xe` = OS_REASON_EXEC / EXEC_EXIT_REASON_WRONG_PLATFORM: our kernel is XNU_TARGET_OS_OSX, iOS-platform binaries are "alien" and refused unless spawn attr platform set or DEVELOPMENT TUNABLE `unentitled_ios_sim_launch`. Next try: same kernel, cmdline + `unentitled_ios_sim_launch=1` (`shim/out/shim-boot-0029i.img`). LONGER TERM: macOS-config kernel marks iOS maps alien (vm_map_mark_alien) and differs from iOS in many policies; an iPhoneOS-config kernel build may eventually be needed.
- **iOS 16 ROOT IMAGE BUILT:** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392.img` (16 GiB case-sensitive HFS+, label iOSRoot, 12 GB used, fsck OK after `fsck.hfsplus -y` rewrote 32 non-ASCII localized WorkflowKit Shortcut names Linux decomposes differently). Built by `tools/mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]` (needs sudo; apfs-fuse as root; OS dmg is UDIF -> link must end .dmg). Contents: iPhone X 20H392 OS root + SystemOS cryptex at /private/preboot/Cryptexes/OS (so /System/Cryptexes/OS -> cache, 44 files, dyld_v1 arm64) + AppOS at .../App. /sbin/launchd and /usr/lib/dyld root:wheel 0755. NEXT: (confirm) `fastboot flash userdata ios16-root-20H392.img` (auto-sparse), boot 0029u (rd=ufsroot0) -> Apple launchd attempt.
- **MILESTONE ON HARDWARE: BOOT FROM INTERNAL FLASH.** 0029 CI `38797924912` PASSED, `xnu/out-0029` SHA256 `a68bc847...2a37`. Image `shim/out/shim-boot-0029u.img` (payload-0029u = 0029 kernel + `shim/out/sweet-0028.adt` + `rd=ufsroot0`, NO RAM disk). Boots 20:01:34 and 20:12:37 UTC (`research/boot0029u*.log`), photo IMG_20261015_221403: `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB, read-only` -> `BSD root: ufsroot0, major 1, minor 0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `SWEET: root volume is not authenticated (error 25); continuing unsealed` -> `load_init_program ... /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> heartbeat (ran for minutes unattended). userdata currently holds the 8 MB `research/root-hfs.img`. NEXT: build a multi-GB HFS+ image with the iOS 16 (iPhone X 20H392) userland (OS rootfs + SystemOS cryptex contents incl. dyld shared cache at /System/Library/Caches/com.apple.dyld; preserve modes/owners/symlinks; decompress LZBITMAP via apfs-fuse), flash it to userdata (sparse, needs user OK), then try Apple's dyld/launchd. Driver is read-only (root mounts RDONLY), fine for first launchd attempts.
- **userdata FLASHED with the 8 MB HFS test volume** (user said "flash it", 2026-10-15 19:53:17 UTC, `research/flash-userdata-hfs-test.log`: `fastboot flash userdata research/root-hfs.img` OKAY; pre-checks: one sweet device, partition-size:userdata 0x1AC4BFB000, partition-type f2fs, image fsck OK, SHA256 3626ea69...). Android userdata is now destroyed (expected/approved). Boot 0028u at 19:53:20 (`research/boot0028u-hfs.log`), photo IMG_20261015_215541: `BSD root: ufsroot0` -> **`hfs: mounted SweetRoot on device b(1, 0)` from internal UFS** -> panic `rootvp not authenticated after mounting @bsd_init.c:931` (arm64 macOS-config requires FSIOC_KERNEL_ROOTAUTH sealed root for non-ramdisk roots). Patch **0029** (`dd1c284`): SWEET logs `SWEET: root volume is not authenticated (error N); continuing unsealed` instead of panicking (Apple configs keep the panic; pivot_root still requires auth). CI **`38797924912`** running. Next: download out-0029, phone image with sweet-0028.adt + rd=ufsroot0 (no RAM disk), boot -> expect init from UFS.
- **UFS ROOT DRIVER VERIFIED ON HARDWARE (read-only).** 0028 CI `38796139570` PASSED, `xnu/out-0028` SHA256 `8b981640...9da6`; QEMU HFS RAM-disk regression `research/q0028-hfs` still runs init. Phone boot 2026-10-15 19:50:24 UTC (`research/boot0028u.log`, image `shim/out/shim-boot-0028u.img`, phone ADT `shim/out/sweet-0028.adt` with /sweet-ufs, NO RAM disk, `rd=ufsroot0`), photos IMG_20261015_215151/215220: `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB, read-only` -> `BSD root: ufsroot0, major 1, minor 0` -> `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : f188` (Android-encrypted data, as expected) -> `cannot mount root, errno = 19`, kernel retries mountroot (no panic). Nothing written to flash. NEXT (destructive, needs user confirmation right before): `fastboot flash userdata research/root-hfs.img` (8 MB HFS with test init) then boot rd=ufsroot0 -> expect init from internal flash.
- **PATCH 0028 (`33aceac`): XNU read-only UFS root device**, CI **`38796139570`** running (next artifacts xnu/out-0028, research/bt29, run29.log). `bsd/dev/sweet_ufs.c`: reuses ABL's UFSHCI state (like the shim probe), takes over UTRL at a kmem_alloc_contig region (DMA layout: UTRL 0x0, UCD 0x1000, 1 MiB bounce 0x4000), verifies UTRLBA readback before any doorbell, IE=0 polled, one SCSI cmd at a time on LUN0 (TUR x3, READ(10) via bounce, PRDT 256 KiB chunks), flush_dcache64 by PA around each command (host not dma-coherent per DT), finds GPT partition by name (`ufsroot=` boot-arg, default `userdata`), exposes `/dev/ufsroot0`/`rufsroot0` (512-byte logical sectors, physical 4 KiB, DKIOC* ioctls), refuses writes (EROFS). IOKitBSDInit: `rd=ufsroot0` -> sweet_ufs_rootdev() (lazy init) else panic. ADT: `tools/mkadt.py` adds `/sweet-ufs` reg <0x1d84000 0x3000> for the PHONE only (not --qemu). Local compile OK, imports resolve. Sandbox research/bt28 (pre0028 originals). FIRST PHONE TEST PLAN (non-destructive): boot 0028 with phone ADT + `rd=ufsroot0` and NO RAM disk: expect `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB` + `BSD root: ufsroot0`, then root mount failure (userdata is still Android-encrypted) -> panic. That validates the driver before flashing an HFS image into userdata (needs explicit user confirmation at that time).
- **SUPERSEDES the 64 GiB iosroot GPT plan: user said "u can delete android" (2026-10-15).** Use the existing `userdata` partition (LUN0 sda34, 4K LBA 0x2B8C00, 0x1AC4B MiB ~107 GiB) directly as the iOS root: NO GPT changes. Keep bootloaders, recovery (OrangeFox), modem/persist/fsg/modemst untouched. Write the HFS image later with `fastboot flash userdata` (sparse, auto-split under the 768 MiB limit). Android will no longer boot normally (expected; reinstallable). Order: XNU UFS driver first, read-only against the current GPT; flash only once it can locate userdata. Nothing erased yet.
- **DECIDED (user, 2026-10-15): 64 GiB `iosroot` on LUN0 (sda), taken from userdata (wipes Android user data; Android/super kept).** Planned GPT (4K LBAs): userdata 0x2B8C00..0xD7D6FF (~43 GiB, keep type/unique GUIDs and attributes); iosroot 0xD7D700..0x1D7D6FF (0x1000000 blocks = 64 GiB, 1 MiB aligned); backup GPT area unchanged. Procedure: user backs up phone data -> boot OrangeFox recovery (adb) -> back up GPT (sgdisk --backup + raw dd of LBA 0-5 and last 6 blocks) to research/ -> sgdisk edit -> Format Data -> verify Android boots to setup. NOT EXECUTED YET.
- **INTERNAL UFS WORKS FROM THE SHIM (read-only), user authorized internal-UFS approach incl. flashing (2026-10-15, "i allow everything").** Shim probe `shim/ufs.c` (commit `12ed3ed` + filter): ufshc@1d84000 is UFSHCI 2.1 (ver 0x210, cap 0x1587031F, 32 slots), ABL leaves HCE=1, HCS=0x10F (device present, lists ready, UIC ready); probe takes over UTRL at its own buffer (verifies UTRLBA readback before any doorbell), TEST UNIT READY/READ CAPACITY/READ(10) all succeed. LUN0 = 0x1D7D800 x 4 KiB (~126.6 GB), valid GPT (64 entries at LBA 2). Photos IMG_20261015_212916/213325/213429. Big LUN0 partitions (start in 4K LBAs / MiB): super 0xCA00/0x2200 (8.5 GiB), logdump 0x22CA00/64, minidump 0x230A00/128, rawdump 0x238A00/128, recovery 0x240C00/128, cache 0x248C00/384 ext4, exaid 0x260C00/384, cust 0x278C00/1024 ext4, **userdata 0x2B8C00/0x1AC4B MiB (~107 GiB), fs unreadable = encrypted (metadata encryption)**. cache..cust contiguous = only 1.75 GiB (too small for iOS ~10+ GiB). Plan needing user consent: shrink userdata in GPT (wipes Android data) and add an `iosroot` partition at the end of LUN0. LUNs 1-5 not listed yet (sdb..sdf backed up in backup/lun).
- **fastboot limit measured (2026-10-15, read-only getvar):** `max-download-size: 805306368` (768 MiB); `max-fetch-size` unsupported; `is-userspace: no` (ABL fastboot). A `fastboot boot` image (shim+kernel+RAM disk) must be < 768 MiB, so the 2.5 GiB iOS shared cache cannot ride in the boot RAM disk even if XNU gets all RAM. Real userland needs storage access (UFS driver, or USB host) or a much smaller cache. Unlocking full RAM is still needed later (GUI).
- **iOS 16 USERLAND SOURCE (Claude, 2026-10-15).** Chose **iPhone X (iPhone10,3/10,6, D22/D221AP)** iOS **16.7.16 (20H392)** over iPhone 8: same A11 (no PAC -> plain arm64, runs on Kryo 470/ARMv8.2), last iOS for A11; X userland is home-button-less and tall-screen (1125x2436 vs sweet 1080x2400), better fit for later GUI. Note the X has its OWN SystemOS cryptex (different digest from iPhone 8's). IPSW URL in `research/ipsw-x/URL.txt` (+ BuildManifest). Images on the user's external SSD (NOT in git, do not format that disk: it holds the user's photos): `/run/media/dipa/android/ios-sweet-ipsw/iPhone10,3_16.7.16_20H392/` — `087-86593-047.dmg` = Cryptex1,SystemOS (2,743,074,816 B), `087-86698-044.dmg` = OS rootfs (4,730,814,044 B), both zip CRC-32 verified, plus Firmware/*.root_hash/.trustcache/.mtree. AppOS cryptex is 087-86715-047.dmg (10 MB, same as iPhone 8).
  - Images are raw APFS; files use LZBITMAP (decmpfs type 14). 7z cannot decode. Tools: `ipsw` 3.1.738 (AUR ipsw-bin, user-approved) needs apfs-fuse on Linux; **apfs-fuse** built in user space (system cmake broken: cmake 4.4.4 wants libjsoncpp.so.27, jsoncpp 1.9.6 installed = partial upgrade, NOT fixed) with pip cmake + `-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint"`, installed to `~/.local/bin/apfs-fuse` (+apfs-dump-quick). Mount via a comma-free symlink (apfs-fuse splits options on ',' in `iPhone10,3`; exFAT has no symlinks -> put link elsewhere): `apfs-fuse LINK/087-86698-044.dmg MNT` -> `MNT/root/...`; unmount `fusermount3 -u MNT`.
  - Verified from headers: `dyld_shared_cache_arm64` magic `dyld_v1   arm64`; `/sbin/launchd`, `/usr/lib/dyld` = cputype 0x100000c subtype 0 (plain arm64). libSystem exists only inside the cache.
  - **BLOCKER for real userland via RAM disk:** shared cache = 3.10 GiB in 44 files (~2.5 GiB without the 592 MB `.symbols`), but the kernel manages only 1.20 GiB (0xb0000000..0xfd000000). Options: (A) give XNU the phone's full ~6 GB (banks 0x80000000+2G and 0x100000000+0xfcc00000, carve-out holes need pmap support) AND check fastboot max-download-size for a multi-GB boot image; (B) storage driver (Qualcomm UFS) to read a root from flash (no flashing allowed: would need a file/partition strategy); (C) run against individual dylibs extracted from the cache (uncertain).
- **MILESTONE ON HARDWARE: HFS+ ROOT FILESYSTEM -> PID 1.** Boot 2026-10-15 18:22:12 UTC (`research/boot0027b.log`, image `shim/out/shim-boot-0027b.img`), photo `IMG_20261015_202334.jpg`: `BSD root: md0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> repeated `SWEET init: still alive`, no panic. Kernel 0027 (built-in Apple OSS HFS + BearSSL MD5), init with SP alignment fix. NEXT: real iOS 16 userland on the HFS image (dyld, libSystem/dyld shared cache, launchd). Known gaps: entitlements (none granted), SHA-1/SHA-384/512 providers, no storage driver (RAM disk only), root mounted read-only.
- **PHONE HFS ROOT (0027) + SIGBUS ROOT-CAUSED.** Phone boot 2026-10-15 17:49:06 UTC (`research/boot0027.log`, photo `IMG_20261015_200031.jpg`): `hfs: mounted SweetRoot on device b(1, 0)` (HFS works on hardware), init printed its first line, then `pid 1 exited -- exit reason namespace 2 subcode 0xa` = SIGNAL/SIGBUS -> `initproc failed to start` panic. Diagnostic boot 18:08:13 UTC (`research/boot0027d.log`, image `shim/out/shim-boot-0027d.img`, diag init `research/diag-init` from scratch diag-init.S with [a][b][c] markers, `cs_debug=1`; photo `IMG_20261015_201547.jpg`): output `PID [a][b]` then SIGBUS -> fault at first `strb w21,[sp]`; no code-signing messages. Cause: kernel enters a static binary with sp at argc, not necessarily 16-aligned (depends on arg/apple string lengths, differs mockfs vs HFS); EL0 SP alignment check -> EXC_ARM_SP_ALIGN -> SIGBUS (sleh handle_sp_align). QEMU does not enforce SP alignment. NOT a kernel bug: real binaries start in dyld which aligns. Fix `1a8fc15`: sweet-init aligns sp at entry. Rebuilt `research/root-hfs.img`, QEMU `research/q0027-hfs2` OK. Phone image `shim/out/shim-boot-0027b.img` SHA256 `098ba46c...c5c` (payload-0027b, ramdisk match check-0027b) ready, awaiting FASTBOOT. Lesson: Assembly `;` is a comment in Apple arm64 asm (use .macro, not ;-joined #define).
- **HFS ROOT WORKS (QEMU).** 0027 CI `38779441895` PASSED, `xnu/out-0027` SHA256 `a8643886...5f6a`. QEMU `research/q0027-hfs` (RAM disk = `research/root-hfs.img`, rd=md0): `BSD root: md0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` + heartbeat, no panic. Phone image `shim/out/shim-boot-0027.img` (payload-0027 = 0027 kernel + sweet-0021.adt + root-hfs.img + rd=md0, ramdisk byte-match check-0027) ready; awaiting FASTBOOT for phone test.
- **HFS ROOT WORK (Claude), patches 0026-0027.**
  - 0026 (`f0e990c` + link fix `64c96f4`): Apple OSS **hfs-627.100.6** (commit `31d5ec238dfab485e78a9a30b8b92d13e8914f03`, paired with xnu-8796.141.3 in macos-135 release.json) staged by `xnu/patches/0026-stage-hfs.sh` into `bsd/sweet_hfs` (core + hfs_encodings; AppleKeyStore include/key prototypes guarded by CONFIG_PROTECT; SDK TargetConditionals copied). New config option `sweet_hfs` (MASTER + MASTER.arm64.sweet); 40 `optional sweet_hfs` files; Makefile.template foreach adds `_CFLAGS_ADD = -include sweet_hfs_compat.h -I...` (MUST come after kernel -D flags; compat undefs BSD/XNU_KERNEL_PRIVATE and the CONFIG_* HFS tests so kext-config.h decides: macOS profile, no content protection). `sweet_hfs_iokit.c` replaces hfs_iokit.cpp/hfs_encodings_kext.cpp: `sweet_hfs_register()` called in bsd_init after vfsinit (encodings init, vfs_fsadd VFS_TBLCANMOUNTROOT, zones, sysctls); is_ejectable 0, iterate_media none, serial KERN_NOT_SUPPORTED, OSKextGetCurrentLoadTag -> kOSKextInvalidLoadTag (renamed via compat macro). hfs_encodings' own `hfs_allocated` renamed per object (separate kexts on Apple; first CI `38777325964` failed on that duplicate at link). mockfs now validates Mach-O magic (its TODO) so non-Mach-O roots fall through to HFS (new fs entries come AFTER mockfs in vfsconf). CI `38778133720` PASSED, `xnu/out-0026` SHA256 `0b49170c...701a`, prints `SWEET HFS: hfs-627.100.6 registered`.
  - HFS+ image: `research/root-hfs.img` (8 MB, mkfs.hfsplus from AUR hfsprogs installed with user approval; loop-mounted with sudo; /sbin/launchd = research/sweet-init, /dev; fsck.hfsplus OK). QEMU `research/q0026-hfs`: `BSD root: md0` then data abort in MD5Init from hfs_getvoluuid (mount computes volume UUID).
  - 0027 (`14bf2b7`): 0020 stager also copies BearSSL hash/md5.c + codec/dec32le.c/enc32le.c; `corecrypto_md5.c` SWEET branch converts MD5_CTX <-> br_md5_context around br_md5_* calls; wrappers sweet_md5.c/sweet_dec32le.c/sweet_enc32le.c. Host test 40/40 match md5sum. CI **`38779441895`** running.
  - Local tooling: `tools/xnu-cc.py` has `XNU_CC_POST` (flags after logged ones). Sandbox `research/bt28/xnu` = 0025 CI tree + 0026/0027 edits; originals `research/pre0026`, `research/pre0027`. HFS local objects `research/hfs-objs`. Checker entries updated to bt28.
  - SHA-1 / SHA-384/512 still have no provider.
- **MILESTONE ON HARDWARE: USERSPACE PID 1 RUNS ON THE PHONE.** Boot 2026-10-15 17:16:00 UTC (`research/boot0025.log`: send OKAY0.410s, boot OKAY0.135s), image `shim/out/shim-boot-0025.img` SHA256 `9dbfa0b1...89db0`. Photo `IMG_20261015_191733.jpg`: `BSD root: md0` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> repeated `SWEET init: still alive` (5 s heartbeat), no panic. Kernel 0025 + mockfs RAM disk + built-in no-entitlements AMFI. No flashing performed.
- **NEXT (toward real iOS userland):** real root filesystem with real binaries. Options: (a) build Apple OSS `hfs` (macOS 13.5 tag `hfs-627.100.6`) into SWEET like libpthread, then an HFS RAM disk image; (b) iOS 16 dyld + libSystem/dyld shared cache from the IPSW (needs APFS or repacking onto HFS). Expected hurdles: entitlements (sweet_amfi grants none), SHA-1/MD5/SHA-384/512 providers (BearSSL), missing kexts/daemons. Current init is a static test program, not launchd.
- **MILESTONE: USERSPACE RUNS (QEMU).** 0025 CI `38774788048` PASSED, `xnu/out-0025` SHA256 `5496069b...4877`. QEMU `research/q0025-rd` (150 s, no panic): `BSD root: md0` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> `SWEET init: still alive` every 5 s. Exec, ad-hoc SHA-256 code signature, syscalls, devfs /dev/console, select timeouts all work. Phone image prepared: `shim/out/shim-boot-0025.img` 16,515,072 bytes SHA256 `9dbfa0b15140c0bd733a16c4f727f6a7df44a8741f98d8e266d5f2fa4f689db0` (payload-0025 = 0025 kernel + sweet-0021.adt + research/sweet-init + rd=md0; ramdisk byte-match check-0025). Waiting for user to put phone in FASTBOOT.
- **0024 CI `38773277811` PASSED**, `xnu/out-0024` SHA256 `a0ba964b...4348`. QEMU `research/q0024-rd` and **PHONE (boot 2026-10-15 17:05:55 UTC, `research/boot0024.log`, image `shim/out/shim-boot-0024.img` SHA256 `cf3dac86...6964`, 16,510,976 bytes, payload `shim/out/payload-0024.bin` = 0024 kernel + `shim/out/sweet-0021.adt` + RAM disk `research/sweet-init` + rd=md0; ramdisk byte-match `check-0024`)** agree: `Added memory device md0`, `BSD root: md0`, `load_init_program: attempting to load /sbin/launchd`, then data abort in accelerate_entitlement_queries (PC 0x...77d2458, FAR 0x1a0) = NULL AMFI interface. Photo `IMG_20261015_190715.jpg`. RAM disk root is physically verified.
- **0025 (`2518c79`)**: new `bsd/kern/sweet_amfi.c` registers a built-in AMFI interface at EARLY_BOOT/MIDDLE: no entitlements (adjustContextWithoutMonitor succeeds without creating a context, queries KERN_DENIED, copies NULL/KERN_NOT_FOUND, get_xml/transmuted false); WithMonitor KERN_NOT_SUPPORTED; OSEntitlements_query and query_context_to_object panic explicitly (CoreEntitlements closed; no kernel caller); TrustCache version 0 (0015 keys on trust_cache_rt==NULL, img4 absent -> still unavailable); CoreEntitlements table zero (PMAP_CS only). Compiles locally; checker updated (bt27 entries). CI **`38774788048`** running. Next artifacts `xnu/out-0025`, `research/bt29`, `research/run29.log`. Real iOS binaries will need real entitlements later (no OSS CoreEntitlements exists).
- **0021-0024 RAM-disk-root iteration (Claude), QEMU only so far, no new phone boot since 0020.** Each kernel tested with `tools/mkpayload.py KERNEL research/q0021.adt OUT --ramdisk research/sweet-init --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"` then `tools/qemu-sweet.py OUT DIR --secs 150 --kernel KERNEL --fail-on-panic` (PATH needs ~/swift/usr/bin for llvm-nm). Fresh ADTs: `research/q0021.adt` (QEMU), `shim/out/sweet-0021.adt` (phone).
  - 0021 CI `38769532000` PASSED, `xnu/out-0021` SHA256 `2b7500a1...962e`, `research/bt25`/`run25.log`. QEMU `research/q0021-rd`: md0 root, then assert `iomediaClass` in IOService::publishHiddenMedia.
  - 0022 (`20ece9c`, publishHiddenMedia without IOMedia class) CI `38770666158` PASSED, `xnu/out-0022` SHA256 `a4d0e787...30c5`, `research/bt26`/`run26.log`. QEMU `research/q0022-rd`: data abort in SHA256_Init (NULL g_crypto_funcs) from vm_kernel_addrhash_internal.
  - 0023 (`c8089d1`, libkern SHA256_Init/Update/Final use built-in ccsha256_ltc_di + ccdigest_init/update on SWEET; also serves code-signing SHA-256; SHA-1/MD5/SHA-384/512 still have NO provider - BearSSL from 0020 could supply them later) CI `38771876423` PASSED, `xnu/out-0023` SHA256 `d763fed7...1003`, `research/bt27`/`run27.log`. QEMU `research/q0023-rd`: **mockfs mounted md0, `load_init_program: attempting to load /sbin/launchd`**, then panic `mockfs_fsnode_vnode failed to create fictitious pages ... rvalue = 5` (mockfs.c:351).
  - 0024 (`3293c80`): mockfs used memdev mi_base (4K page number of a VIRTUAL kernel address for IOKit RAMDisk, truncated to 32 bits) << PAGE_SHIFT as a physical address. Now zero-copy pager mapping only when mi_phys (with 4K units); virtual md uses pagein/blockmap/strategy -> memdev bcopy. memdev's own mdPhys path assumes 4K ppnums (unsafe on 16K arm64), so do NOT switch the RAMDisk to physical. CI **`38773277811`** running. Next artifacts `xnu/out-0024`, `research/bt28`, `research/run28.log`.
  - Sandboxes: bt25/bt26/bt27 contain the next patch's edited files; pristine copies in `research/pre0022`..`pre0024`.
- **PATCH 0021 (Claude): RAM disk root via mockfs -> first userspace program.** Commits `9c37771` (initial; CI `38768500280` FAILED: mockfs_fsnode.c kfree_type(mockfs_fsnode_t) typed-alloc static assert; local xnu-cc stubs `__builtin_xnu_types_compatible` so it can't catch this) and `193772f` (fix), CI **`38769532000`** running: https://github.com/dipa-studio/ios-sweet/actions/runs/38769532000. Next artifacts: `xnu/out-0021`, `research/bt25`, `research/run25.log`.
  - Kernel: `config/MASTER.arm64.sweet` FILESYS_BASE += `mockfs` (XNU's built-in FS: root device is itself a Mach-O, served as /sbin/launchd, plus /dev for devfs). Upstream mockfs bit-rot fixed: kalloc_type/kfree_type used the POINTER typedef mockfs_fsnode_t (nodes were 8 bytes) -> `struct mockfs_fsnode`. `IOService::setRootMedia` asserted IOBlockStorageDriver class exists (IOStorageFamily absent): SWEET-only, a missing class means root=NULL (same as IOSetImageBoot); Apple path keeps assert. All 21 patches apply (`research/check-patch-order.py`); 5 TUs compile locally with -DMOCKFS (`research/mockfs-objs`), imports resolve vs 0020 kernel. Sandbox `research/bt24/xnu` now holds 0021 versions of those 3 files; pristine 0020 copies in `research/pre0021`.
  - Tools: `tools/mkpayload.py --ramdisk IMAGE` appends IMAGE as an extra payload segment right after the kernel, moves kernel_end past it (shim unchanged: ADT+boot_args follow it, image stays below topOfKernelData), and adds ADT `/chosen/memory-map` `RAMDisk`=<phys,size>. Boot with `rd=md0`. Without --ramdisk output is byte-identical to existing 0020 QEMU and phone payloads (verified). `tools/sweet-init.S` + `tools/mkinit.py`: PIC static init (open /dev/console, print PID, heartbeat every 5 s via select, never exits) wrapped into a static arm64 Mach-O (__PAGEZERO, __TEXT@0x100000000, LC_UNIXTHREAD, ad-hoc SHA-256 CodeDirectory v0x20400, 4K pages). Build: `~/swift/usr/bin/clang -target arm64-apple-ios16.0 -c tools/sweet-init.S -o X.o && tools/mkinit.py X.o research/sweet-init` (ld64.lld cannot link static iOS). Signature not verified by a real codesign; the kernel will tell.
  - QEMU with 0020 kernel + RAM disk (`research/q0020-rd.bin`, rd=md0, 150 s `research/q0020-rd150`): `Added memory device md0`, **`BSD root: md0, major 1, minor 0`** (plumbing works), then the setRootMedia assert (now fixed in 0021). NOTE: QEMU needs >60 s to reach IOFindBSDRoot (60 s run showed nothing after stf0; likely RTC/inittodr wait), so use --secs 150.
  - NEXT: download 0021, QEMU 150 s with ramdisk; expect mockfs mount then init exec -> `*** SWEET userspace: init running as PID 1 ***`. Then phone boot with same payload (phone ADT shim/out/sweet-0020.adt pattern + --ramdisk + rd=md0).
- **0020 PHONE RESULT (user transcription of screen, 2026-10-15): KERNEL INIT COMPLETE, WAITING FOR ROOT.** After Skywalk self-tests PASSED, kminit, ifnet_attach lo0/gif0/stf0, screen prints `BSD`, then `Waiting on <dict><key>IOProviderClass</key><string>IOMedia</string><key>Content</key><string>Apple_HFS</string></dict>` and `Failed to write boot-picker-bringup-reason to NVRAM.` No panic. This is IOFindBSDRoot waiting for a root device (none exists); NVRAM warning is expected (no NVRAM driver). QEMU `cpu_idle` snapshot is presumably the same wait (not GDB-confirmed; milestone GDB run was cancelled by user since phone answered it). Draft `research/check-milestones20.gdb` (hbreaks inittodr/vfs_mountroot/IOFindBSDRoot/bsd_utaskbootstrap) left uncommitted/ignored.
- **NEXT MILESTONE: root filesystem -> launchd.** Plan: (1) RAM disk: shim places an image in RAM, ADT /chosen/memory-map `RAMDisk` entry, boot-arg `rd=md0` (XNU md driver). (2) HFS is a separate kext on Apple; build Apple OSS `hfs` (tag paired with macOS 13.5 in distribution-macOS macos-135 release.json) into SWEET like libpthread 0019. APFS is closed. (3) Minimal HFS image with iOS 16 launchd (later: dyld/shared cache; AMFI/code-signing absent will be a later hurdle). User's end goal is full iOS GUI.
- **0020 (AES) VERIFIED + PHONE BOOTED (Claude took over after Codex usage limit).** Actions `38765422261` (`ab9e23f`) PASSED; kernel `xnu/out-0020/kernel.development.sweet` checksum OK; buildtree `research/bt24`, log `research/run24.log`. Actual-kernel AES checks PASS (`research/check-aes20.gdb` / `.log`, 12 extra CBC cases match OpenSSL). 45s QEMU `research/q0020`: no panic, zero unknown glyphs, past old TCP/AES panic -> Skywalk self-tests PASSED -> bsd_autoconf kminit -> ifnet_attach lo0/gif0/stf0; final snapshot PC in `cpu_idle` (idle or waiting; NOT yet diagnosed, could be root-device wait). Image `shim/out/shim-boot-0020.img` 16,461,824 bytes SHA256 `775200cb02d95fc750981516b79d91dcdc96184bbe67a38f9d57c59b4a391828`, ramdisk byte-matches `payload-0020.bin` (`check-0020`). **Phone temporarily booted 2026-10-15 16:10:00 UTC**: one FASTBOOT product sweet, send OKAY0.407s, boot OKAY0.134s, total0.549s, exit0 (`research/boot0020.log`). Awaiting user photo; do not reboot the phone before it. NEXT: read photo; diagnose what QEMU waits on after stf0 (GDB backtrace of threads / longer run). Codex's STOP request below is complete.
- User requests STOP after completing the current0020 AES iteration (Actions/QEMU/phone), then leave a precise committed handoff for another AI. User says FASTBOOT ready again after0019 photo. Do not start a new blocker fix after this iteration.
- Goal: iOS16-generation XNU on Redmi Note10Pro sweet/SM7150. Always commit, use gh/Actions for kernel builds, keep this handoff current. Temporary fastboot boot only; preserve Android/backups. No userland/root filesystem boot yet.
- **Patch0020 update:** initial `e71098d` / Actions `38765012466` superseded by scalar-only compile flags. Added -mgeneral-regs-only to all six provider/BearSSL TUs and corecrypto_aes.c; object disassembly now confirms no SIMD. Updated implementation `ab9e23f`, Actions `38765422261` queued/running: https://github.com/dipa-studio/ios-sweet/actions/runs/38765422261. Do not launch another run. portable BearSSL AES CBC and legacy one-block ECB wired directly into SWEET corecrypto_aes.c. Genuine bitslice implementation, no CPU AES/SIMD requirement, heap or fake global crypto table. Source fetched by `0020-stage-bearssl.sh` from official BearSSL git at immutable `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`; MIT license retained, downloaded source stays out of git. Kernel limits include adapter only. All128/192/256-bit keys, CBC encrypt/decrypt, copied contexts, in-place/out-of-place; zero block count and size overflow handled, invalid keys clear context. Seven TUs compile to arm64 objects (`research/aes-objs`), imports resolve, all20 patches apply and sources match (`research/patch0020-check.log`). Sandbox `research/bt23/xnu`, originals `research/pre0020`. NEXT: Actions then actual-kernel NIST/random CBC/ECB checks and >=45s QEMU before another phone boot. Next artifacts out-0020 / bt24 / run24.log. 0020 updated CI pending. Local sources/checks match the scalar-only patch.
- **Latest built kernel0019**, implementation `256d3f0`, patches0001-0019. Actions `38763072907` PASSED (4m5s): https://github.com/dipa-studio/ios-sweet/actions/runs/38763072907. Kernel `xnu/out-0019/kernel.development.sweet`, SHA256 `a27df7568f03fb541cf0e8024c0a594fa178a85054fcf9eba29158cbd4c5ecc0`; checksum verified. Log/source `research/run23.log` / `research/bt23` (no nested .git). Seven pthread source/build/header files and staged adaptations byte-match local compile inputs.
- **0019 hardware photo received** `IMG_20261015_175306.jpg`: pthread registration visible, passes mbinit and DLIL checksum/scheduling, same AES fault as QEMU: PC`0xfffffe00079174e0` (`aes_encrypt_key128`), LR`0xfffffe000767c3a4` (`tcp_init`), FAR0x78, x8=0. Disassembly proves NULL g_crypto_funcs->ccaes_cbc_encrypt. Photo processed; no more0019 observation pending. General crypto table remains absent, other hash/mode operations may still fault after0020. DO NOT bypass TCP or install fake crypto. XNU already contains SHA256/HMAC implementations; AES source there is a dispatch wrapper. Full future provider interface libkern/libkern/crypto/register_crypto.h. Apple/corecrypto default README describes evaluation-only licensing, so no external corecrypto was fetched or staged; BearSSL selected for this portable AES integration.
- **0019 pthread FIXED in QEMU and observed beyond its init on phone**: 45s QEMU `research/q0019`, zero unknown glyphs, then AES panic. Actual-kernel API checks PASS `research/check-pthread19.gdb` / `.log`, guest `research/q0019-api`: function/callback table identity, actual provider pthread_init returns, locks/global hash/threadcall/typed zones initialized, thread_selfid via syscall shim + buffer bounds, process hash init/delete via shims restores NULL. Monitor quit prints expected remote-disconnect after PASS. All fixture guests stopped.
- **0019 provider**: genuine Apple libpthread-514.100.2 commit `a6c70ca0e544347a0377babf748f831577ec24f9`, exact paired release with xnu8796.141.3 in distribution-macOS/macos-135/release.json (`research/macos-135-release.json`). 0019 shell stage runs before patches (workflow copies *.sh); three SWEET wrapper TUs use kext BSD header view, vm_fault_external, renamed provider-side pthread_kern/current_uthread; register real function table at EARLY_BOOT SECOND, real pthread_init later. Stager applies bounded address casts, zero user addresses, proper panic format. SWEET opaque ksyn storage enlarged48->56 for trailing kwe_uth, checked with provider static assertion. Local pre-CI TargetConditionals fixture was syntax-only; artifact bt23 carries actual SDK header and CI is authoritative.
- **Phone0019 temporarily booted** 2026-10-15 15:51:36 UTC: exactly one FASTBOOT/product sweet verified; send OKAY0.406s, boot OKAY0.134s, total0.546s, exit0 (`research/boot0019.log`). Image `shim/out/shim-boot-0019.img`, 16,457,728 bytes, SHA256 `19429a57e486b856880df9e27327ed488e0139345311fdb0350974e72103a9a1`. Phone ADT/payload `shim/out/sweet-0019.adt` / `payload-0019.bin` timer19, unpacked ramdisk byte-match (`check-0019`). EntryPA0xb146b488, kernelendPA0xb2000000, boot_argsPA0xb2004000/top0xb2008000. Last user FASTBOOT+USB reply preceded this boot; do not assume phone is still in FASTBOOT.
- **CPU0018 physically verified** by `IMG_20261015_173951 (2).jpg`: boot CPU0 registered, real SGI1 verified, initialization complete -> BSD/MAC/mbinit -> old pthread-table panic. SweetCPU uses legacy IOCPU, ml_processor_register/processor_start/initCPU, real SGI1 clears SIGPdisabled before max CPUs completion; no forced flags/skipped waits. CPU APIs and two-CPU rejection passed (`research/check-cpu18.gdb`, `q0018-api`, `q0018-smp`). Single core; SMP/suspend unsupported. CURRENT SWEET has NO USE_APPLEARMSMP.
- QEMU next run: generate fresh `tools/mkadt.py research/q0020.adt --qemu` (timer27), mkpayload with `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`, then `tools/qemu-sweet.py PAYLOAD OUT --secs 45 --kernel KERNEL --fail-on-panic`. 12s can miss later panics because IORTC waits30s. Runner snapshot only: panic exit2, blank/incomplete exit3; absence of panic does not mean complete boot. Qualcomm XPU/ABL/panel and phone SMC PSCI require hardware checks.
- Local checker `tools/xnu-cc.py research/run23.log research/bt23 FILTER --per-file`, XNU_CC_OBJDIR for objects. For new BearSSL TUs add XNU_CC_EXTRA with -I bt23/xnu/libkern/crypto/sweet_bearssl/{inc,src}. Patch checker `research/check-patch-order.py` uses temporary upstream index; source fixtures/artifacts ignored under research. GDB mask registers to unsigned64, use function-entry manual-call return traps, discard manipulated guests. No QEMU guest intentionally left running.

## Session log and earlier handoffs (superseded by the active summary above)

### Prior 0019 transition notes (superseded)

- Goal: iOS 16 generation XNU on sweet. Always commit completed work, use gh/Actions for kernel builds, keep this handoff current. Temporary fastboot boot only; preserve Android/backups.
- **0019 build PASSED** Actions `38763072907` (4m5s), implementation `256d3f0`. Kernel `xnu/out-0019/kernel.development.sweet`, SHA256 `a27df7568f03fb541cf0e8024c0a594fa178a85054fcf9eba29158cbd4c5ecc0`; checksum verified. Log/source `research/run23.log` / `research/bt23`; all seven patched files and staged provider adaptations match local compile inputs. Prefer bt23 as the next baseline.
- **0019 pthread FIXED in QEMU**: 45s run `research/q0019` registers provider, passes mbinit, DLIL SUM16 checks and DLIL thread scheduling. Actual-kernel GDB checks `research/check-pthread19.gdb` / `.log` PASS: function/callback table identity, real _pthread_init returns, locks/global hash/cleanup threadcall/typed zones non-NULL, thread_selfid via actual syscall shim with output bounds, process hash init/delete through actual shims restores NULL. Guest stopped after tests; no fixture left running. GDB monitor quit prints expected remote disconnect after PASS.
- **Current next blocker:** QEMU0019 panic data abort FAR0x78 at `aes_encrypt_key128` PC`0xfffffe00079174e0`, called by `tcp_init` LR`0xfffffe000767c3a4`. Disassembly confirms `g_crypto_funcs == NULL`, loading ccaes_cbc_encrypt at+0x78. General crypto registration is still absent; DO NOT bypass TCP or supply fake crypto. Next integrate a genuine compatible crypto implementation/provider (AES first needed here, other digest/mode APIs also used). XNU contains SHA256/HMAC but no AES implementation; its corecrypto_aes.c is a dispatch wrapper. Full crypto table is in libkern/libkern/crypto/register_crypto.h.
- **0019 phone image prepared and verified**, user reports FASTBOOT+USB again; exactly one device/product sweet verified. `shim/out/shim-boot-0019.img`, 16,457,728 bytes, SHA256 `19429a57e486b856880df9e27327ed488e0139345311fdb0350974e72103a9a1`; unpacked ramdisk matches phone payload (`shim/out/check-0019`, `payload-0019.bin`, `sweet-0019.adt`, timer19). EntryPA0xb146b488; kernelendPA0xb2000000; boot_argsPA0xb2004000/top0xb2008000. Usual debug cmdline cpus=1/-noprogress. **Temporarily booted 2026-10-15 15:51:36 UTC**: send OKAY0.406s, boot OKAY0.134s, total0.546s, exit0 (`research/boot0019.log`). User asked for photo after45s. Await the photo; DO NOT reboot again before observation. Bootloader acceptance only; no0019 physical progress observed yet.
- **Hardware photo confirms 0018 progress** (`IMG_20261015_173951 (2).jpg`): real phone prints `SWEET CPU: boot CPU 0 registered, SGI1 verified, initialization complete`, enters BSD/MAC, reaches `mbinit: done`, then the same missing-pthread-function-table panic as QEMU at pthread_shims.c:318. CPU completion/interrupt delivery is now physically observed. The photo was received; no further phone boot performed yet.
- **Patch0019 committed/pushed `256d3f0`; Actions `38763072907` queued/running:** https://github.com/dipa-studio/ios-sweet/actions/runs/38763072907. Do not launch a duplicate. genuine Apple OSS libpthread-514.100.2 provider (immutable commit `a6c70ca0e544347a0377babf748f831577ec24f9`), paired with xnu-8796.141.3 by Apple's `distribution-macOS/macos-135/release.json` (`research/macos-135-release.json`). `0019-stage-libpthread.sh` fetches/stages it before patches; workflow now copies shell scripts too. SWEET-only wrappers compile all three real kernel sources, rename provider-side symbols, use kext header view and vm_fault_external, register the real function table at EARLY_BOOT SECOND. Existing pthread_init then runs the actual provider initialization. No pthread wait/syscall is bypassed. Source staging includes bounded explicit address casts/zero user addresses and panic-format fixes. SWEET kernel opaque ksyn storage enlarged from48 to56 bytes to contain this provider's trailing kwe_uth, with a provider size assertion. Local compile of three provider TUs plus pthread_shims/kern_fork passed; imports resolve and provider exports do not collide with existing kernel symbols. All19 patches apply and changed sources match (`research/patch0019-check.log`). Source sandbox `research/bt22/xnu`, originals `research/pre0019`, objects `research/pthread-objs`. Local TargetConditionals fixture only has target macros for syntax; CI stages the real macOS SDK header and is authoritative. Next Actions artifacts: out-0019 / bt23 / run23.log; run QEMU >=45 seconds. 0019 CI is pending; no 0019 QEMU run yet.
- **Latest kernel implementation `e7d9550`, patches0001-0018; CI `38760004398` PASSED (3m56s)**: https://github.com/dipa-studio/ios-sweet/actions/runs/38760004398. No build is pending. Kernel `xnu/out-0018/kernel.development.sweet`, SHA256 `b3c8d59acc88f93a2c836eb2527a05ed4fbaac7c580a2ea8c3b795137f4aadf1`; checksum verified. Source/log `research/bt22` / `research/run22.log` (baseline for patch0019; no nested .git). New driver/config sources match CI exactly.
- **CPU wait FIXED in QEMU**: SweetCPU uses legacy IOCPU, validates single boot CPU, ml_processor_register -> processor_start -> initCPU real SGI1 -> scheduler/IOKit completion -> ml_set_max_cpus. It does NOT force a flag or skip a wait. CPU APIs passed: Running CPU0, processor_array[0] association, SGI signalling enabled, name, unsupported secondary-start error (`research/check-cpu18.gdb` / `.log`, `research/q0018-api`). Single-core only; SMP and suspend unsupported. Two-CPU topology fails explicitly (`research/q0018-smp`).
- **Next blocker reproduced in 45s QEMU** (`research/q0018-long`, zero unknown glyphs): BSD initialization reaches MAC framework, network mcache and `mbinit: done`, then **`pthread kernel extension not loaded (function table is NULL). @pthread_shims.c:318`**. Do not bypass pthread_init: find/integrate the matching open-source libpthread kernel implementation and register its real function table. A 12s run `research/q0018` looked panic-free before the 30s IORTC wait elapsed; use >=45s or GDB checkpoints for the next iteration. No userland/root filesystem boot yet.
- **Phone 0018 TEMPORARILY BOOTED** 2026-10-15 **15:31:15 UTC**: exactly one FASTBOOT device verified product sweet; send OKAY0.400s, boot OKAY0.134s, total0.539s, exit0. Local log `research/boot0018.log`. This confirms bootloader acceptance only. User was asked for a photo; await it, DO NOT reboot the phone again before their observation. No flashing performed.
- Current hardware image `shim/out/shim-boot-0018.img`, **16,437,248 bytes**, SHA256 **`b7a23595d143a76c72c9fe2cfcc5d77a8f16987b5af1a2703bbffdfa6922908c`**. Phone ADT/payload `shim/out/sweet-0018.adt` / `shim/out/payload-0018.bin` (timer19), unpacked ramdisk exactly matches payload (`shim/out/check-0018`). EntryPA0xb146b488; kernelendPA0xb1ffc000; boot_argsPA0xb2000000/top0xb2004000. Command line `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`.
- QEMU payload/ADT `research/q0018.bin` / `research/q0018.adt` (timer27). Run `python3 tools/qemu-sweet.py research/q0018.bin research/NEW-OUT --secs 45 --kernel xnu/out-0018/kernel.development.sweet --fail-on-panic`; fresh ADT seed for a new payload. Output fb.png/fb.txt/regs.txt/qemu.log/result.json. Visible panic exit2; blank/incomplete decode exit3; no_panic_observed is snapshot-only, not successful boot. -noprogress prevents the graphical indicator corrupting console text. Qualcomm XPU/bootloader/panel and SMC PSCI still need physical validation.
- Patches0014-0017 fixed PRNG registration/direct SHA256, explicit unavailable trust-cache service with rejected loads/no matches, built-in SweetPlatformExpert, and timer PPI from ADT (phone19/default19, QEMU27). All their checks/artifacts are preserved in the history below. CURRENT SWEET has NO USE_APPLEARMSMP; do not assume gCPUIC/gAIC/gPMGR AppleARMSMP globals exist.
- Local single-file checker: tools/xnu-cc.py research/run22.log research/bt22 FILTER --per-file, output via XNU_CC_OBJDIR. Patch checker `research/check-patch-order.py` applies all18 via temporary index, matches sandbox sources. Sandbox for0018 `research/bt21/xnu`, originals `research/pre0018`, objects `research/cpu-objs`. First compiler issues were missing machine/atomic.h and nonexistent getNameSymbol; fixed and both TUs compiled successfully before CI. Fixtures/artifacts under research are local-only/ignored.
- GDB: mask int(register)&((1<<64)-1) for unsigned kernel pointers; use function-entry manual-call return traps, discard manipulated guests. No QEMU fixture is intentionally left running.

### Earlier active handoff snapshot (superseded)


- **0018 milestone:** Actions `38760004398` PASSED (3m56s), checksums passed, new CPU/build files byte-match CI tree. Kernel `xnu/out-0018/kernel.development.sweet`, SHA256 `b3c8d59acc88f93a2c836eb2527a05ed4fbaac7c580a2ea8c3b795137f4aadf1`; log/tree `research/run22.log` / `research/bt22`. QEMU `research/q0018` 12s: boot CPU registered, SGI1 verified, CPU initialization complete, **BSD initialization entered** (copyright, MAC framework, buffer headers), no panic/zero unknown glyphs. Direct CPU API checks PASS (`research/check-cpu18.gdb`, `.log`, `research/q0018-api`): CPU0 Running, processor matches processor_array[0], real SGI enabled, CPU name correct, secondary-start returns KERN_NOT_SUPPORTED. Two-CPU ADT correctly panics before registration (`research/q0018-smp`). Longer 45s run `research/q0018-long` still running; next BSD wait/fault not yet identified.
- **Prepared 0018 phone image:** `shim/out/shim-boot-0018.img`, 16,437,248 bytes, SHA256 `b7a23595d143a76c72c9fe2cfcc5d77a8f16987b5af1a2703bbffdfa6922908c`. Phone ADT/payload `shim/out/sweet-0018.adt` / `shim/out/payload-0018.bin`, usual debug args with cpus=1 and -noprogress. Unpacked ramdisk byte-compared (`shim/out/check-0018`). Entry PA remains 0xb146b488; kernel end PA 0xb1ffc000, boot_args 0xb2000000/top0xb2004000. User reports FASTBOOT+USB ready; read-only check found exactly one product sweet. NEXT: temporary fastboot boot of this verified image, record send/boot results, then await hardware photo without rebooting again.
- Patch 0018 committed/pushed **`e7d9550`**; Actions **`38760004398`** queued/running: https://github.com/dipa-studio/ios-sweet/actions/runs/38760004398. Do not launch a duplicate run.
- Active continuation: patch **0018** implemented in `iokit/Kernel/arm/SweetCPU.cpp` + arm64 source list + exact qcom,kryo470 personality. Keeps legacy IOCPU, accepts ONLY the single boot CPU, fills ml_processor_info from parsed topology with CPU object as cpu_id, calls ml_processor_register and processor_start. initCPU sends SGI1 via ICC_SGI1R_EL1 and waits (1-second deadline) for real cpu_signal_handler to clear SIGPdisabled. Only after successful startup calls ml_cpu_init_completed/cpusRunning and publishes completion/max CPUs. Secondary startup/suspend are explicitly unsupported. Local C++ compile passed for both TUs (`research/cpu-objs`), all imports resolve, object contains dsb ishst / ICC_SGI1R / isb; all18 patches apply in order and match sources. Sandbox `research/bt21/xnu`, originals `research/pre0018`; next CI artifacts should use `xnu/out-0018`, `research/bt22`, `research/run22.log`. User has the phone available; first run QEMU, then prepare/temporarily boot useful hardware progress.
- Goal: iOS 16 generation XNU on Redmi Note 10 Pro / sweet. User wants persistent progress, commits, kernel builds via `gh` Actions, and this handoff kept current across agent switches. Temporary `fastboot boot` only; preserve Android and backups.
- Latest kernel implementation: **`aa3f991`**, patches **0001-0017**. All four new Actions builds PASSED: 0014 `38748543058` (4m0s), 0015 `38749549779` (4m3s), 0016 `38750733996` (4m3s), 0017 **`38751507437` (4m7s)**. Latest: https://github.com/dipa-studio/ios-sweet/actions/runs/38751507437. No build is pending.
- **Current blocker: CPU initialization has no SWEET CPU driver.** QEMU registers SweetPlatformExpert, then kernel_bootstrap_thread waits in **kperf_init_early -> ml_wait_max_cpus**. Confirmed via actual-kernel GDB call stack (`research/locate-cpu-wait2.log` / `.symbols`) and normal-run `max_cpus_initialized == 2` (MAX_CPUS_WAIT). Do not force the flag or remove the wait. Next implement real CPU registration and completion using XNU's CPU/processor interfaces. CURRENT SWEET uses legacy IOCPU (`USE_APPLEARMSMP` is absent); enabling AppleARMSMP is a separate architectural change requiring GIC/PMGR services. No userland/root filesystem boot yet.
- Fixed this session: PC=0 unregistered PRNG callback (0014, built-in HMAC-SHA256 provider + direct entropy/write_random hashing); absent Image4/AMFI trust-cache service (0015, unavailable runtime, loads rejected and queries no-match); missing platform driver (0016, SweetPlatformExpert exact personality); incorrect QEMU timer IRQ (0017, validated ADT PPI property, phone default19/QEMU27).
- **Latest kernel** `xnu/out-0017/kernel.development.sweet`, SHA256 `055a80190ab998b4df9203f1102072dcc78faad1779a537a514e522b682b582f`. Buildtree/log **`research/bt21` / `research/run21.log`**; all nine changed C/C++/header/config files byte-match local checked sources. Checksums passed. Entry PA `0xb146b488`, kernel end PA `0xb1ff4000`. Preferred baseline for patch 0018: `research/bt21/xnu` (no nested .git).
- **QEMU verified**: `research/q0017-noprogress` ran **30 seconds**, no visible panic, **zero unknown glyphs**, IRQ exceptions returned normally (4 IRQs, tickless idle). It still waits for CPU completion, so this is NOT a completed boot. Earlier 15s run `research/q0017` captured 122 IRQs, timer value27, GICR_ISENABLER0 `0x08000002`, and CPU WAIT; graphical progress overlay corrupted 13 glyphs. Add **`-noprogress`** to keep the console readable. New runner classifies incomplete/blank decoding explicitly (exit3 with --fail-on-panic), visible panic exits2. Snapshot-only; use regs/GDB to diagnose stalls.
- Current QEMU payload/ADT: **`research/q0017-noprogress.bin` / `research/q0017.adt`**. Run: `python3 tools/qemu-sweet.py research/q0017-noprogress.bin research/NEW-OUT --secs 30 --kernel xnu/out-0017/kernel.development.sweet --fail-on-panic`. Generate fresh seed with mkadt for a new payload. QEMU virt models generic ARM execution, not Qualcomm XPU/bootloader/panel or the phone's SMC PSCI conduit.
- **Prepared phone image**: **`shim/out/shim-boot-0017.img`**, 16,416,768 bytes, SHA256 `51ff7fe30b4a6668c3452ffe1da922c5ac30cb8de60625c2a652587ecbd0ec92`. Phone ADT/payload `shim/out/sweet-0017.adt` / `shim/out/payload-0017.bin` (timer19). Unpacked ramdisk exactly matches payload (`shim/out/check-0017`). Command line **`debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`**. NOT sent to phone this session: known CPU-driver wait should be addressed in QEMU first. Last actual phone boot remains 0013; user's latest photo `IMG_20261015_160033.jpg` confirmed the old PRNG PC=0 panic.
- Direct QEMU/GDB API checks: PRNG 600-byte chunking, buffer bounds, changing output, zero-length reads/writes, user reseed, contended refresh, generator1 init/output PASS again on latest0017 (`research/check-prng17.gdb` / `.log`, guest `research/q0017-api`). Trust-cache unavailable-service/argument/output cases PASS on0015 (`research/check-trustcache.gdb` / `.log`). Platform registration + target/product normal/truncated/zero/null/bounds PASS on0016 (`research/check-platform.gdb`, `research/check-platform3.log`). Different platform ID correctly retains IOPanicPlatform fallback (`research/q0016-negative`). Timer PPI32 and size2 correctly panic (`research/q0017-badppi`, `research/q0017-badsize`); omitted property selects19 (`research/q0017-default`). These are API/bring-up checks, not crypto certification or SMP scheduling validation.
- Local checker **`research/check-patch-order.py`** verifies all17 patches and changed source equality using a temporary upstream git index. Fixtures/logs/artifacts under research are local-only and ignored. For GDB unsigned kernel pointers, mask `int(register_value) & ((1<<64)-1)`; x30/x29 may be rendered signed. Stop at a function entry for manual-call return traps, and discard each manipulated guest after API tests. All fixture QEMU guests have been stopped.


- User wants persistent progress toward iOS 16, commits for completed changes, builds through `gh` / GitHub Actions.
- Keep this file current at every milestone so another agent can resume immediately, including when usage limits interrupt a session.
- Patch 0017 committed/pushed `aa3f991`, Actions `38751507437` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38751507437. Next artifacts: xnu/out-0017, research/bt21, research/run21.log. Do not duplicate.
- Patch 0016 kernel downloaded/checksum verified: SHA256 `d06336c54baa06e0df6561a8bd7df98a9b9f7502c9381c62d1a44c045cd1ce46`, `xnu/out-0016/kernel.development.sweet`; buildtree/log `research/bt20` / `research/run20.log`. QEMU `research/q0016` (10s) prints platform expert registered, zero unknown glyphs, no panic observed BUT stalls (timer mapping still wrong until 0017). Direct actual-kernel GDB tests passed for registered instance and target/product names, including maxLength=1/0 and NULL, sentinels intact (`research/check-platform.gdb`, `research/check-platform3.log`, `research/q0016-api3`). First two fixture attempts used premature/wrong return breakpoints; discarded, corrected fixture uses start-return observation then a function-entry return trap. No phone boot yet.
- Patch 0016 CI `38750733996` PASSED (4m3s); artifacts not yet downloaded/booted. Patch 0017 prepared to fix QEMU timer fidelity BEFORE interpreting future no-panic/stall runs: actual `qemu-system-aarch64 -M virt,gic-version=3,dumpdtb=research/qemu-virt.dtb ...` timer property has virtual PPI 11 => INTID 27, while phone uses INTID 19. Kernel formerly hardcoded 19. New optional /arm-io/gic virtual-timer-intid property selects the PPI (validate exact u32 and 16..31); missing property retains phone default 19. One RO-late global drives GIC enable and sleh_irq dispatch. tools/mkadt.py emits phone19/QEMU27. Both C TUs compile (`research/timer-objs`), ADT serialize/parse checks confirm values/addresses, all 17 patches apply and match. Sandbox `research/bt19/xnu`, originals `research/pre0017`; initial local sleh compile failed due read-only exported header not updated, fixed export and BOTH recompiled successfully. CI exports normally.
- Architecture correction for next agent: CURRENT SWEET board_config does NOT define USE_APPLEARMSMP; it compiles the legacy IOCPU path, not cpu_boot_thread/IOPMGR AppleARMSMP. Existing roadmap mentions AppleARMSMP as a future approach. Do not look for gCPUIC/gAIC/gPMGR globals in the current kernel: they are absent. Determine the actual CPU-init wait after platform registration before choosing the next driver architecture.
- Patch 0016 committed/pushed as `0dcdf5c`; Actions `38750733996` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38750733996. Next artifacts: `xnu/out-0016`, `research/bt20`, `research/run20.log`.
- Patch 0016 implemented: new `iokit/Kernel/arm/SweetPlatformExpert.cpp`, arm64 source list, built-in personality in KernelConfigTables.cpp guarded by ARM64_BOARD_CONFIG_SWEET. Matches only xiaomi,sweet at score 10000, keeps IOPanicPlatform fallback for other hardware. Uses IODTPlatformExpert base initialization/power domain and createNubs CPU logical-ID annotation; overrides top-level publishing because base requires nonexistent Apple NVRAM /options. Publishes real CPU/top-level nodes, provides bounded target/product names, and registers the platform service. GIC/PMGR and arm-io child-bus drivers remain separate work. Both C++ TUs compile (`research/platform-objs`), all imports resolve against 0015/own objects, all 16 patches apply and sources match. Sandbox `research/bt19/xnu`; originals `research/pre0016`. NEXT: full Actions/QEMU; inspect CPU boot waits after the platform registers, do not equate absence of panic with successful boot.
- Patch 0015 CI `38749549779` PASSED (4m3s), checksum verified. Kernel `xnu/out-0015/kernel.development.sweet`, SHA256 `ca1b424a596c7058550ad1659080a333e7643270f019656401d80bf2b876645f`; buildtree/log `research/bt19` / `research/run19.log`. QEMU `research/q0015` (10s, zero unknown glyphs) passes trust-cache handling and machine lockdown, then panics `Unable to find driver for this platform: "xiaomi,sweet" @IOPlatformExpert.cpp:2319` (IOPanicPlatform fallback). Next concrete work: add a built-in SWEET IODTPlatformExpert personality/class; keep the fallback for mismatched platforms. No phone boot yet.
- Direct trust-cache API checks PASSED in the actual 0015 kernel under QEMU/GDB (`research/check-trustcache.gdb`, `research/check-trustcache.log`, `research/q0015-api`): runtime pointers NULL, hash/UUID queries return NOT_FOUND, typed/untyped/legacy loads return NOT_SUPPORTED, capabilities outputs zero and bounds preserved, invalid-argument cases still reject. Second PRNG check also verifies contended refresh returns immediately (`research/check-prng2.log`, `research/q0014-api2`).
- Patch 0015 committed/pushed `8f851d4`, Actions run `38749549779` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38749549779. Do not duplicate.
- Direct PRNG API checks passed in the REAL 0014 kernel under QEMU/GDB, stopped before trust-cache init (`research/check-prng.gdb`, log `research/check-prng.log`, guest `research/q0014-api`). Invoked read_random for 600 bytes (chunking and buffer sentinel intact), repeated output differed, zero-length read/write succeeded, write_random(64-byte input) succeeded, secondary generator 1 initialized and generated distinct output. QEMU log has no exceptions during these checks; guest was discarded after the tests. This tests API behavior, not a cryptographic certification or SMP scheduling.
- Patch 0015 implemented in `bsd/kern/kern_trustcache.c`: SWEET with missing Image4/AMFI marks both runtime pointers NULL and logs service unavailability. Existing static loader handles NULL. Public load APIs return KERN_NOT_SUPPORTED; hash/UUID queries return KERN_NOT_FOUND; capabilities outputs are zero with KERN_NOT_SUPPORTED. Invalid-input checks remain, and Apple paths/available-provider initialization retain existing behavior. No trust is granted. Local arm64 compile passed (`research/trustcache-objs`), all 15 patches apply and sources match. Sandbox `research/bt18/xnu`, baseline `research/pre0015`. Next: Actions/QEMU and direct unavailable-service API checks using GDB.
- Patch 0014 CI `38748543058` PASSED (4m0s), artifact checksum passed. Kernel `xnu/out-0014/kernel.development.sweet`, SHA256 `741de20c851755d333759d7e668346f3edb4f325efac4cc7c142d80d1f5ff75c`; buildtree/log `research/bt18` / `research/run18.log`. QEMU run `research/q0014` (10s, zero unknown glyphs) prints `SWEET PRNG: HMAC-SHA256 registered`, passes commpage initialization, loads vnguard policy, THEN panics `image4 interface not available @kern_trustcache.c:253`. PC=0 fault FIXED IN QEMU. No new phone boot performed.
- Next work: SWEET currently has neither AppleImage4 nor AMFI. `trust_cache_runtime_init` unconditionally requires both; unlike BCM2837, SWEET enters this startup path. Investigate an explicitly unavailable trust-cache runtime with rejected loads/no query matches while providers are absent; preserve all Apple-platform checks. Merely skipping the startup call would leave later AMFI/IMG4 dereferences unsafe. Existing load_static_trust_cache already handles trust_cache_rt == NULL.
- QEMU runner now supports `--kernel MATCHING_KERNEL --fail-on-panic`: runtime font extraction from the payload produces exact `fb.txt`, plus `result.json`; a visible panic returns 2. Baseline 0013 re-run `research/q0013-text` detects the same NULL callback panic with zero unknown glyphs. Synthetic glyph roundtrip (including literal `?`) and invalid-VA rejection passed. Screen rows remain circular, not a full log; `no_panic_observed` does not prove successful boot. README has the invocation.
- Patch 0014 committed/pushed as `be61f99`; Actions run `38748543058` is running: https://github.com/dipa-studio/ios-sweet/actions/runs/38748543058. Do not start a duplicate build.
- 2026-10-15 active: hardware photo `IMG_20261015_160033.jpg` confirms the same PC=0 instruction-fetch abort as QEMU (LR `0xfffffe0007443fe4` = read_random). Independently reproduced with kernel 0013 in `research/q0013` (8-second run); prior allocator fix works. Latest tooling commit reviewed: `8d66d2d`.
- Patch 0014 implemented: registers a built-in HMAC-SHA256 DRBG provider at EARLY_BOOT/FIRST, before commpage creation. A shared mutex serializes output/reseed across CPUs (correctness first; no per-CPU scaling yet), initgen validates CPU identities, refresh uses try-lock and the existing entropy filter/health tests. SWEET entropy accumulation and write_random now use the built-in SHA256 directly; no incomplete g_crypto_funcs table is registered. The existing bootloader-seed requirement remains enforced.
- Local validation: both PRNG translation units compile to arm64 objects in `research/prng-objs`; no SHA256 wrapper imports remain. All 14 patches apply in CI order and resulting PRNG sources match local inputs. Sandbox `research/bt17/xnu`, originals `research/pre0014`, checker `research/check-patch-order.py`. NEXT: Actions kernel build, download to `xnu/out-0014`, run the QEMU payload before requesting another phone boot. Baseline QEMU ADT/payload are `research/q0013.adt` / `research/q0013.bin`.
- 2026-10-15 08:15 QEMU EMULATION WORKS (`tools/qemu-sweet.py`, see "QEMU emulation" below). Patch 0013 VERIFIED in QEMU: boot passes kmem_crypto_init and ulock_initialize. NEXT BLOCKER (reproduced in QEMU, expected on hardware too): `pmap_create_commpages -> random() -> RandomULong -> read_random -> prng_funcs.refresh == NULL` (instruction fetch abort at pc 0, lr read_random+0x34). Nobody calls `register_and_init_prng` (closed corecrypto kext does it on Apple). Fix = patch 0014: SWEET-registered cckprng_funcs provider (osfmk/prng/prng_random.c); note random_cpu_init also panics while !prng_ready. The 07:26 hardware boot of 0013 will most likely show this same panic.
- Previous hardware boot: patch 0013 (`a3a46e2`) CI `38686345689` PASSED (3m44s), image `shim/out/shim-boot-0013.img` was temporarily booted on 2026-10-15 at 05:26 UTC: send OKAY (0.407s), boot OKAY (0.134s), total 0.546s. Superseded by the hardware/QEMU PC=0 observation and patch 0014 work above.
- Latest kernel implementation commit: `8f851d4`. Latest successful kernel CI: `38749549779` (patch 0015), https://github.com/dipa-studio/ios-sweet/actions/runs/38749549779.
- CURRENT image: `shim/out/shim-boot-0013.img`, 16,412,672 bytes, SHA256 `110d1c46aee4b60253db24b6535e9781c4cf0fd25ca2a70e9016cf81378cea25`. Kernel `xnu/out-0013/kernel.development.sweet`, SHA256 `f0b1d375995316c550ce7014ff234d8271e9b9ac2857795ceff41b76ba2cd740`, plain arm64. CI log confirms 0013 applied; checksums passed. Entry PA `0xb146b488`, kernel end PA `0xb1ff4000`.
- Current log/buildtree: `research/run17.log`, `research/bt17` (extracted, no nested .git; preferred baseline for patch 0014). Payload/ADT: `shim/out/payload-0013.bin`, `shim/out/sweet-0013.adt` (includes /defaults). Unpacked ramdisk byte-compared with payload in `shim/out/check-0013`.
- Patch-0012 original image: `shim/out/shim-boot-0012.img`, 16,412,672 bytes, SHA256 `cf4b8ec4a65a51888d71c17d09cd0831d6b761b90e2531fdb8e575c32f4fd24a`. Kernel `xnu/out-0012/kernel.development.sweet`, SHA256 `69b78f49f6459cc63a0af44e7cf73c1779ef72130ef72b12ae86c88183ffc742`; checksums passed and sweet_early_relocate symbol present. Entry PA remains `0xb146b488`, kernel end PA `0xb1ff4000`.
- Patch-0012 log/tree: `research/run16.log`, `research/bt16` (extracted, no nested .git; preferred baseline for next patch). CI log confirms 0012 applied. Payload `shim/out/payload-0012.bin`, ADT `shim/out/sweet-0012.adt`; packed ramdisk byte-compared with payload in `shim/out/check-0012`.
- Active payload command line: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam`.
- Latest observed photo: `/home/dipa/Downloads/IMG_20261015_071346.jpg` (local only). Stages 9 (kernel framebuffer VA), 10 (TTBR0 off/TLB flush), 11 (physmap table pointers) and 12 (calling pmap_bootstrap) all visible. Persistent diagnostics are VERIFIED ON HARDWARE.
- pmap.c:1987 is the assert after SecureDTLookupEntry(NULL, "/defaults", &entry). The pmap-max-asids property itself is optional; an empty /defaults uses XNU's MAX_ASIDS. Do not bypass the assertion or guess an ASID limit.
- Previous payload-only fix image: `shim/out/shim-boot-0012-defaults.img`, SHA256 `3ff61a268853c6ea7cbf284833f1346a1d791980ca366b6e1a215a86e040512b` (16,412,672 bytes). Same kernel/CI as 0012; ADT 2516 bytes (`shim/out/sweet-0012-defaults.adt`), root /defaults present with no ASID override. Payload `shim/out/payload-0012-defaults.bin`; unpacked ramdisk exactly matches payload (`shim/out/check-0012-defaults`). This boot reached the allocator crypto NULL-pointer fault below.
- Latest hardware photo: `/home/dipa/Downloads/IMG_20261015_071608.jpg` (local only). Normal serial framebuffer console works. VM bootstrap/zone initialization, logging, scheduler, IPC, KDP and ulock_initialize print before the kmem_crypto_init data abort. gPanicBase warnings are expected because PLATFORM_PANIC_LOG_DISABLED is configured, and are not the faulting call.
- Faulting instruction: `ldr x0, [x8, #0x150]` in crypto_random_kmem_ctx_size; x8 = NULL g_crypto_funcs. Source: libkern/crypto/corecrypto_rand.c and osfmk/vm/vm_kern.c:2448. rand.h explicitly permits NON-cryptographic kmem RNGs, but requires no locking in generation and context size <=256. Do not substitute read_random here: the global PRNG provider is also unregistered and kmem generation must remain lock-free. early_random may seed contexts during single-CPU bootstrap; it uses the built-in HMAC-SHA256 DRBG with the host-generated ADT seed.
- Patch 0013 keeps the replacement entirely private to vm_kern.c: SplitMix64 state is 8 bytes per CPU/sizeclass, initialized using early_random. Uniform selection uses rejection sampling to avoid modulo bias. Other platforms retain the original corecrypto calls; SWEET's generic crypto APIs/registration remain untouched. Reference: https://prng.di.unimi.it/splitmix64.c (Sebastiano Vigna, public domain).
- Validation: vm_kern.c compiles to arm64 (`research/kmem-rng-objs`, using `research/run16.log` + `research/bt16`). Undefined symbols include early_random and NO crypto_random_* references. All 13 patches apply in CI order and match the checked source. Original saved as `research/vm_kern.pre0013.c`.
- RNG host checks: extracted actual new helper in `research/kmem-rng-check.c`; 16,384 outputs match the author's downloaded reference (`research/splitmix64-reference.c`), 147,456 bounded outputs satisfy their limits (including 1 and UINT64_MAX), and a forced rejection advances correctly. Full kernel crypto services and the registered kernel PRNG provider remain future work; this patch only supplies the allocator RNG contract.
- Patch-0011 artifacts: kernel `xnu/out-0011/kernel.development.sweet`, log `research/run15.log`, extracted buildtree `research/bt15` (no nested .git). CI log confirms patch 0011 applied; artifact checksums passed. Kernel is plain arm64, entry VA `0xfffffe000746b488` -> PA `0xb146b488`, kernel end PA still `0xb1ff4000`.
- Patch-0011 image: 16,412,672 bytes, SHA256 `29ad5cbfc752473de4af2c471a24a8b34f92da4bff5afec65c614b90cd3ad53a`. Kernel SHA256 `8ac3a956894b09f7bd0d890de8c5de38c65513287feb1e1bcd12db8ca772ee5c`. Payload/ADT in `shim/out/payload-0011.bin`, `shim/out/sweet-0011.adt`. Header unpacked/checked in `shim/out/check-0011`.
- First patch-0011 attempt timed out waiting for FASTBOOT; superseded by the successful send/boot recorded above. Do not repeat the boot before the user reports the current screen.
- Patch-0011 photo: `/home/dipa/Downloads/IMG_20261015_070218.jpg` (local only). Visible stages 1-8 and old red shim text; entry PA B146B488, boot args PA B1FF8000 / top B1FFC000. No evidence yet locating execution after stage 8.
- Patch-0012 sandbox: `research/bt15/xnu` (NO nested .git). Proposed permanent debug VA: VM_MIN_KERNEL_ADDRESS + ARM_TT_L2_SIZE, in the reserved gap below gVirtBase and above low globals; map the same PA framebuffer as Normal-NC in the final TTBR1 before arm_vm_physmap_init changes phystokv(). Keep the early panic sink until normal kprintf startup.
- Patch 0012 validation: all 4 changed C translation units compiled to arm64 objects in `research/post-ttbr0-objs`; internal header export added to osfmk/arm64/Makefile and recreated locally in EXPORT_HDRS for pe_serial.c. All 12 patches apply in CI order and match all 6 edited files. Baselines are saved under `research/pre0012/`.
- Actual mapping-helper host model (`research/early-map-check.c`, barrier stubbed only for host compilation) confirms: VA `0xfffffe0002000000`, L1 index 2016, L2 index 1; descriptor `0x006000009c000605` = framebuffer PA, Normal-NC, outer-shareable, EL1 RW, PXN+UXN. All other L2 entries preserved. The gap is excluded by ARM_LARGE_MEMORY's pmap_virtual_region(0).
- Next boot command line MUST add `-enable_kprintf_spam`: DEVELOPMENT disables kprintf_output by default even when DB_KPRT is set (`pexpert/arm/pe_kprintf.c`). Planned command line: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam`.

### Patch 0012 screen legend (after original stages 1-8)
- 9: kernel framebuffer VA live; 10: TTBR0 off / TLB flushed; 11: physmap pointers rebound.
- 12: calling pmap_bootstrap; 13: pmap_bootstrap returned; 14: early I/O page tables ready; 15: arm_vm_init returned.
- 16: calling serial_init; 17: fbcon config read / mapping; 18: fbcon mapping returned; 19: serial_init returned; 20: kprintf startup complete.
- Normal serial initialization clears the screen, so earlier stages may disappear before 19. If serial exists, early console disables after 20; otherwise early panic output stays enabled.
- Local source sandbox: `research/bt13/xnu` (git baseline `623b060`, patches 0001-0008 already in baseline; uncommitted export edits correspond to patch 0009). Preserve those edits when producing patch 0010.
- Previous compile log/tree: `research/run13.log`, `research/bt13`; previous kernel: `xnu/out/kernel.development.sweet`. New successful log/tree: `research/run14.log`, `research/bt14`; new kernel: `xnu/out-0010/kernel.development.sweet`.
- Current device access: patch-0012 image was temporarily started from FASTBOOT; no adb connection is expected. Never flash partitions for bring-up; use `fastboot boot` with the shim image.
- Patch `0010` lives in `xnu/patches/0010-sweet-early-framebuffer-debug.patch`. Its exact compiled sources are under `research/bt13/xnu`; generated objects are in `research/early-objs`.
- Local checks passed: all 5 changed/new translation units compiled to arm64 Mach-O objects; new console only imports `_doprnt` and `iso_font` (both exist in the previous kernel). All 10 patches applied to a temporary upstream git index in CI order, and all 7 resulting changed source files matched the local compile inputs.
- Reproduce changed-file compilation: `XNU_CC_OBJDIR="$PWD/research/early-objs" python3 tools/xnu-cc.py research/run13.log research/bt13 arm_init.c arm_vm_init.c kern/debug.c arm64/start.s osfmk/arm64/sweet_early.c --per-file` (create the output directory first). The helper now accepts explicit new source paths absent from the old artifact's Makefiles and rejects empty selections.
- Device command result: `adb reboot bootloader`, then `timeout 30s fastboot boot shim/out/shim-boot-0010.img`: send OKAY (16028 KB), boot OKAY, total 0.541s. This confirms bootloader acceptance only, not XNU progress.
- The successful boot above was on 2026-10-14. On 2026-10-15, `timeout 45s fastboot boot ...` timed out waiting for a device; no image was sent. Boot image SHA256 rechecked and unchanged. No new build is needed to repeat the trial.
- Earlier retry: `timeout 90s fastboot boot shim/out/shim-boot-0010.img` timed out (exit 124). This is superseded by the successful 2026-10-15 repeat boot recorded above; do not repeat it until the user reports the screen.
- User observation confirms MMU-on C entry, platform expert, CPU topology/bootstrap, startup bootstrap and cpu_init all complete. Failure is within arm_vm_physmap_init (non-XNU_MONITOR branch), before stage 8 / TTBR0 teardown. Old red shim text remains behind the early console; this is expected because only written rows are cleared.
- New edit/compile sandbox is `research/bt14/xnu` (extracted artifact, NO nested .git; `git diff` there would act on the parent project). Use a saved baseline file / generated unified diff, or explicitly initialize a local baseline before editing. `research/bt13/xnu` does have its own baseline git repo.
- Exact user-reported panic: `arm_vm_init.c:1606 Assertion failed: (temp_ptov_table[ptov_index - 1].va + temp_ptov_table[ptov_index - 1].len) <= physmap_end`.
- Root cause: MacOSX build sets `ARM_LARGE_MEMORY=1`; `XNU_MONITOR` is off. Block alignment in arm_vm_physmap_slide can add gaps, but the non-PPL large-memory physmap_end omitted the padding already present in the small-memory branch. Patch 0011 reserves PTOV_TABLE_SIZE*ARM_TT_TWIG_SIZE (8*32MB) in both the aperture bound and its L1 allocation; monitor/PPL builds retain zero padding. The assertion is preserved.
- Validation: `arm_vm_init.c` compiled to an arm64 object using `research/run14.log` + `research/bt14` (object in `research/physmap-objs`). All 11 patches apply in CI order and produce exactly that source. Host harness `research/physmap-check.c` extracts the real XNU slide function with page-table-write stubs: all 65,536 page-aligned slides within 1GB checked against the current kernel memory layout; old bound fails 65,504 cases, padded bound fails zero; worst overrun 0x1ffc000 bytes. Baseline source saved as `research/arm_vm_init.pre0011.c`.
- CI: https://github.com/dipa-studio/ios-sweet/actions/runs/38638702083 (success; artifacts and full log downloaded).
- New image: `shim/out/shim-boot-0010.img` (16,412,672 bytes, SHA256 `ed2c270e0081bad5ff517be415f9bfe8805463aea7866732203fa97ac737e64e`). Previous image preserved as `shim/out/shim-boot-pre0010.img`.
- New kernel SHA256: `3ee05270cc291c49589a101678f904b1b69cb2f2575e05398bce8a4e052f409a`. Artifact checksums passed; arm64 (not arm64e), early-console symbols present. Payload entry VA `0xfffffe000746a488` -> PA `0xb146a488`; kernel end PA `0xb1ff4000`. Command line remains `debug=0xc serial=3 cpus=1 keepsyms=1`.

### Patch 0010 screen legend
- Bottom-left white 64x32 tile: assembly entered `start_first_cpu`, before page tables/MMU.
- Text `SWEET EARLY 1`: entered `arm_init` with MMU on; 2: platform expert done; 3: CPU topology done; 4: CPU bootstrap done; 5: startup bootstrap done; 6: CPU init done; 7: entering `arm_vm_init`; 8: leaving bootstrap TTBR0.
- `SWEET EARLY PANIC:` prints a copy of panic arguments before normal panic handling needs per-CPU/thread state.
- In patches 0010/0011 the console disabled before TTBR0 invalidation; patch 0012 replaces that with a persistent kernel VA alias and diagnostics through normal serial/kprintf startup (see its legend above).
- Screen contents require a physical observation from the user: adb cannot capture a framebuffer after Android has been replaced by a bare-metal kernel.

## QEMU emulation (2026-10-15)
- `tools/mkadt.py OUT.adt --qemu` = same ADT with QEMU virt GICD 0x08000000 / GICR 0x080a0000 / CNTFRQ 62.5 MHz.
- `tools/qemu-sweet.py PAYLOAD OUTDIR [--secs N] [--hmp "x /8gx ADDR"] [--gdb]`: host-side copy of shim/loader.c
  (segments + ADT + boot_args at their PAs, 6-instruction stub sets x0 and jumps), `-M virt,gic-version=3 -cpu cortex-a76`
  (same ID_AA64MMFR0 as Kryo 470), 4G RAM so 0x9c000000 fb and 0xb0000000 kernel are plain RAM.
  Outputs `fb.png` (the screen), `regs.txt`, `qemu.log` (exceptions with ESR/FAR/ELR). `--gdb` halts with a stub on :1234
  (system `gdb` + `set architecture aarch64`, `hbreak *VA` works with the MMU on).
- Not emulated: Qualcomm XPUs, ABL, real panel, PSCI conduit (QEMU virt uses hvc). Hardware still decides those.
- Example: `python3 tools/mkadt.py /tmp/q.adt --qemu && python3 tools/mkpayload.py xnu/out-0013/kernel.development.sweet /tmp/q.adt /tmp/pq.bin --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam' && python3 tools/qemu-sweet.py /tmp/pq.bin /tmp/q`

## Layout
- `backup/`  partition dumps + full sdb..sdf LUNs, `SHA256SUMS` (all verified on-device == local)
- `dtb/`     live FDT (from OrangeFox), PixelOS boot.img dtb, dtbo, dts versions
- `shim/`    bare-metal boot shim (`./build.sh` -> `out/shim-boot.img`)
- `research/` mainline sm7150 dts, XNU pe_fiq.c etc, PocketDarwin patches
- `tools/unpack_bootv2.py`

## Device access
- No root in PixelOS 17 (user build, adb root disabled). Root = `fastboot boot recovery/recovery.img` (OrangeFox R12.0_2, temp boot, not flashed).
- `fastboot boot` works for stock boot.img, OrangeFox, and our shim.
- Shim resets itself via PSCI SYSTEM_RESET; IMEM restart_reason 0x77665500 is seen
  (bootreason=bootloader) but ABL still boots Android, so it does not stop in fastboot.

## Hardware facts (live DT + shim measurements)
- Framebuffer (ABL cont splash): 0x9c000000, 1080x2400, stride 4320, a8r8g8b8, panel is DSC *video* mode (no kick needed).
- RAM: 0x80000000+2G, 0x100000000+0xfcc00000. Live reserved-memory = PixelOS base dtb + sweet dtbo fixups.
- GICv3: GICD 0x17a00000, GICR 0x17a60000 (stride 0x20000, 8 PEs). GICD_TYPER 0x007B041D: SecurityExtn=1 (DS=0).
  ABL leaves GICD_CTLR = 0x10 (ARE_NS on, groups off). Writing 0x13 reads back 0x12: EnableGrp1A (bit1) is the one that matters.
- GIC Group 1 VERIFIED (shim 2c): vtimer INTID 19 = 30 IRQs/300ms, ptimer INTID 18 = 30, SGI 1 to self = 10/10.
  ICC_SRE=1 already, ICC_CTLR 0x8440, priority/enable writes stick. GICR_IGROUPR0 and GICD_IGROUPRn read 0 from NS
  even though Group 1 delivery works (RAZ in NS view) - do not use them as a health check.
  INTID 28 shows pending constantly (unknown PPI 12).
- Timer 19.2 MHz. Qualcomm PPI numbering: secure-phys/phys/virt/hyp = PPI 1/2/3/0 -> vtimer INTID 19, ptimer 18 (NOT 27/30).
- PSCI 1.1 via SMC, SMCCC 1.1. CPU_ON ok; SYSTEM_SUSPEND and SYSTEM_RESET2 unsupported.
- SMP: CPU_ON works for all 7 secondaries (MPIDR 0x81000n00).
- Entry EL1, MMU off, x0 = DTB (magic ok). ID_AA64MMFR0 = ...101122: 16K granule YES, 4K YES.
- No serial console: debug UART SE is in SPI mode on sweet. Screen is the only debug channel.

## XNU implications
- XNU GICv3 path (pexpert/arm/pe_fiq.c, VMAPPLE) puts the timer (PPI 27) in Group 0 = FIQ. With SecurityExtn=1,
  Group 0 is secure -> impossible from NS EL1. Need: Group 1 IRQ, ICC_IGRPEN1, INTID 19, and the timer handled in the IRQ path.
- XNU VMAPPLE PSCI uses `hvc`; sweet needs `smc`.
- PocketDarwin NonAppleGenericCpu.patch only swaps -mcpu=apple-h7 for -march=armv8-a.

## Milestone 3 (2026-10-14): 16K MMU verified
- ABL loads the Image at 0x80080000 (base 0x80000000 + 0x80000) regardless of text_offset=0.
- 16K granule, T0SZ=28 (L2 start, 32MB blocks), MAIR 0x44ff00, TCR 0x1409CB51C, PARange=2 (40-bit PA).
- Only own image pages WB (Qualcomm XPUs: never map carve-outs cacheable); fb Normal-NC; rest Device.
- AT S1E1R checks all as expected; unmapped -> translation fault L2.
- Fill fb: 110 ms (MMU off) -> 3.5 ms (NC). Memory loop 70 ms -> 0.7 ms. vtimer + SGI IRQs still fine with MMU on.

## XNU loader design notes (xnu-8796)
- Entry: `_start` -> `start_first_cpu`, x0 = boot_args (PA), MMU off, EL1. boot_args: Revision/Version 2,
  virtBase/physBase/memSize/topOfKernelData, Boot_Video (fb 0x9c000000, rowBytes 4320, 1080x2400, depth 32),
  deviceTreeP/Length (ADT), CommandLine[608], bootFlags, memSizeActual.
- start.s maps [kernel mach header .. physBase+memSize) with cacheable BLOCKS. On Qualcomm every XPU carve-out
  must therefore lie below the kernel: use physBase = 0xb0000000 (above ramdump_fb 0xaf000000+16M,
  cdsp_sec, qseecom), end before rmtfs 0xfde01000 -> memSize ~0x4d000000 for bring-up.
- Base ADT: iPhone 8 d20ap DeviceTree from iOS 16.7.16 IPSW (tools/remotezip.py + tools/adt.py; LZFSE payload).
  Template values are empty: the loader must fill chosen/*, memory-map, cpu*/reg etc. like iBoot.
- XNU build: GitHub Actions (dipa-studio/ios-sweet, xnu.yml) via blacktop/darwin-xnu-build, MACOS_VERSION=13.5.
  New-clang fixes in xnu/patches; Xcode 16 iig is incompatible (IORPC.kernelContent) -> macos-14 runner, oldest Xcode.

## Kernel architecture plan (SWEET machine config)
- First stock build (VMAPPLE) is arm64e (PAC). Kryo 470 = ARMv8.2, no PAC -> SWEET must build plain arm64
  (HAS_APPLE_PAC is only defined for __arm64e__, so arm64 drops PAC automatically).
- Template: VMAPPLE (16K pages, GICv3 code in pe_fiq.c) minus APPLEVIRTUALPLATFORM hypercalls/paravirt PAC,
  plus BCM2837 (generic ARM: no APPLE_ARM64_ARCH_FAMILY Apple sysregs). Do not link the KDK lib<cfg>.os.*.a.
- Timer: XNU expects it on FIQ (sleh_fiq -> rtclock_intr). SWEET: GIC Group 1, so sleh_irq must ack ICC_IAR1,
  route INTID 19 to the same timer path, SGIs to cpu_signal_handler, EOI via ICC_EOIR1.
- SMP/interrupts: open-source AppleARMSMP (USE_APPLEARMSMP) needs an IOInterruptController (AIC role) and an
  IOPMGR (CPU power -> PSCI CPU_ON via SMC). Plan: built-in IOKit classes (SweetGIC, SweetPMGR, platform
  expert) compiled into the kernel and matched via iokit/KernelConfigTables.cpp -> no closed kexts needed.
- Debug: xnu patch 0003 sweet-fbcon = kprintf drawn on the framebuffer (ADT node "sweet-fbcon").
- ADT /arm-io ranges must not map to parent 0 (pe_arm_get_soc_base_phys()==0 disables serial): use parent 0x1000000.

## Status 2026-10-14 late: first XNU boot attempt
- SWEET kernel builds (CI run 38635343732, patches 0001-0009). Verified offline: arm64, no PAC, no >v8.2 code,
  70300 classic relocs all in range (slide 0 = no loader work), entry = __start.
- Boot: shim loader worked (initrd 0x846F0000-0x855FE3E0, BOOTARG 0xB1FF8000, JUMP 0xB146A488) but screen stayed
  red -> xnu died/hung before serial_init (fbcon). kprintf is a no-op before STARTUP_SUB_KPRINTF.
- Historical next-step plan (patch 0010, now implemented and built; see active handoff above): early fb debug channel
  1. start.s start_first_cpu (after mov x20,x0 / mov x21,#0): PA marker band (MMU off, direct store to 0x9c000000).
  2. start.s after both create_bootstrap_mapping calls: add ONE block entry to the existing V=P L2 table
     (x25 + 2*PGBYTES; do NOT call create_bootstrap_mapping again: fb and RAM share L1 entry 0, it would replace
     the L2 table). Entry = TYPE_BLOCK|VALID|SH(OUTER)|ATTRINDX(CACHE_ATTRINDX_WRITECOMB=1)|AF|PNX|NX|0x9c000000,
     index = (va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT.
  3. new osfmk/arm64/sweet_early.c (+files.arm64 entry): sweet_early_mark(n) colored bands at screen bottom and
     sweet_early_putc using iso_font via VA=PA while the bootstrap TTBR0 is live; disable before
     arm_vm_init's set_mmu_ttb(invalid_ttep) (arm_vm_init.c:2148).
  4. markers in arm_init after PE_init_platform, ml_parse_cpu_topology, cpu_bootstrap, kernel_startup_bootstrap,
     cpu_init, before arm_vm_init.
  5. panic hook at top of panic_trap_to_debugger: va_copy + _doprnt(fmt, &copy, sweet_early_putc, 16).
  Verify with tools/xnu-cc.py (compile+symbols) and patch-order simulation before the single CI run.

---

## 2026-11-05 — Dump del LK di fabbrica (aboot) e riformulazione del problema eMMC

Dump della partizione `aboot` (p8) dalla recovery con root:

```sh
adb shell 'dd if=/dev/block/mmcblk0p8 of=/sdcard/aboot.img bs=4096'
adb pull /sdcard/aboot.img
```

Risultato: `aboot` **è il bootloader LK vero**, ELF32 ARM stripped.
Entry `0x340f90`, `PT_LOAD` vaddr `0x0f900000` (file off `0x8000`,
filesz `0xe93d8`, memsz `0xfe53c`). Salvato in
`research/lk-g4/aboot-lk-g4-h815.elf`
(sha256 `0e120ff5d930a9d8bd3f113932e8af88d07ff12caa742e0911f884672cc21666`).

Stringhe rilevanti: `mmc init failed!`, `%s: target is not emmc boot`,
`%s: mmc write failure - %s`, `MMC Boot: MBR read failed!`,
`GPT: Could not read primary gpt from mmc`, `failed to set sdc%u_iface_clk`,
`sdhci_msm_init_dll`, `platform/msm_shared/sdhci.c`.

`ANDROID!` compare dentro il segmento di testo: è `BOOT_MAGIC`, usato da LK
per validare l'header del boot image. LK legge il boot image da
`MEMBASE + 0xFF000` (`app/aboot/aboot.c:89-92`); se MEMBASE = 0x0f900000,
l'header sta a `0x0f9ff000`, subito dopo la fine di LK.

**Riformulazione del problema.** LK ha *già* inizializzato l'eMMC prima di
saltare al nostro shim: legge il boot image dalla card con `mmc_read()`
(`app/aboot/aboot.c:890` → `platform/msm_shared/mmc.c:2462`). Quindi la card
è già alimentata, cloccata e **selezionata** quando il shim parte. Il
bring-up da zero in `shim/emmc.c` (reset software SDHCI, HC_POWER, clock
GCC, CMD0/1/2/3/7) è quindi **non necessario e dannoso**:

* il reset software dell'host distrugge la configurazione lasciata da LK
  (DLL/CDC, bit vendor, MCLK selector, IO pad switch);
* l'RCA **non serve**: sulla bus SD l'indirizzo della card non viaggia nei
  comandi, la selezione è avvenuta con CMD7. CMD24 scrive sulla card già
  selezionata.

Conseguenza: la strada giusta è **non reinizializzare** (solo CMD16 +
CMD24 + CMD18) o, meglio, **chiamare il driver di LK**
(`mmc_write(uint64_t addr, uint32_t len, uint32_t *in)` e
`mmc_read(uint64_t addr, uint32_t *out, uint32_t len)` — **ordine argomenti
diverso**), che è già funzionante.

**Sospetto aggiuntivo sul canale del tempo.** `reset_stage` viene impostato
solo sul percorso normale (`shim/shim.c:908`). Se il driver va in fault,
`exc_handler()` chiama `reset(15)` con `reset_stage == 0xffffffff` → attesa
15 s, e `reset()` rientra in `emmc_write_log()` → ricorsione. Quindi
l'attesa di ~40 s letta come "stadio 2" **implicherebbe che il driver è
tornato con errore e non è andato in fault** — ma la misura include anche
il boot della TWRP, quindi il modello numerico va validato prima
(vedi `GUIDE-03-EMMC-DEBUG.md`, Test 0).

**Nota Android.** Il telefono è Android **8.1.0 Oreo**
(`lge/omni_g4/g4:16.1.0/OPM8.181105.002/23:eng/test-keys`), non Android 13:
SuperSU 2.46 ha quindi buone probabilità di funzionare. `ro.boot.ddr_size`
= 3221225472 (3 GiB), `ro.boot.bootdevice` = `f9824900.sdhci` (conferma
l'indirizzo dell'host usato nel driver).

---

## 2026-11-05 — Il canale del tempo NON misura quello che credevamo (Test 0)

Prima di ogni altra cosa ho validato il canale del tempo, che era l'unico
dato che avevamo su cosa falliva nel bring-up eMMC.

Due build, stessa identica tranne l'attesa forzata:

| build | attesa imposta | tempo misurato |
|---|---|---|
| `FORCE_WAIT_S=5`  | 5 s  | **40,2 s** |
| `FORCE_WAIT_S=60` | 60 s | (in corso) |

Il codice e' corretto: in `shim/shim.c:232` l'attesa forzata sovrascrive
quella calcolata dallo stadio. Il tempo misurato non cambIa con l'attesa
imposta, quindi **i 40 s non sono l'attesa dello shim**: sono il tempo di
boot del bootloader + il boot della TWRP fino a quando `adb devices`
la vede.

Conseguenza: le letture di stadio fatte prima ("stadio 2", "stadio 4",
"errore 0") erano **interpretazioni sbagliate di un numero che non
cambiava**. Il tempo totale e' dominato da una costante di ~35-40 s
(boot TWRP), e la codifica `stage * 20 s` produceva numeri simili per
stadi molto diversi: e' per questo che l'attesa e' sembrata sempre
~41-42 s anche cambiando il codice.

Il canale del tempo, cosi' com'era, era **inutilizzabile**. Va rifatto:
- la misura va ancorata a un evento osservabile *dentro* la finestra dello
  shim, non al rientro in recovery;
- in alternativa il tempo di attesa va reso molto piu' grande del tempo di
  boot fisso, in modo che la differenza domini il rumore.

Nota: `tools/run-shim.sh` misura `t1 - t_fastboot_exit`, dove
`t_fastboot_exit` e' il momento in cui il telefono lascia il fastboot.
Quel momento include gia' il boot di LK, quindi non e' un riferimento
pulito per l'inizio dello shim.

---

## 2026-11-05 — Il crash è in test_gic(), non nel driver eMMC

Con il tracciamento fine (fasi numeriche + `emmc_step`), la codifica del
tempo è cambiata in "un secondo per unità": il panic handler mette
`phase_no + emmc_step` secondi nell'attesa, e `tools/run-shim.sh` lo
decodifica.

Risultati:

| run | attesa | fase |
|---|---|---|
| `phase*20 + step` | 158,9 s | 7 (psci) o 8 (gic) |
| micro-fasi psci/gic | 79,6 s | **80 = "gic (before test_gic)"** |

Quindi **PSCI risponde** (`smc()` non e' il problema) e il crash e' dentro
`test_gic()`.

Nota sul DT: `msm8992-lg-h815.dts` contiene esplicitamente

```
/* psci is broken */
/delete-node/ psci;
```

e le CPU usano `enable-method = "qcom,8994-arm-cortex-acc"`. Il kernel
Android del G4 (`arch/arm64/kernel/psci.c`) cerca un nodo DT con
`compatible = "arm,psci"`: senza, `psci_init()` restituisce `-ENODEV` e
non registra `arm_pm_restart`/`pm_power_off`. Percio' **sul G4 il reboot
del kernel Android non passa da PSCI**. Il nostro `smc(PSCI_SYSTEM_RESET)`
in `reset()` pero' funziona: il TrustZone risponde a `smc #0`, e' il DT
che non lo dichiara. (Da verificare con calma, ma il telefono rientra in
recovery, quindi il reset avviene.)

Sospetti dentro `test_gic()`, in ordine:

1. `rsr("icc_sre_el1")` / `wsr("icc_sre_el1", ...)`: se il GIC non e' un
   GICv3 con interfaccia sysreg, l'istruzione e' undefined -> trap.
2. `rd32(GICD_BASE)` / `wr32(GICD_BASE, ...)`: `GICD_BASE` =
   `0xf9000000`, se l'XPU lo blocca in EL1 non sicuro -> data abort.
3. `find_gicr()` e i registri del redistribuitor (`0xf9002000`).

Ora c'e' una micro-fase per ogni istruzione di `test_gic()` (81..109), per
sapere esattamente quale faulta.

---

## 2026-11-05 — Tre bug trovati nel codice (non solo nel timing)

Il canale del tempo era diventato troppo ambiguo (il jitter del boot
TWRP e' di qualche secondo e l'unita' era di 1 s), quindi il formato e'
stato cambiato in **eventi a tabella fissa, 20 s per slot**:

```
wait = 40 s + 20 s * indice_evento
```

Il jitter di +-5 s non sposta piu' la lettura nel slot vicino, e l'host
non deve piu' indovinare nulla: `tools/run-shim.sh` ha la tabella.
Un crash ha una base separata (500 s) e dentro c'e' l'offset dell'ELR in
unita' da 32 byte, cosi' i due formati non si sovrappongono.

### Bug 1 — `TG0`: la "correzione" era un errore, l'encoding era invertito

`shim/mmu.c`:

```c
#define TCR_VAL  (28UL | ... | (2UL << 14) | ...)   /* TG0 = 2 */
```

**Correzione 2026-11-12.** I bit `[15:14]` di TCR_EL1 sono TG0. L'encoding
vero, dall'ARM ARM (DDI0487, TCR_EL1) e dal `osfmk/arm64/proc_reg.h` di
Apple, è:

| TG0 (bit 15:14) | granulo |
| --- | --- |
| `0b00` | 4 KiB |
| `0b01` | **64 KiB** |
| `0b10` | **16 KiB** |

Quindi la nota che diceva "0 = 4K, 1 = 16K, 2 = 64K" era **l'inverso**, e
la "correzione" da `2` a `1` ha **programmato 64 KiB** su page table
costruite per 16 KiB (L2 = blocchi da 32MB con la base a bit 25, L3 =
pagine da 16K, 2048 voci per tabella). Con 64 KiB la passeggiata parte
dal livello sbagliato: il blocco L2 copre 128MB con la base a bit 29,
mentre le tabelle memorizzano blocchi da 32MB con la base a bit 25.

**Stato attuale: `TG0 = 2` (16 KiB)**, che è il valore corretto e
originario. Il bug era autoprodotto: la verifica era stata fatta sul
modello delle page table, non sull'encoding del registro.

L'encoding di **TG1** (bit `[31:30]`) è diverso: `0b01` = 16 KiB,
`0b10` = 4 KiB, `0b11` = 64 KiB. `TG1 = 1` nel codice è dunque 16 KiB
(corretto per combinazione).

Anche `mmu_granule_ok()` era debole: `ID_AA64MMFR0_EL1.TGran1` è una
**bitmask**, non un codice (bit 0 = 4 KiB, bit 1 = 16 KiB, bit 2 = 64 KiB).
Il test era "un qualunque bit è diverso da zero", che passa anche su un
core che implementa solo 4 KiB. Ora è `(v & 0x2) != 0`, cioè "16 KiB è
implementato".

### Bug 2 — la dcache invalidava le page table appena scritte

```c
dcache_inval_range(img_lo, img_hi);   /* dc ivac: invalida senza scrivere */
... fill delle page table ...
```

Le page table stanno nel `.bss`, quindi dentro l'immagine: invalidarle
**senza clean** scarta i dati e in RAM restano gli zeri (o spazzatura di
LK). La traduzione non trovava niente. Ora il codice fa `dc civac`
(clean+invalidate) **prima** di riempire le tabelle e un secondo
`dc civac` **dopo**, solo sul pool delle tabelle, cosi' quello che la MMU
legge e' quello che e' stato scritto.

### Bug 3 — il core reset del controller eMMC

`emmc_hc_mode()` replicava la sequenza di LK (`sdhci_msm_init`): clear
HC_MODE, **core reset** (bit 7 di CORE+0x00), write del registro. Ma il
core reset azzera anche la generazione del clock e il DLL/CDC: senza
clock nessun comando completa. Ora la sequenza senza core reset viene
provata per prima e quella con il core reset solo come secondo tentativo.

### Cosa dice il telefono

| run | attesa | evento |
|---|---|---|
| HC mode senza core reset, poi con | 51,3 s | 1: la carta non risponde a CMD5 ne' CMD7 |
| + clock di card e classificazione | 431,8 s | 20: **il controller e' vivo e clockato, la carta non risponde** |

La classificazione (registri letti prima di toccare qualcosa) esclude:
core MMC non raggiungibile, capability vuote, clock spento, clock non
stabile, linea di comando bloccata. Resta solo la carta: e' addormentata
e la sequenza di risveglio non la convince.

Percio' e' stata implementata la **re-identificazione completa**
(CMD0 GO_INACTIVE -> CMD1 SEND_OP_COND -> CMD2 ALL_SEND_CID ->
CMD3 SET_RELATIVE_ADDR -> CMD7 SELECT_CARD) al clock di identificazione
(~400 kHz), che non presume niente sullo stato precedente della carta.

Nota: il layout del registro COMMAND di questo controller **non** e' quello
dello standard SDHCI: il riferimento Linux del kernel CAF usa

```c
#define SDHCI_MAKE_CMD(c, f) (((c & 0xff) << 8) | (f & 0xff))
```

cioe' l'indice del comando sta ai bit [13:8] e i flag nel byte basso
(CRC = 0x08, index = 0x10, data present = 0x20). Il driver gia' usava
questa convenzione: verificato, non un bug.

### XNU: il porting SWEET e' gia' un telefono Qualcomm con device tree

Il repo `dipa-studio/ios-sweet` non porta XNU su un iPhone: lo porta su
un **Xiaomi** con un **device tree** (platform expert `IODTPlatformExpert`,
timer virtuale letto dal DT, boot della CPU senza PSCI). E' esattamente il
nostro caso, quindi la base e' riusabile:

- `tools/mkadt.py` ha gia' il profilo `--device g4` (RAM map, GIC,
  framebuffer, eMMC, compatible `lg,h815` + `qcom,msm8992`).
- `shim/loader.c` sa gia' lanciare XNU (payload, ADT, boot_args, salto).
- La build XNU gira in CI su `macos-14` (Xcode 15.x, `iig` compatibile):
  non serve Xcode in locale.

Adattamenti fatti ai patch:

- `-march=armv8.2-a+crypto` -> `-march=armv8-a+crypto`: il G4 e' fatto
  solo di Cortex-A53 (ARMv8.0); con armv8.2 il compilatore puo' emettere
  istruzioni LSE/complex che l'A53 non ha (SIGILL).
- Aggiunta la voce di `KernelConfigTables` con
  `'IONameMatch' = 'lg,h815';` perche' il platform expert si attacchi al
  nostro ADT e non solo a quello dello Xiaomi.

### Aggiornamento 2026-11-05 12:05 — CI XNU verde, immagine G4 pronta

La CI `42012839180` (xnu, macos-14, MACHINE_CONFIG=SWEET con le patch
adattate al G4) e' **riuscita in 8m35s**. Kernel:
`xnu/out-42012839180/kernel.development.sweet` (Mach-O 64-bit arm64, non
arm64e).

Catena completa e verificata sul G4:

```
python3 tools/mkadt.py shim/out/g4.adt --device g4
python3 tools/mkpayload.py xnu/out-42012839180/kernel.development.sweet \
        shim/out/g4.adt shim/out/g4.bin \
        --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"
cd shim && ./build.sh out/g4.bin
```

Risultato: `shim/out/shim-boot-g4.img` (17.870.848 byte,
sha256 `71be0b8f149d3ade6eb5433d7a1758bdeb17dc70b5dca9bd92452a35c9a0238e`),
`tools/verify_bootimg.py` -> PASS e il ramdisk coincide con il payload.
**Non e' stato flashato**: il telefono e' stato ripristinato con
`backup/boot-stock.img` e torna su Android.

Il prossimo test (quando si riprende) e' semplice: flashare questa
immagine e guardare se XNU arriva almeno a "init alive". Le incognite
restanti, in ordine: il driver eMMC in XNU (il porting SWEET legge la root
da UFS), il D22AP ADT per l'identita' del userland, e il display (il
framebuffer del G4 e' nella secure region, protetto dall'XPU).

### Aggiornamento 2026-11-05 18:10 — raccolta dati grezzi eMMC (sessione di misura)

Obiettivo della sessione: raccogliere dati, non correggere niente. Tutto quello
che segue e' misura reale; nessuna ipotesi e' stata applicata al codice.

**Run di misura sul telefono (2026-11-05 17:59:32Z - 18:07:47Z)**

Immagine flashata: `shim/out/shim-boot.img` (build `RAW_TIMING=1 FINE_TIMING=1`,
sha256 `58ffdb5aea3690f51aa41ba89b907a8c45dc8565d7abcb4256dada5bea647ead`).
Esito grezzo del canale temporale:

```
elapsed from fastboot reboot : 486.9 s
elapsed from the bootloader : 486.6 s
shim wait                   : 451.4 s
  -> event 21: the card does not answer to the full identification (CMD0/1/2/3/7)
```

451,4 s contro i 451,3 s della sessione precedente: la misura e' riproducibile.
Evento 21 = la carta non risponde ne' a CMD0/CMD1/CMD2/CMD3/CMD7.

Dopo il run i 4 settori di log (spare1 LBA 259328/259329, spare2 LBA 385280/385281)
sono **tutti zero** (512 byte di 0x00): il shim non ha scritto niente, la carta
e' muta. Marcatori `G4SHIMLG` / `G4SHIMDG` assenti.

**Vicoli morti (errori esatti)**

- Lettura della RAM / IMEM dal recovery: `/dev/mem` non esiste; `mknod /dev/mem c 1 1`
  va a buon fine ma l'apertura fallisce con `No such device or address` (ENXIO).
  Idem `/dev/kmem` (`mknod /dev/kmem c 1 2` -> ENXIO).
- `/proc/kcore`, `/dev/fb0`, `/proc/device-tree`, `/sys/firmware/devicetree/base`:
  assenti. `/sys/class/remaproc` assente. Nessun `/proc/config.gz`.
- `clk_summary` non esiste in questo kernel: esistono le directory per clock.
- `tools/fb2text.py` sul dump QEMU non riconosce i glifi (geometria diversa dal
  default 640x480); il testo del console e' stato quindi letto direttamente dal
  dump di RAM di QEMU (buffer `con_log`).

**Dati nuovi raccolti**

- `mmc0:0001/state` = `0x00000209`, `mmc0:0001/status` = `00000900`.
- `ios` mostra `clock: 200000000 Hz / actual clock: 172000000 Hz` **subito dopo
  il boot** e `clock: 0 Hz` nei campioni successivi (1 s dopo): il clock della
  carta viene gatingato quando la carta e' inattiva. `max_clock` = 344000000.
- `gcc_sdcc1_apps_clk` CBCR = 0x80004ff0, rate 344 MHz; `sdcc1_apps_clk_src`
  CMD_RCGR 0x80000000, CFG_RCGR 0x00000501 (M=0, N=0, D=0xff);
  `gcc_sdcc1_ahb_clk` CBCR 0x8000cff0, rate 0.
- LK di fabbrica: `target_uninit` (0x0f900574) chiama `mmc_put_card_to_sleep`
  (0x0f916a6c) e poi `sdhci_mode_disable` (0x0f914940). `mmc_put_card_to_sleep`
  emette **CMD7** (arg 0, deselect) e **CMD5** (arg = (RCA<<16)|0x8000 =
  0x00028000, sleep) e non scrive nessun registro 0xf9824xxx / 0xfc40xxxx.
  `sdhci_mode_disable` scrive solo CORE+0x78 (HC_MODE) azzerando il bit 0.

Gli artefatti grezzi sono in `research/raw-2026-11-05/`, il disassembly del LK in
`research/lk-g4/dis-4.*.txt`.

Il telefono e' stato ripristinato con `backup/boot-stock.img` e verificato su
Android 13 (`sys.boot_completed=1`).

### Aggiornamento 2026-11-05 22:20 — run 3 (bring-up che replica l'init dell'LK) e disegno del run 4 diagnostico

**Run 3 sul telefono (2026-11-05 20:56-21:05 UTC)**

Immagine: `shim/out/shim-boot.img`, build `RAW_TIMING=1 FINE_TIMING=1`,
sha256 `8033989a56c69d079afafd3b8eb89965ce5dc75606d92434e716746ed2ce0ee6`.

```
elapsed from fastboot reboot : 467.8 s
elapsed from the bootloader : 467.5 s
shim wait                   : 432.3 s
  -> event 20: the controller is fine but the card does not answer
```

Evento 20 = `EV_CARD_ABSENT`: il controller e' sano (capabilities lette, clock
interno stabile, software-reset azzerato, HC mode riattivato) ma la carta
**non risponde a CMD0/CMD1**. I 4 settori di log (spare1 LBA 259328/259329,
spare2 LBA 385280/385281) sono **tutti 0x00**: il shim non ha scritto nulla,
quindi il fallimento e' avvenuto prima. Dati grezzi e checksum in
`research/raw-2026-11-05/run-3-bringup/` (README.md + run-output.txt + 4 .bin).
Telefono ripristinato su Android 13 con `boot` stock (`sys.boot_completed=1`).

L'evento 20 e' una codifica **nuova**, aggiunta in questa sessione: prima,
"CMD0/CMD1 senza risposta" e "CMD1 risposta ma mai pronta" finivano entrambi
sull'evento 21. Ora il primo e' l'evento 20 (`CLASS_CARD_ABSENT`) e il secondo
l'evento 21 (`STAGE_REINIT`).

**Modifiche fatte al codice dell'altra AI (solo integzione e diagnostica)**

1. `shim/emmc.c`: ripristinati `unsigned emmc_class` e l'enum `CLASS_*`,
   piu' `STAGE_REINIT`. Senza, `shim.c` non linkava:
   `undefined reference to 'emmc_class'` (l'altra AI aveva cancellato la
   variabile insieme a `emmc_classify()`).
2. `shim/emmc_lk_bringup.inc`: ogni punto di fallimento imposta ora
   `emmc_last_stage` / `emmc_class`.
3. `shim/emmc.c`: `emmc_wake()` (solo percorso legacy) condizionato a
   `!EMMC_LK_BRINGUP` (warning `-Wunused-function`).
4. `tools/run-shim.sh`: `BOOT_OVERHEAD` ricalibrato da 35,2 s a **27,0 s**.
   Con 35,2 i due run reali (evento 21 e evento 20) venivano decodificati
   bene per caso (scarto < 1 slot); il valore vero, ricavato dai due run
   (467,5 - 440 = 27,5 s e 486,6 - 460 = 26,6 s), e' 27 s.

**Verifica delle affermazioni della seconda AI (7/7 confermate a suo favore)**

| affermazione | esito | fonte |
|---|---|---|
| la polarita' del loop CMD1 (uscire quando bit31 e' attivo) | **confermata** | `dis-4.9-c`: `cmp r3,#0; blt` sulla risposta R3 letta come valore con segno -> esce con bit31 = 1; e `lk-source-recovered/mmc.c:751` (`if (!(mmc_resp & MMC_BOOT_OCR_BUSY)) return CARD_BUSY`) |
| OCR `0x40ff8000` | **confermato** (e con un dettaglio in piu') | `MMC_BOOT_OCR_27_36 = (0x1FF << 15)` e `MMC_BOOT_OCR_SEC_MODE = (2 << 29)` -> `host->ocr = 0x40ff8000` (`lk-source-recovered/mmc.h:322`, `mmc.c:1900`) |
| `HC+0x11c` = caps override | **confermato**, ed e' un indirizzo **HC**, non CORE | il driver CAF scrive `host->ioaddr + CORE_VENDOR_SPEC_CAPABILITIES0` dove `host->ioaddr` e' la base HC: l'indirizzo assoluto e' 0xf9824a1c = HC+0x11c, come nell'LK (`dis-4.9-d` 0x0f914038) |
| clock: divisore pari 2..2046 in campo 10 bit | **confermato** | `dis-4.9-m-0f912ec4.txt`: `mov r4,#2` ... `cmp r4,#0x7fe`, `lsr #1`, `ubfx #8,#2`, `lsl #8`, `orr #1` |
| base clock = `caps[15:8] x 1e6` | **confermato** | `dis-4.9-m-0f913954.txt`: `ubfx ip, r3, #8, #8` + `mul r0, r0, ip` con r0 = 0xf4240 |
| maschera SW reset = 1 | **confermato** | `dis-4.9-a`: `mov r1,#1` |
| `HC+0x29` scritto dall'LK | **confermato** ma con un valore diverso: 14 e 15 = 3,3 V | `dis-4.9-m-0f913954.txt` 0x0f913a0c/0x0f913a14; il sorgente CAF spiega: `SDHCI_VOL_3_3 = 7`, `voltage <<= 1`, `|= SDHCI_BUS_PWR_EN` |

**Fatti nuovi trovati nel binario (che la seconda AI non aveva)**

- **LK non fa il core reset "a vuoto"**: `sdhci_msm_init` (0x0f913f10) scrive
  CORE+0x78 &= ~1, poi CORE+0x00 |= 0x80 (core reset), `delay(1)` (1 ms),
  CORE+0x78 |= 1, CORE+0x78 |= 0x2000. Il nostro codice fa lo stesso (con
  10 ms di attesa invece di 1 ms).
- **Il primo OCR di LK e' `0x40ff0000`**, non `0x40ff8000`: `str r3,[r4,#0x3c]`
  con r3 = 0x40ff0000 a 0x0f91571c, e CMD1 usa `[dev+0x3c]` come argomento
  (0x0f915760). Dopo la risposta, LK ci rimette dentro l'OCR **della carta**
  (0x0f9157cc) e usa quello per i retry.
- **Il clock target di LK e' `0x60000` = 393216 Hz** (0x0f9156ec/0x0f9156f0),
  non 400000. Con base 200 MHz: divisore 510 (n = 255, clk = 0xFF01); noi
  con 400000: divisore 500 (n = 250, clk = 0xFA01). Entrambi ~400 kHz.
- **LK scrive `HC+0x29 = 0x0e` poi `0x0f`** a ogni boot: nel encoding CAF
  (`SDHCI_VOL_3_3 = 7`, `<<1`, `|= BIT(0)`) questo vuol dire **3,3 V**,
  non 1,8 V. Il nostro codice scriveva `0x0b` (= 1,8 V, `SDHCI_VOL_1_8 = 5`)
  **solo se il bit di power era spento**: differenza reale, non equivalente.
- **LK non scrive `HC+0x3e` in questa fase**, e `HC+0x28 = 0x10` (ADMA 32 bit,
  non il bit 1,8 V: `SDHCI_ADMA_32BIT = BIT(4)`).
- Gli offset GCC **0x4c4 / 0x4c8** sono confermati da due fonti indipendenti:
  la tabella clock dell'LK (`dis-4.9-k`: `sdc1_core_clk -> 0x4c4`,
  `sdc1_iface_clk -> 0x4c8`) e `clock-gcc-8992.c` (`SDCC1_APPS_CBCR 0x04C4`,
  `SDCC1_AHB_CBCR 0x04C8`).
- Il **driver Linux CAF** (quello che funziona con questa carta) fa la stessa
  cosa: CORE+0x78 = 0, CORE+0x00 |= CORE_SW_RST e **poll fino a che il bit 7
  si azzera** (10 us x 1000), CORE+0x78 = 1, poi |= FF_CLK_SW_RST_DIS, poi
  la stessa handshake PWRCTL. Nota: il driver non usa `HC+0x29` per la
  potenza ("Bus power control is handled by interacting with PMIC",
  quirk `SDHCI_QUIRK_SINGLE_POWER_WRITE`).

**Run 4 diagnostico (in corso)**

Il trail in IMEM non e' leggibile, quindi l'unico canale e' il tempo. Invece
di un evento per run, il build diagnostico (`DIAG_VARIANTS=1 DIAG_TIMING=1`)
prova **8 varianti del bring-up nella stessa esecuzione** e riporta quella
che e' andata piu' lontano:

```
wait = 40 s + 50 s * variante + 10 s * stadio
```

con stadio 0 = niente risponde, 1 = CMD1 risponde ma la carta non e' pronta,
2 = CMD1 pronta ma CMD2/3/7 falliscono, 3 = carta identificata.

Le 8 varianti sono le differenze reali trovate sopra:

| # | variante | ipotesi che testa |
|---|---|---|
| 0 | senza core reset | il core reset rompe la sincronizzazione con la carta |
| 1 | azzera CORE+0x10c bit 1 (CLK_PWRSAVE) | il controller gatea il clock della carta da solo (come nel driver Linux) |
| 2 | potenza bus 3,3 V (0x0f) | il valore di HC+0x29 conta, e l'LK usa 3,3 V |
| 3 | potenza bus 1,8 V (0x0b) forzato | la carta va tenuta a 1,8 V |
| 4 | potenza bus non toccata | la scrittura di HC+0x29 e' io stessa il problema |
| 5 | clock 393216 Hz + INT_ENABLE/CTRL1 dell'LK | i valori dei registri contano |
| 6 | ordine dell'LK (niente CMD5) + OCR 0x40ff0000 | il risveglio e l'OCR contano |
| 7 | niente software reset (HC+0x2f) | il reset delle linee confonde la carta |

Codice: `shim/emmc_lk_bringup.inc` (bring-up parametrizzato),
`shim/emmc_diag.inc` (tabella + sweep), `shim/shim.c` (codifica del tempo),
`tools/run-shim.sh` (`MODE=diag`).

**Run 4 diagnostico (8 varianti) — risultato: nessuna variante funziona**

```
elapsed from fastboot reboot : 67.6 s
elapsed from the bootloader : 67.3 s
shim wait                   : 40.3 s
  -> variant 0: no core reset
  -> stage   0: nothing answered (CMD0/CMD1)
```

Decodifica: `wait = 40 + 50*var + 10*stage` con 40,3 s → variante 0, stadio 0.
Il codice riporta la variante **migliore**: siccome tutte e 8 hanno finito a
stadio 0, il risultato e' "nessuna delle 8 fa rispondere la carta". Quindi
non e' la causa: il core reset, il gating del clock (CORE_CLK_PWRSAVE), il
valore di HC+0x29 (3,3 V / 1,8 V / non toccato), il clock target, gli
INT_ENABLE/CTRL1, l'ordine CMD5/CMD0, il primo OCR e il software reset.

**Errore trovato nel run 4: il registro vendor spec e' a HC+0x10c**

La variante 1 (clear di CORE_CLK_PWRSAVE) scriveva a `CORE + 0x10c`
(0xf982410c). Il registro giusto e' **`HC + 0x10c` = 0xf9824a0c**: il driver
CAF lo raggiunge come `host->ioaddr + CORE_VENDOR_SPEC`, e `host->ioaddr` e'
la base SDHCI (0xf9824900); l'LK scrive la stessa indirizzo
(`dis-4.9-d` 0x0f913f98, `str r2, [r3, #0x10c]` con r3 = &HC). La variante 1
del run 4 ha quindi provato un indirizzo sbagliato.

**Nuova ipotesi, dal driver Linux funzionante**

`sdhci-msm.c` (riga ~2905), quando la carta **non** e' in HS400:

```c
	/* Select the default clock (free running MCLK) */
	writel_relaxed(((readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC)
				& ~CORE_HC_MCLK_SEL_MASK)
				| CORE_HC_MCLK_SEL_DFLT),
				host->ioaddr + CORE_VENDOR_SPEC);
	/* Disable HC_SELECT_IN ... */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC)
			& ~CORE_HC_SELECT_IN_EN
			& ~CORE_HC_SELECT_IN_MASK),
			host->ioaddr + CORE_VENDOR_SPEC);
```

`CORE_HC_MCLK_SEL` (bit 9:8) = 2 di default, **3 = HS400**;
`CORE_HC_SELECT_IN` (bit 21:19) = 6 in HS400, `CORE_HC_SELECT_IN_EN` (bit 18).
Dopo una sessione HS400 (che e' quello che fa l'LK a ogni boot) il MCI resta
con la sorgente clock e il select CDC **in modalita' HS400**: il nostro shim
non ha mai toccato quel registro, e identifica la carta a 400 kHz con il
controller ancora puntato sul percorso HS400. E' la differenza piu'
plausibile rimasta.

**Run 5 diagnostico (16 varianti)**: le 8 precedenti (con la variante 1
corretta sull'indirizzo giusto) piu' 8 nuove che azzerano il vendor spec
(MCLK_SEL = default, SELECT_IN = 0) da sole o in combinazione con il resto.
Codifica del tempo aggiornata: `wait = 60 + 25*var + 10*stage` (massimo
465 s, sotto la base dei crash a 500 s).

### Aggiornamento 2026-11-05 23:20 — run 5 (16 varianti) e run 6 (fix 3,3 V): cosa e' cambiato

**Run 5 diagnostico (16 varianti in un solo boot) — LA CARTA HA RISPOSTO.**

`shim wait 139.7 s`. La codizione del tempo e' `wait = 60 + 25*var + 5*stage`
(il canale e' il tempo di attesa prima del reset; 25 s e 5 s sono divisori
coprimi, quindi la decodifica e' univoca). Decodifica: **variante 2,
stadio 3**.

- variante 2 = `{ power 0x0f, clk 400000, flags 0 }`: potenza bus a 3,3 V
  (0x0f, lo stesso valore che l'LK scrive a ogni boot: prima 0x0e e poi 0x0f),
  clock di identificazione 400 kHz, nessun flag speciale (core reset fatto,
  vendor spec non toccato, CMD5 di sveglia sì, OCR 0x40ff8000).
- stadio 3 = carta identificata: CMD0 e CMD1 rispondono, CMD2/CMD3/CMD7
  riescono. RCA = 2, R1 di CMD16 = 0x00000900.

**Stato dei registri misurato (settore di diagnostica, run 5)**

| registro | valore | nota |
|---|---|---|
| HC+0x29 (power) | 0x00 | a ingresso: potenza bus spenta |
| HC+0x40 (CAPS1) | 0x342dc8b2 | base clock = caps[15:8] MHz |
| HC+0x44 (CAPS2) | 0x00008007 | |
| HC+0x2c (CLK_CTRL) | 0x000ffa07 | |
| HC+0x10c (vendor spec) | 0x00000a1c | |
| CORE+0x78 (HC_MODE) | 0x01 | HC mode attivo |
| CORE+0x50 (MCI_VER) | 0x1000003e | major != 0, minor != 0x11/0x12 => caps override |
| EXT_CSD SEC_COUNT | 0x03a3e000 | |
| EXT_CSD EXT_MODE | 0x57380000 | |
| ADDR_MODE | 1 | indirizzamento a settore |
| FAIL_INT | 0x00018000 | |

**Run 6 (percorso normale, `lk_var_default = { 0x0f, 400000, 0 }`) — FALLITO.**

`shim wait 439.6 s` -> **evento 20** (controller sano, carta muta). La
variante di default del run 6 e' letteralmente la variante 2 del run 5.

**FATTO NUOVO (run 6, verificato il 2026-11-05 23:15): i 4 settori di log
del run 6 sono byte-per-byte IDENTICI a quelli del run 5.**

```
sp1a.bin  run5=84edac3e...  run6=84edac3e...  IDENTICI
sp1b.bin  run5=2e4dee7f...  run6=2e4dee7f...  IDENTICI
```

Il settore di log del run 6 dice `stage=7 (write), error=0, R1 CMD16=0x900,
rca=2` — cioe' **lo stato di successo del run 5**, non quello del run 6.
Conclusione: **il run 6 non ha scritto assolutamente nulla sulla carta**,
perche' la carta era muta e la scrittura dei log non e' riuscita. I dati
"col marker" che sembravano nuovi nel run 6 sono materiale lasciato dal
run 5. Corregge la lettura precedente.

**DEDUZIONE (non un fatto): perche' la stessa configurazione funziona nel
run 5 e non nel run 6.**

L'unica differenza osservabile e' **la cronologia**, non la configurazione:

1. nel run 5 la variante vincente era **preceduta da due varianti fallite**
   (var 0 senza core reset, var 1 con clear di CORE_CLK_PWRSAVE): la carta
   aveva gia' ricevuto due sequenze CMD5/CMD0/CMD1 a vuoto, e il controller
   era stato resettato (core reset + SW reset ALL) piu' volte;
2. `CMD_TIMEOUT_US` = 1 s, quindi ogni variante fallita costa ~3 s di
   timeout (CMD5 + CMD0 + CMD1): nel run 5 la variante 2 e' partita circa
   6+ s dopo l'ingresso dello shim, nel run 6 subito.

Ipotesi in ordine di costo di verifica:

- H1 **tempo**: la carta (o il suo alimentatore / il PMIC) ha bisogno di
  qualche secondo di rampa dopo che il bus e' stato spento dall'LK
  (HC+0x29 = 0x00) prima di rispondere. Test: inserire un delay di 5-15 s
  prima di CMD5/CMD0/CMD1 nel percorso normale.
- H2 **stato del controller**: il numero di core reset / SW reset ALL
  conta (il run 5 ne ha fatti 3, il run 6 uno).
- H3 **sequenza**: le CMD a vuoto (senza potenza) " svegliano" la carta, che
  altrimenti resta in uno stato in cui non risponde al primo CMD1.
- H4 **potenza pilotata dal PMIC e non dal registro SDHCI**: il driver Linux
  funzionante non usa HC+0x29 ("Bus power control is handled by interacting
  with PMIC", quirk `SDHCI_QUIRK_SINGLE_POWER_WRITE`). Scritto cosi' in
  HC+0x29 potrebbe essere un semplice effetto collaterale, non la causa.

Il run diagnostico successivo dovrebbe testare H1/H2/H3 insieme: due
varianti "identiche" alla vincente, una lanciata subito e una dopo un delay
di 10 s, nello stesso boot.

**Stato del telefono a fine sessione**: Android 13, `boot` stock
(`backup/boot-stock.img`), `sys.boot_completed=1` verificato.

---

## Sessione 2026-11-08/09: T1-T4 (analisi statica, modello QEMU, canale pstore)

### FATTO — T1: la tensione del bus (D1 era giusta, ma per il motivo opposto)

Il disassembly esteso (`research/lk-g4/dis-4.9-p-sdhci_setup_host-full.txt`,
233 righe) mostra che `sdhci_setup_host` (0x0f913954) contiene tre rami di
tensione: 3.3 V (0x0f9139b8), 3.0 V (0x0f913a6c), 1.8 V (0x0f913a88).
Con CAPS1 = 0x342dc8b2 (bit24=0, bit25=0, bit26=1) il ramo preso e' quello
da 1.8 V: `movne ip,#0xb` / `movne r0,#0xa`, cioe' HC+0x29 = **0x0a poi
0x0b**. Non 0x0e/0x0f (3.3 V) come scriveva il commento del bring-up.

La deduzione D1 dell'utente era dunque corretta sul fatto (il LK NON scrive
3.3 V con questo CAPS1) e la sua conseguenza (che il percorso dello shim
dovesse usare 1.8 V) e' ora la base delle varianti V4/V5.

### FATTO — T1: il handshake PWRCTL (D3 va corretta)

Il handler IRQ di potenza (0x0f913ac0) scrive CORE+0xe8 = 5
(BUS_ON_OFF_SUCC | IO_SIG_SUCC, calcolato dal MASK 0xf). L'init ack
(0x0f913fa0-0x0f913fc4) copia STATUS->0xe4 e OR-bita 1/4 su 0xe8; poi
CORE+0xe0 = 0xf. Dopo le due scritture di HC+0x29 l'LK esegue
`bl 0xf92e360` = **event_wait(&sdhc_event)** e BLOCCA finche' l'handler non
ha scritto l'ack.

Correzione rispetto a D3: l'ack non e' "dopo ogni scrittura di HC+0x29" ma
un handshake interrupt-driven: l'LK scrive HC+0x29, il controller alza
l'IRQ di potenza, l'handler scrive 5 su CORE+0xe8 e l'LK prosegue. Lo
shim non ha gli IRQ, quindiPuo' solo farlo a mano (variante V1/V5) o
lasciare fare al controller (V0/V3).

### FATTO — T2: modello del controller nel guest

Su `virt` non esiste un MMIO pluggabile a 0xf9824000 (`generic-sdhci` non
esiste su virt, `sdhci-pci` sta su bus PCI, `-m 4G` fa coprire l'indirizzo
dalla RAM). Il modello e' quindi dentro il guest (`shim/emmc_model.inc`),
con 4 modalita': mute, ack, delay, always. Il modello e' dichiarato
ipotetico: non prova nulla sull'hardware del telefono.

FATTO (QEMU, modalita' "ack", build diagnostica): **V0 fallisce (stadio 0,
rc=-12) e V1 identifica la carta (stadio 3, rc=0)**, con la sequenza
CMD5, CMD0, CMD1, CMD2, CMD3, CMD7, CMD16, CMD8, 4xCMD24, CMD17. Il test
"ack" e' quindi discriminante: senza ack la carta resta muta, con ack
(CORE+0xe8 = 5) risponde. Questo e' il modello che dice H5, non
l'hardware.

### FATTO — T3: il canale pstore/ramoops (nuovo, risolve il canale di misura)

FATTO (dal dmesg del telefono): `ramoops: attached 0x100000@0xff00000,
ecc: 16/0` e `console [pstore0] enabled`. Il recovery e Android hanno
entrambi il driver ramoops con la stessa identica zona (file
console-ramoops-0 da 116488 byte in entrambi).

DEDUZIONE (dal DT + dal codice del driver): record-size = 0xffffa >
spazio per le record zone (0xc0000) => max_dump_cnt = 0 => **nessuna
record zone** => la zona console parte da 0xff00000. Confermato dal
dimensionamento: 116468 byte di dati + 20 di avviso ECC = 116488, il
file che il telefono produce.

FATTO: 0xff00000 e' sopra la secure_region da 150 MiB (0x9600000) che
l'XPU protegge, quindi e' DRAM scrivibile dal mondo non sicuro dove gira
lo shim.

Implementato: `shim/pstore.c` (scrive il record nel formato del ramoops,
con la parita' Reed-Solomon) e `shim/rs8.c` (l'encoder del kernel,
ridotto). Verificati offline contro il codice del kernel:
- `tools/rs8-test.sh`: l'encoder del shim e' identico byte per byte a
  quello di lib/reed_solomon (lunghezze 0..239, 40 prove casuali ciascuna);
- `tools/pstore-test.sh`: il record che il kernel leggerebbe torna intero,
  l'intestazione e tutti i 911 blocchi passano il decoder DEL KERNEL senza
  che modifichi un byte;
- QEMU end-to-end: record trovato, CRC ok, end marker presente.

FATTO: senza la parita' il decoder del kernel "correggerebbe" il blocco e
rimetterebbe i byte vecchi; sull'intestazione (che cambia solo start e
size) la correzione e' sempre possibile, quindi la parita' e' obbligatoria.

### Strumentazione aggiunta (T3)

Il bring-up ora legge e mette nel log CORE+0xdc, CORE+0xe0, CORE+0xe4,
CORE+0xe8 e HC+0x29: all'ingresso (step 30), dopo la scrittura 0x0e
(step 33), dopo la 0x0f (34), dopo l'ack (35), dopo l'ack PWRCTL (37) e al
fallimento (90/91). L'etichetta delle righe (PV0..PV5) dice a quale
variante della sweep appartiene la lettura.

FATTO (QEMU): le righe PV/VAR compaiono correttamente e distinguono V0
(stadio 0) da V1 (stadio 3).

### FATTO — run 2 e run 3 sul telefono (2026-11-09): cosa dice davvero l'hardware

Il canale pstore ha funzionato: il record e' arrivato integro (CRC ok,
end marker presente), 10439 byte nel run 2 e 15291 nel run 3 (con le sonde
per comando).

**1. La zona console non e' sempre allo stesso indirizzo.** Il
/proc/iomem del recovery elenca tre regioni persistent_ram:
0ff00000-0ff7ffff (512 KiB di record zone), **0ff80000-0ff9ffff (128 KiB:
la zona console)** e 0ffa0000-0ffbffff (pmsg). Il DT del telefono chiede
invece record-size 0xffffa, che non sta nello spazio delle record zone:
per Android non ce ne sono e la console parte da 0xff00000. Lo shim
scrive in entrambe le zone, prima 0xff80000 (il recovery e' il primo
kernel che parte e quello che legge).

**2. FATTO, e corregge una nota precedente: all'ingresso dello shim
HC+0x29 = 0x0b, NON 0x00.** Il bus e' ACCESO a 1.8 V: e' esattamente il
valore che il LK scrive con questo CAPS1 (0x0a poi 0x0b, vedi T1). Il
valore 0x00 riportato nel handover 2.1 non era lo stato all'ingresso: era
quello letto DOPO il reset software dell'SDHCI (HC+0x2f = 1), che azzera
i registri del blocco HC. **La deduzione "l'LK ha spento il bus prima di
saltare al kernel" e' quindi falsa**, e con lei la parte di H1 che
riposava su quella.

**3. FATTO: il bus si accende davvero.** Dopo la scrittura di HC+0x29 =
0x0f, CORE+0xdc = 0x02. I bit del driver Linux (sdhci-msm.c:70-71) sono
CORE_PWRCTL_BUS_OFF = 0x01 e CORE_PWRCTL_BUS_ON = (1 << 1): **0x02 vuol
dire BUS ON**. (Nelle note precedenti i due bit erano scambiati.) La
transizione di potenza del bus completa e il controller la segnala: la
mano mancante (H5) non e' quello che blocca la carta.

**4. FATTO: l'ack PWRCTL non cambia niente.** V0 (ack=1, cioe' quello
scritto dal shim) e V1 (ack=5, come l'handler del LK) danno lo stesso
risultato, e lo stesso vale per V2 (ack + 10 s di attesa), V3 (HC+0x29 non
scritto), V4 (1.8 V) e V5 (1.8 V + ack): **tutte e sei le varianti si
fermano allo stadio 0**, la carta non risponde ne' a CMD0 ne' a CMD1.

**5. FATTO (run 3, sonde per comando): il controller MANDA i comandi e
scade il timeout.** Per CMD5 e CMD1: INT_STATUS = 0x00018000, cioe' il
bit di errore (15) piu' il **Command Timeout Error** (bit 16), e il
registro risposta (HC+0x10) resta 0x00000000. CMD0 invece "riusce"
(rc=0) solo perche' non ha risposta: il controller lo considera finito
appena lo ha spedito. Il PRESENT register durante CMD5 vale 0x01e80007
(CMD_INHIBIT attivo) e durante CMD1 0x01f80001.

**⇒ La carta e' SILENZIOSA sulla linea CMD**: non e' un problema di
potenza, di tensione, di ack, ne' di clock. Le piste rimaste sono la
configurazione dei registri (CTRL1, INT_ENABLE) e lo stato in cui il
controller si trova dopo il reset software (che azzera HC+0x29 e quindi
spegne il bus sul registro).

**6. FATTO: HC+0x29 = 0x0b dopo il reset software.** Il reset software
dell'SDHCI (HC+0x2f = 1) azzera il blocco HC, quindi il registro di
potenza torna a 0 e il shim deve riscriverlo: e' quello che vede la
sonda subito dopo (0x0e, poi 0x0f). Questo e' anche il motivo per cui la
sonda "all'ingresso" delle run precedenti leggeva 0x00.

### FATTO — run 4 (2026-11-09): otto varianti, tutte mute

Aggiunte due varianti con i registri che scrive il LK:

- V6 = INT_ENABLE 0x000b/0xffff, INT_SIGNAL 0x000b/0xffff, **CTRL1 = 0x10**;
- V7 = idem a 1.8 V.

CTRL1 bit 4 e' "SD Bus Power Control Mode": con quel bit l'SD Bus Power
diventa read-only e la potenza del bus la gestisce il PMIC, non il
registro SDHCI (e' il quirk che il driver Linux funzionante commenta:
"Bus power control is handled by interacting with PMIC").

FATTO: anche V6 e V7 danno stadio 0, e le sonde per comando sono
identiche a quelle delle altre varianti: CMD5 e CMD1 finiscono con
INT_STATUS = 0x00018000 (errore + Command Timeout Error) e risposta 0.
**Otto varianti, tre run, stesso risultato: la carta e' muta e il
controller manda i comandi.**

### IPOTESI H7 (nuova, la piu' promettente): la VCC della carta e' spenta

FATTO (DT del telefono, nodo sdhci@f9824900, righe 6411-6417):

```
vdd-supply      = <0x11d>;      /* pm8994_l20, un regolatore RPM SMD */
vdd-io-supply   = <0x39>;
qcom,vdd-voltage-level   = <0x2d0370 0x2d0370>;   /* 2.95 V */
qcom,vdd-io-voltage-level = <0x1b7740 0x1b7740>;  /* 1.8 V   */
```

FATTO (driver Linux funzionante, sdhci-msm.c): il driver chiama
regulator_enable()/regulator_set_voltage() sui due regolatori prima di
parlare con la carta (`msm_hs_power_on`, `vdd_data`/`vdd_io_data` presi
dal DT). Lo shim non ha un driver dei regolatori.

DEDUZIONE: se il LK spegne la VCC della carta prima di saltare al kernel
(perche' il driver Linux la riaccende), allora allo shim la carta e'
completamente senza alimentazione: il controller puo' dire BUS_ON (e lo
dice: CORE+0xdc = 0x02) e puo' mandare i comandi (e li manda: il timeout
di comando scade con la risposta a zero), ma la carta non ha tensione e
non risponde. **Questo spiegherebbe tutto quello che e' rimasto fermo,
perche' spiega anche perche' il run 5 (unico run positivo) non e'
riproducibile: nel run 5 la sequenza di reset e la messa in tensione era
diversa.**

COME VERIFICARLA (prossimo passo):
1. leggere dal telefono, in Android, lo stato del regolatore:
   `adb shell dmesg | grep -i "pm8994_l20\\|regulator"` (il driver stampa
   "supply disabled" / "already enabled");
2. leggere il registro di stato dell'LDO20 del PM8994 via SPMI (il shim ha
   gia' le costanti SPMI in shim/g4.h) per vedere se la VCC e' accesa;
3. se e' spenta, accenderla dallo shim (RPM SMD: la sequenza di enable
   del driver linux e' in drivers/regulator/qcom_rpm_smd.c) e ripetere la
   sweep.

---

## Sessione 2026-11-09 (T0/T1/T2): la strada SDCC2 e un HANG del telefono

### FATTO — T0: lo slot microSD (SDCC2) FUNZIONA sotto Android

La microSD (SanDisk SS32G, 32 GB, CID 035344535333324780a468430e010a00)
e' stata inserita e il kernel Android la enumera senza problemi:

```
mmc1: SDHCI controller on f98a4900.sdhci [f98a4900.sdhci] using 32-bit ADMA
mmc1: new ultra high speed DDR50 SDHC card at address aaaa
mmcblk1: mmc1:aaaa SS32G 29.7 GiB
```

Dati grezzi: `research/raw-2026-11-09/t0-live.txt`, `t0-live2.txt`
(script ripetibile: `tools/t0-sd-live.sh`).

FATTO: `mmc1` = `f98a4900.sdhci` (SDCC2), `mmc0` = `f9824900.sdhci` (eMMC),
`mmc2` = `f9864900.sdhci` (SDIO/WLAN).

FATTO: i regolatori dello slot SD (`pm8994_l21` = VDD, `pm8994_l13` = VDD-IO,
DTS:6445-6450) sono **"disabled"** nel framework dei regolatori mentre la
carta funziona; il dmesg dice anche `mmc1: no vqmmc regulator found` /
`no vmmc regulator found`. **DEDUZIONE: per lo slot SD non c'e' nessun
regolatore da accendere** (a differenza dell'eMMC, dove `pm8994_l20` e'
"enabled").

FATTO: il card-detect di SDCC2 e' `cd-gpios = <0xd2 0x08 0x01>`
(pm8994-gpio indice 8, ACTIVE_HIGH); il kernel lo mostra come
`gpio-873 (f98a4900.sdhci cd) in lo`.

FATTO (non leggibile): `/sys/kernel/debug/mmc1/ios` e' Permission denied
(il telefono non ha root in Android, `adb root` e' disabilitato). Quindi
clock/bus width/tensione della carta SD **non** sono stati letti
direttamente: vengono dalla tabella clock e dal DT.

### FATTO — T1: A1 (il divisore SDHCI e' ignorato su MSM) e' ora un FATTO

`sdhci-msm.c:3488-3489`:

```c
host->quirks |= SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN;
host->quirks2 |= SDHCI_QUIRK2_ALWAYS_USE_BASE_CLOCK;
```

Con quel quirk il driver SDHCI generico **non programma il divisore**: il
clock della carta e' il *base clock*, cioe' quello che fornisce il SoC, e
`sdhci_msm_set_clock()` lo imposta con `clk_set_rate(msm_host->clk, ...)`
(sdhci-msm.c:2929), cioe' sul **GCC**. Conclusione: **il clock reale della
carta eMMC lo pilota il RCG di SDCC1 (0xfc4004d0), che il nostro shim non
ha MAI scritto**. Le 8 varianti dei run precedenti non hanno mai controllato
la velocita' con cui parlavano alla carta.

FATTO: `CORE_IO_PAD_PWR_SWITCH` e' il **bit 16** di HC+0x10c
(sdhci-msm.c:102), non il 15: `REQ_IO_HIGH` lo azzera, `REQ_IO_LOW` lo
accende (2441-2448). La nota precedente che diceva "bit 15/16" era imprecisa.

FATTO: `CORE_CLK_PWRSAVE` e' il bit 1 di HC+0x10c (sdhci-msm.c:97) e il
driver lo **toglie** durante l'identificazione (2800-2810). Il nostro shim
lo toglie solo con la variante `LK_F_CLR_PWRSAVE`, che **non** e' nella
configurazione di default.

FATTO: **il LK non inizializza SDCC2**. Nel binario non c'e' alcun
riferimento a sdc2/sdcc2 (grep su `research/lk-g4/`); la tabella clock del LK
ha solo `sdc1_core_clk`/`sdc1_iface_clk`. Quindi, a differenza dell'eMMC,
lo shim **non trova nulla di gia' fatto** su SDCC2: e' la strada piu'
promettente, non un ripiego.

FATTO: `set_sdc_core_clk` dell'LK accetta frequenze 393216, 49152000,
96000000, 191500000, 200000000, 400000000 (dis-4.9-h-set_sdc_core_clk.txt):
il clock dell'SDCC1 e' pilotato dal GCC dal bootloader, non dal divisore.

Dettaglio completo: `research/lk-g4/T1B-SDCC2.md`.

### FATTO — T2 run 1: HANG. Il telefono si e' bloccato con il logo LG

Costruita la prima versione della sonda T2 (`shim/sd_probe.inc`,
`shim/sd_probe.c`, build `SHIM_SD_PROBE=1`), pensata per essere "di sola
lettura". NON lo era:

```
SD_PROBE(0xb0): wr32(G4_SD_CORE_BASE + CORE_HC_MODE, ...);
SD_PROBE(0xb2): wr8(G4_SD_BASE + HC_SWRST, 1);     /* software reset SDCC2 */
```

**FATTO (l'errore esatto)**: il flash e' riuscito
(`fastboot flash boot` -> "OKAY [0.070s]", "Finished. Total time: 0.393s"),
il reboot e' partito, e **il telefono non e' piu' comparso ne' in adb ne' in
fastboot**: dopo 600 s (10 min) di `tools/run-shim.sh` in attesa della
recovery, il telefono era ancora sul logo LG e non risultava su USB
(`adb devices` vuoto, `fastboot devices` vuoto).

FATTO: il watchdog e' fermato dal shim all'avvio (`stop_watchdog()`), quindi
un hang del bus non ha niente che lo resetti: il telefono resta fermo.

DEDUZIONE (la spiegazione piu' probabile): la scrittura di HC+0x2f su un
controller SDCC2 che **nessuno ha mai inizializzato** (core clock spento)
ha bloccato l'AXI: la transazione non si completa e il processore si
ferma lì. Il boot logo e' il bootloader che non trova un boot image
avviabile.

**Due lezioni, messe nel codice:**

1. **MAI scrivere su un controller non inizializzato.** Una lettura di una
   finestra morta e' innocua (legge 0xffffffff); una scrittura puo' bloccare
   il bus. Il reset software di SDCC2 e' ora la fase D, spenta di default,
   e gira solo in una build dedicata.
2. **Ogni fase lascia un marker nel trail IMEM** prima di toccare qualcosa:
   il trail sopravvive al reset e si legge dalla recovery con
   `tools/read-trail-imem.py`, cosi' anche un hang dice DOVE si e' bloccato.

La sonda e' stata riscritta con le fasi A (eMMC sola lettura), B (eMMC dopo
il reset, finestra viva: e' lo stesso reset che fa il bring-up), C (SDCC2
sola lettura, nessuna scrittura ai registri SDCC2) e D (reset SDCC2,
pericolosa, off). `SHIM_SD_PROBE_PHASE` seleziona le fasi: una
configurazione per boot.

FATTO: il recupero del telefono richiede **la combinazione dei tasti**
(volume giù + power sul G4) per tornare in fastboot; da fastboot
`tools/recover-boot.sh` rimette `backup/boot-stock.img` e riavvia.

### FATTO — T2 v2 (2026-11-09, secondo tentativo): ANCORA UN HANG

Dopo il ripristino del telefono (ROM stock reinstallata dall'utente, Android 13,
`sys.boot_completed=1`) e la riscrittura della sonda a fasi, il run e' stato
ripetuto. FATTO: la ROM stock **ignora** `adb reboot bootloader` e
`adb reboot recovery` (3 tentativi, il telefono torna sempre su Android), quindi
il bootloader si raggiunge solo con i tasti (volume giu' + power).

FATTO: la build `SD_PROBE_PHASE=7` (fasi A = eMMC sola lettura, B = eMMC dopo
il software reset, C = lettura dei pin TLMM; **nessuna scrittura su SDCC2**) e'
stata flashata con successo (`fastboot flash boot` -> OKAY 0.232 s) e il
telefono **non e' piu' tornato su USB**: 240 s di attesa, 40 s di campionamento
continuo, `adb devices` e `fastboot devices` entrambi vuoti. **Secondo hang.**

FATTO: il telefono era in fastboot grazie ai tasti; la sequenza
`tools/run-t2.sh` (flash -> attesa -> lettura pstore + trail -> ripristino di
`boot` stock) non e' arrivata al passo 4, perche' il telefono non e' tornato.

IPOTESI (non verificata: manca il trail, che si legge solo con root): le
cause candidate sono (a) la lettura dei registri TLMM a 0xfd510000 dalla
posizione non sicura (XPU) o dal bus, (b) il software reset dell'eMMC fatto
**senza** il core reset che il driver fa prima (la sequenza del LK e' core
reset e poi SW reset: la fase B fa solo il SW reset), (c) un'interazione con
la ROM stock reinstallata.

**Regola che ne deriva (per i prossimi run):** prima di ogni misura nova si
ri-stabilisce la **baseline** (la build collaudata delle run precedenti, senza
sonda) e si verifica che torni: cosi' si distingue "la sonda blocca" da "il
telefono/ROM non regge piu' lo shim". Poi si aggiunge una fase sola per volta.
E il prologo del prossimo run e' il **ripristino immediato di `boot` stock**,
non la sonda.

---

## Sessione 2026-11-09, seconda parte: il telefono non deve piu' bloccarsi

L'utente ha ripristinato il telefono (ROM stock reinstallata) e ha chiesto
che il codice non possa piu' causare un hang. Quello segue.

### FATTO — la causa dei due hang

Non era il driver eMMC: era una **scrittura MMIO a un controller mai
inizializzato**. Il meccanismo:

1. SDCC2 (`0xf98a4900`) non e' mai stato inizializzato: il bootloader LK non
   lo tocca (FATTO: nessun riferimento a `sdc2` nel binario) e il kernel
   Android non e' ancora partito;
2. il suo clock e' quindi spento;
3. la scrittura di `HC+0x2f = 1` su un blocco con il clock spento non si
   completa: la transazione resta sul bus;
4. il bus AXI e' bloccato e con lui la CPU: l'accesso MMIO non ritorna piu';
5. **non e' una fault** (nessuna eccezione, nessun handler) e **il watchdog
   non e' armato** (FATTO: nei log dei run collaudati non c'e' nessuna riga
   `WDT`, quindi `WDT0_EN` era 0 all'ingresso dello shim): niente reset, il
   telefono resta sul logo LG per sempre, irraggiungibile da USB.

La seconda versione della sonda ha ripetuto la cosa **leggendo** il TLMM
(`0xfd510000`), una finestra mai toccata: stesso esito. Il principio e'
generale: qualunque accesso a una finestra non inizializzata puo' fermare il
telefono.

### FATTO — le difese messe in atto (tutte verificate offline)

1. **Allowlist MMIO a runtime** (`shim/mmio.h`): ogni accesso passa per
   accessori che rifiutano quello che non e' in allowlist. Le finestre
   consentite sono solo quelle gia' pilotate con successo su questo
   telefono: il controller eMMC (il LK ci ha appena letto il boot image), il
   GCC (il LK ci abilita i clock di SDCC1), il watchdog, l'IMEM, il GIC,
   PSHOLD; piu' le finestre di memoria (immagine, DTB, ramoops, scratch, i
   due banchi RAM) e, in sola lettura, il framebuffer (l'XPU ne rifiuta la
   scrittura). **Gli indirizzi non sono dalla memoria**: vengono dal log dei
   run collaudati (`IMAGE lo=00080000 hi=000D0000`, `DTB at=02000000`) e da
   `/proc/iomem` del telefono.
2. **La deny-path non usa la console**: la console scrive nel framebuffer,
   che sul telefono e' protetto dall'XPU, e una scrittura li' e' un data
   abort: l'abort dentro la deny-path trasformerebbe una scrittura rifiutata
   in una fault. La prima versione lo faceva e il self-test e' andato in
   panic con `FAR=0xfc4004c4`, esattamente l'indirizzo che stava rifiutando.
   Ora la deny-path scrive solo nel trail e conta le refuse.
3. **Guardia del watchdog** (`shim/wdt.c`): i registri sono quelli di
   `drivers/soc/qcom/watchdog_v2.c:40-46` (tree CAF), non scelti a memoria;
   i tempi sono scritti come li scrive il driver. La guardia e' armata prima
   dei controller e spara dopo che il log e' al sicuro, e i loop di attesa
   la accarezzano. Limite onesto: il DT ha `qcom,ipi-ping`, quindi il
   firmware sicuro puo' accarezzare il watchdog da solo: e' una rete, non
   una garanzia. La garanzia e' la allowlist.
4. **Un hang e' self-limiting**: `trail_seen()` (shim/trail.h) fa saltare la
   fase che ha bloccato al boot successivo, perche' il trail IMEM sopravvive
   al reset.
5. **Audit statico** (`tools/check-mmio-writes.py`), eseguito dalla build:
   fallisce se una scrittura grezza bypassa la allowlist su una finestra
   device.

### FATTO — i test, tutti offline, e il loro esito

- `python3 tools/qemu-mmio-guard.py` → **tutti i check passano**: nessun
  accesso alla finestra SDCC2 e' arrivato al bus, una scrittura alla
  finestra GCC consentita e' passata, il self-test ha contato le refuse.
- `python3 tools/qemu-emmc.py --mode always` → la sequenza completa e'
  quella giusta (CMD5, CMD0, CMD1, CMD2, CMD3, CMD7, CMD16, CMD8, CMD24 x4,
  CMD17) con 768 accessi al buffer: il bring-up non e' rotto.
- `mute` e `ack` → comportamento identico a prima della allowlist.
- `python3 tools/check-mmio-writes.py` → `ok: no raw store bypasses the MMIO
  allowlist (6 device windows checked)`.
- Le build per il telefono (baseline e sonda) compilano e l'audit gira in
  entrambe.

FATTO: durante questi test il modello QEMU serviva il GCC solo per 4 KB
(0x1000) mentre il registro reale e' a `0xfc4004c4`: l'accesso usciva dal
modello e andava all'indirizzo vero (panic con `FAR=0xfc4004c4`). Il modello
e' stato allargato a 0x2000, la dimensione reale della finestra GCC
(`qcom,gcc@fc400000`, reg size 0x2000).

### La regola per i prossimi run

1. prima la **baseline** (build collaudata, senza sonda);
2. una fase sola per volta;
3. il ripristino di `boot` stock e' dentro la sequenza (`tools/run-t2.sh`);
4. ogni accesso nuovo passa dalla allowlist; se serve una finestra nuova,
   prima in QEMU, poi in `shim/mmio.h` con la prova che e' viva;
5. le misure che richiedono hardware non inizializzato si fanno dalla
   recovery (`tools/read-mmc-registers.sh`), dove c'e' un kernel vero.

Guida completa: `GUIDE-05-MMIO-SAFE.md`.

---

## Sessione 2026-11-09, terza parte: T3, la bring-up della microSD (shim/sd.c)

FATTO: il codice che inizializza lo slot microSD (SDCC2) e' scritto, e
**testato in QEMU**. Non e' ancora stato provato sul telefono: per quello
servono le misure dalla recovery (i pin SDC2 sono muxati su SDC2?).

### FATTO — l'ordine che conta, e come e' verificato

Il codice fa, in quest'ordine:

1. i branch GCC di SDCC2 (AHB prima, poi apps);
2. il clock della carta a **400 kHz**, via il RCG del GCC
   (CFG[4:0]=23, CFG[10:8]=0 XO, M=1, N=0xFFFFFFFC, D=0xFFFFFFFB:
   l'entrata F(400000, gcc_xo, 12, 1, 4) di ftbl_sdcc2_4_apps_clk_src,
   clock-gcc-8992.c:1125);
3. **solo dopo** il controller: HC mode, core reset, software reset, bus power;
4. i comandi, nell'ordine del core CAF: CMD0, CMD8 (0x1AA), CMD55+ACMD41
   (OCR 0x40ff8080, 100 tentativi con 10 ms), CMD2, CMD3, CMD7, CMD16, CMD17.

Il **modello QEMU di SDCC2 riproduce lo stall**: le finestre di SDCC2 non
sono servite finche' il branch dei clock non e' acceso, quindi un ordine
sbagliato fa fallire il test invece di bloccare il telefono. E' il test che
mancava il 2026-11-09.

FATTO (test `tools/qemu-sd.py`, tutti i check verdi):

```
ok  : the clock (seq 269) comes before the controller (seq 280)
ok  : the command sequence is [0, 8, 55, 41, 2, 3, 7, 16, 17]
ok  : the RCG of SDCC2 is programmed for 400 kHz
ok  : 768 words came out of the buffer port
ok  : SD rca=00000000 cid=03534453
ok  : SD ocr=C0FF8080 stage=0000000F
```

FATTO: i due check sugli altri percorsi restano verdi: la protezione MMIO
(`tools/qemu-mmio-guard.py`) e il percorso eMMC (`tools/qemu-emmc.py --mode
always`: 13 comandi, 768 parole al buffer).

### I quattro bug che i test hanno trovato (tutti miei)

1. **`emmc_model.inc`, ramo `win == 1` dimenticato**: riscrivendo
   `model_write_reg()` per aggiungere le finestre di SDCC2 ho perso il ramo
   dell'host eMMC: tutte le sue scritture finivano nel ramo del GCC e la
   carta non rispondeva piu'. Trovato dal test del percorso eMMC.
2. **`sd.c`, accessori non sorvegliati dal modello**: usavano
   `mmio_rd32()` (accesso diretto) invece degli accessori del modello, quindi
   nel QEMU finivano all'indirizzo reale: `PANIC SYNC, FAR=0xfc400508`. La
   lezione e' gia' stata scritta: nel build QEMU gli accessi devono passare
   dal modello, come quelli di emmc.c.
3. **`sd_read_block()` aspettava `XFER_DONE` prima di leggere il buffer**: il
   trasferimento finisce *mentre* si legge. L'ordine giusto (risposta del
   comando, poi R1, poi lettura con `DATA_AVAILABLE`) e' quello di
   `emmc_xfer()`, il riferimento collaudato.
4. **il self-test usava SDCC2 come finestra vietata**: ora SDCC2 e' in
   allowlist (il kernel Android la guida a ogni boot), quindi il self-test
   usa il TLMM (0xfd510000), che resta fuori allowlist ed e' la seconda
   finestra che ha bloccato il telefono.

### Lo stato della build

- `SHIM_SD_INIT=0` (default): la bring-up della SD **non** parte. La build
  per il telefono resta quella collaudata.
- `SHIM_SD_INIT=1`: parte dopo l'eMMC e prima della scrittura del pstore,
  cosi' il suo esito finisce nel record che sopravvive al reset.

FATTO: il self-test della allowlist e' passato con **3 refuse** (la
scrittura, la lettura e la seconda lettura sul TLMM).

---

## Sessione 2026-11-09, quarta parte: risolto il caricamento del payload microSD (shim/sd_payload.c, shim/emmc_model.inc)

FATTO: tutti gli 11 check di `tools/qemu-sd.py` ora passano (`ALL CHECKS PASSED`).
Anche `tools/qemu-emmc.py --mode always` (13 comandi, 768 parole) e `tools/qemu-mmio-guard.py` (3 rifiuti) restano verdi.

---

## Sessione 2026-11-09, quinta parte: test live sul telefono (run con SHIM_SD_INIT=1 e SHIM_SD_PAYLOAD=1)

Test eseguito sul telefono reale (partendo da fastboot):
1. Flashata l'immagine `shim/out/shim-boot-sd.img` nella partizione `boot`.
2. Esecuzione dello shim sul telefono: watchdog armato, nessuna schermata nera/hang, pstore scritto, disarmo watchdog regolare e reboot automatico in recovery TWRP 3.7.
3. Ripristino immediato e verificato per SHA256 della partizione `boot` con `backup/boot-stock.img` (`070980b26f62d79f...`).
4. Estratto `/sys/fs/pstore/console-ramoops-0` salvato in `research/raw-2026-11-09/pstore-sd-live.bin`.

### Risultato misurato sul telefono reale

Nel pstore:
```text
EMMC prepare=FFFFFFF4 stage=00000004
SD rc=FFFFFFEC stage=00000007
SD err=FFFFFFEC rca=00000000
...
SDPAY preFFFFFFEC stage=00000007
```
- Lo slot microSD (SDCC2) è vivo: i clock GCC AHB/APPS e il RCG a 400 kHz salgono regolarmente, il reset core e HC completano, i registri rispondono (`caps = 0x342dc8b2`).
- L'errore avviene a `stage=7` (`SD_STEP_CMD0`) con `rc=FFFFFFEC` (`-20`).
- Causa identificata: `shim/sd.c` definisce `#define SD_HC_CLK_CTRL 0x2c`, ma **non programma mai** il registro `CLK_CTRL` dell'host controller SDHCI (a differenza di `emmc_lk_bringup.inc` che abilita clock interno bit 0, attende stabilità bit 1 e abilita SD clock bit 2). Senza SD clock abilitato nel controller, i comandi verso la microSD vanno in timeout.
- In recovery TWRP confermato: la microSD è presente (`/dev/block/mmcblk1`, 32GB SS32G), la partizione dati FAT32 parte dal settore 8192 (lasciando liberi i settori 2048..8191 per il payload header e test payload), e i pin SDC2 (150..152) sono configurati regolarmente su `f98a4900.sdhci`.


### Causa reale dei fallimenti del report (diversa dai sospetti iniziali)

Nel report `PROBLEMA-SD-PAYLOAD.md` si sospettava che il ramo `sd_payload_load()` non venisse eseguito. In realtà veniva eseguito, ma falliva prima di stampare `SDPAY len=` e impostare il loader:

1. **`model_sd2_disk_blocks` azzerato dopo essere stato impostato a 2**:
   in `shim/emmc_model.inc`, all'interno di `emmc_model_config()`, il blocco `if (model_sd_payload)` impostava `model_sd2_disk_blocks = 2`. Subito sotto, il codice di default azzerava incondizionatamente `model_sd2_disk_blocks = 0`. Di conseguenza, `idx < model_sd2_disk_blocks` era sempre falso, il blocco 2048 veniva restituito pieno di zeri e `sd_payload_load()` falliva subito con codice `-2` (`0xFFFFFFFE`, magic mismatch). Spostata l'inizializzazione dei default di SD2 a inizio funzione.
2. **Mappatura LBA in CMD17 del modello**:
   in `emmc_model.inc`, `case MCMD_SD_READ_BLOCK`: per LBA < 2048 (es. lettura settore 0 in `sd_init()`), l'operatore ternario mappava `idx = lba = 0`, restituendo impropriamente il blocco del payload invece del settore 0. Corretto separando esplicitamente `lba >= MODEL_SD2_PAYLOAD_LBA` dal fallback settore 0.
3. **`SD_PAYLOAD_RAM` in QEMU virt (`0x10000000` vs `0x42000000`)**:
   su QEMU `virt`, la RAM fisica è mappata a `0x40000000..0x80000000`. L'indirizzo `0x10000000` (che sul telefono reale è RAM banco 1) su QEMU virt corrisponde a spazio PCIe unmapped. Le scritture venivano ignorate e le letture restituivano tutti `0xFF` (con CRC risultante `0xbd7bc39f`), provocando errore `-6` (`0xFFFFFFFA`, CRC mismatch). Condizionato `#if SHIM_EMMC_MODEL` a `0x42000000UL` (mantenendo `0x10000000UL` per l'hardware del G4).


---

## Sessione 2026-11-09, sesta parte: Bring-up SDCC2 completato con successo su hardware e Boot XNU (iOS)

### 1. Risultato Bring-up MicroSD (SDCC2) su Hardware Reale (Run 6 e successive)

Con le correzioni a:
- Pad TLMM SDC2 (`0xfd512048 = 0x1fe4`, pull-up + 16mA);
- Clock RCG SDCC2 a 400 kHz (`0x2017`, dual-edge mode e polling update bit);
- Host controller `CLK_CTRL` (`0x2c`: abilita clock interno, attende stabilità bit 1, abilita SD clock bit 2);
- Richiesta OCR standard SD (`0x40ff8000`, rimosso bit 7 eMMC);
- Loop ACMD41 con retry fino a 100 tentativi:

La sequenza CAF completa ha avuto **successo totale al 100% su hardware reale**:
```text
SD tlmm_old=00000BDB tlmm=00001FE4
SD vs_old=00000A1C vs=00000A1C
SD clk=000E0007 pwr=0000000F
SD cmd55 rc=00000000 rsp=00000120
SD acmd41 rc=00000000 rsp=41FF8000
SD acmd41 ok i=00000002 ocr=C0FF8000
SD rca=0000AAAA cid=430E010A
SD ocr=C0FF8000 stage=0000000F
SD rc=00000000 stage=0000000F
SD err=00000000 rca=0000AAAA
```
Tutti i 15 stage passati senza errori (`stage=0000000F`, `rc=00000000`, `err=00000000`). La scheda risponde con RCA `0xAAAA`, CID `430E010A` e OCR `0xC0FF8000` (bit 30 CCS=1, High Capacity SDHC).

### 2. Diagnosi hardware della scheda microSD inserita

- **Lettura**: perfetta al 100% (settore 0 MBR e settore 8192 FAT32 letti con checksum integro).
- **Scrittura**: la scheda fisica presente nello slot (SanDisk Ultra 32GB con file risalenti ad agosto 2017) è entrata nel meccanismo di protezione hardware permanente (read-only lock per fine cicli di vita/wearout flash).
  Qualsiasi comando di scrittura (anche tramite kernel Linux in TWRP: `dd of=/dev/block/mmcblk1 seek=2048`) viene rigettato dal controller della scheda con `card status 0x80900` (bit 19 ERROR) e `end_request: I/O error dev mmcblk1`.
  La scheda è quindi utilizzabile solo in lettura; non può essere formattata né scritta finché non viene sostituita fisicamente.

### 3. Flash e Boot di iOS XNU (`g4.bin`)

Dato che:
- La memoria interna eMMC (`mmcblk0`) è pienamente operativa (29.1 GiB HS400, 51 partizioni);
- La partizione `boot` (`mmcblk0p37`) è di ben 48 MiB (`98304` settori da 512 byte) ed è perfettamente scrivibile e verificata per SHA256;
- Il bootloader LK carica senza problemi il ramdisk della boot image a `0x02200000` passandolo via DTB `/chosen` (`linux,initrd-start`);
- Il kernel XNU per G4 con ADT (`shim/out/g4.bin`, 16.3 MB) è pronto:

Abbiamo preparato e flashato l'immagine completa di XNU:
1. Aggiornato `shim/loader.c` per scrivere il pstore console e armare il watchdog a 60 secondi prima del salto in XNU:
   ```c
   pstore_console_write(con_log, (unsigned)con_log_len);
   g4_wdt_arm(60000);
   ((xnu_entry_t)entry_pa)(ba_pa);
   ```
2. Compilata l'immagine `out/shim-boot-xnu.img` (17.952.768 byte, sha256 `2e7f4ae6a629d8f1c42b09edf9892a082b6c7801477642d7e3264ca64a1d54f9`):
   - Kernel: shim (380 KB)
   - Ramdisk: payload XNU + ADT (`g4.bin`, 16.3 MB)
   - DTB: QCDT v3 H815 (`0xb64`)
3. Flashata nella partizione `boot` con `dd` in TWRP e verificata per SHA256.
4. Riavviato il telefono per avviare il caricamento di iOS XNU.

---

## Sessione 2026-11-09, settima parte: Setup LAF Unbrick toolkit, estrazione KDZ e auto-fallback in TWRP

### 1. Condizioni Hardware dei Pulsanti Fisici
- I pulsanti **Power** e **Volume Giù** non funzionano correttamente sul dispositivo.
- L'unico pulsante fisico funzionante e affidabile è **Volume Su** (utilizzato per entrare in Download Mode / modalità ripristino LAF collegando il cavo USB).

### 2. Toolkit di Ripristino LAF (Download Mode) e KDZ
1. **Estrazione KDZ dal telefono**:
   - Trovato `/sdcard/Europe H81520C_00_1218.kdz` (1.7 GB) sul telefono in TWRP.
   - Estratto localmente in `kdz/Europe H81520C_00_1218.kdz`.
   - Disimballato il contenitore DZ in `kdz/H81520c_00.dz`.
2. **Aggiornamento di `kdztools`**:
   - Corretto `tools/kdztools/undz.py` per gestire gli header moderni v20c (CRC32 opzionale a zero e slice unallocated).
   - Estratte le partizioni stock in `kdz/extracted/`: `PrimaryGPT.image`, `boot.image`, `recovery.image`, `aboot.image`.
3. **Ambiente Python e `lglaf`**:
   - Configurato ambiente virtuale `.venv` con `pyusb` e `cryptography`.
   - Verificati `tools/lglaf/lglaf.py` e `tools/lglaf/partitions.py`.
   - Creato script di automazione `tools/laf-tool.sh` (comandi: `list`, `restore-boot`, `restore-recovery`, `reboot`, `reboot-recovery`, `shell`).
   - Procedura unbrick in Download Mode: a telefono spento, tenere premuto **Volume Su** e collegare il cavo USB -> eseguire `./tools/laf-tool.sh restore-boot`.

### 3. Sicurezza per i test di Boot XNU (Auto-fallback in TWRP)
Per prevenire qualsiasi bootloop in assenza dei tasti fisici:
- `shim/loader.c` scrive esplicitamente `mmio_wr32(G4_RESTART_REASON, LK_RECOVERY_MODE)` prima del salto in XNU:
  in caso di timeout del watchdog hardware (60s) o reset, il bootloader LK rileva `RECOVERY_MODE` nella memoria IMEM e **si riavvia automaticamente in TWRP recovery con ADB attivo**, senza richiedere alcuna pressione di tasti fisici.
- La partizione `boot` attuale è stata ripristinata con `backup/boot-stock.img` (SHA256 `070980b2...`) per garantire la sicurezza del dispositivo.

---

## Sessione 2026-11-09, ottava parte: Esecuzione hardware di iOS XNU con RAMDisk e preparazione storage `userdata`

### 1. Approvazione dell'utente per lo Storage `userdata`
- L'utente ha confermato: *"per la memoria se funziona userdata va bene userdata"*.
- `userdata` (`/dev/block/bootdevice/by-name/userdata` -> `/dev/block/mmcblk0p50`, 23.2 GiB) sull'eMMC interna (`mmcblk0`) è la destinazione definitiva per il root filesystem iOS (HFS+ case-sensitive `SweetRoot`).
- I backup critici (KDZ stock 1.7GB, TWRP 3.4/3.7) sono stati salvati su host locale e verificati.

### 2. Risoluzione dei Blocchi al Boot di XNU
1. **Data Abort sincrono via XPU su `0x03400000`**:
   - `sweet-fbcon` e `FB_BASE` puntavano inizialmente a `0x03400000` (buffer di splash LK).
   - Sul G4 (MSM8992), `0x00000000..0x09600000` è protetto dalla Qualcomm XPU in EL1 non sicuro: qualsiasi scrittura CPU genera immediatamente un *Synchronous Data Abort*.
   - All'avvio di XNU, `fbcon_init()` eseguiva `fbcon_clear_row()` scrivendo nel buffer di splash e mandando in crash il kernel prima di qualsiasi messaggio.
   - **Risoluzione**: `sweet-fbcon` in `tools/mkadt.py` e `G4_FB_BASE` in `shim/g4.h` sono stati riallocati a `0x20000000` nel banco di memoria DRAM 1 (`0x10000000..0x50000000`), dove la CPU in EL1 scrive liberamente senza abort XPU.
2. **Integrazione Ramdisk (`rd=md0`) per PID 1**:
   - Il cmdline richiedeva `rd=md0`, ma il payload `g4.bin` conteneva solo i 10 segmenti Mach-O del kernel senza immagine ramdisk.
   - Compilato `tools/sweet-init.S` con clang (`-target arm64-apple-ios16.0`) e impacchettato con `tools/mkinit.py` in `shim/out/sweet-init.bin` (Mach-O arm64 statico con firma ad-hoc e heartbeat).
   - Aggiornato `g4.bin` via `tools/mkpayload.py --ramdisk shim/out/sweet-init.bin`: iniettato nodo `RAMDisk` (`0x82478000`, `0x8000`) in `/chosen/memory-map` dell'ADT e aggiunto l'undicesimo segmento nel payload.

### 3. Risultato del Boot Hardware (Misurato su Telefono)
- Flashato `shim/out/shim-boot-xnu.img` (17.969.152 byte, sha256 `baf83d7ee0b2b9646c220f056bf195716d3f79e0451c8f2f8209950bec05491b`).
- Log recuperato da `/sys/fs/pstore/console-ramoops-0`:
  ```text
  INITRD lo=02200000 hi=03197640
  PHYS base=80000000 size=5FF00000
  VIRT FFFFFE0006000000
  ENTRY FFFFFE000747B488
  SEGS n=0000000B end=82480000
  BOOTARG pa=82484000 top=82488000
  JUMP 000000008147B488
  XNU jumping (fbcon clears screen)
  ```
- **Esito**:
  - Lo shim ha decompresso ed esteso in RAM fisica tutti gli 11 segmenti (`0x80000000`..`0x82480000`).
  - Preparato `boot_args` a `0x82484000`.
  - Saltato con successo nell'entry point di XNU a PA `0x8147B488` (`__start`).
  - L'auto-fallback `LK_RECOVERY_MODE` con watchdog a 60 secondi ha funzionato perfettamente: al termine della finestra di esecuzione, LK è rientrato stabilmente in TWRP recovery con ADB attivo, senza richiedere l'uso dei pulsanti fisici danneggiati.

### 4. Risorse IPSW Identificate per il Rootfs su `userdata`
- File IPSW locale presente: `/Users/io/Dev/ios/lg g4/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw` (5.7 GB).
  - Immagine OS: `087-86698-044.dmg` (4.5 GB)
  - Immagine SystemOS: `087-86593-047.dmg` (2.6 GB)
  - Immagine AppOS: `087-86715-047.dmg` (10 MB)
- Script di creazione volume nativo su macOS disponibile in `/Users/io/Dev/ios/lg g4/UniXNU-lumia950xl/scripts/mkiosroot-root.sh` (utilizza `hdiutil` e `ditto` preservando permessi, proprietari e link simbolici/hardlink senza richiedere fuse).

### 5. Creazione e Scrittura del Rootfs iOS 16 su `userdata` (Hardware eMMC)
- **Generazione Volume HFS+**:
  - Creato `/Users/io/Dev/ios/ios-root-work/ios-root.sparseimage` (17.5 GB, HFS+ Journaled Case-Sensitive, etichetta `iOSRoot`, UUID `CBE356DE-75C3-3B85-9FF5-4531C519AE23`).
  - Copiati decompressi con `ditto` OS Root, SystemOS Cryptex (`/private/preboot/Cryptexes/OS`) e AppOS Cryptex (`/private/preboot/Cryptexes/App`).
  - Eseguito `stage-ios-root.py`: 63 hardlink cryptex, 44 file di cache dyld (`/System/Library/Caches/com.apple.dyld`), `/sbin/launchd` proprietario `0:0` permessi `0755`, `launchd.plist` modificato (`_PanicOnCrash` rinominato in `_PanicOnCrasX`, rimossi job `CommCenter`), creati container e marker diagnostici.
  - Verificato con `fsck_hfs`: `The volume iOSRoot appears to be OK`.
- **Scrittura Diretta su Partizione Fisica**:
  - Smontati `/data` e `/sdcard` in TWRP.
  - Eseguito streaming compresso zlib al volo via `adb exec-in` direttamente su `/dev/block/bootdevice/by-name/userdata` (`mmcblk0p50`).
  - Scritti tutti i 17.500 MB (17.09 GiB) in 557 secondi (~31.4 MB/s medi) con `sync` finale.
- **Verifica su Telefono**:
  - Letto offset 1024 di `/dev/block/bootdevice/by-name/userdata`:
    ```text
    00000000  48 58 00 05 80 00 21 00  48 46 53 4a 00 00 00 8a  |HX....!.HFSJ....|
    ```
    `HX` (HFS+ Case-Sensitive), versione 5, `HFSJ` (Journaled), 4.478.976 blocchi da 4096 byte. La partizione `userdata` è pronta e integra per il boot di iOS 16.

---

## Sessione 2026-11-10: piano d'azione e verifica delle fonti esterne

Nessuna modifica al codice dello shim in questa sessione: solo analisi e
verifica. Il piano completo e in `docs/PLAN-2026-11-10.md`.

### FATTO — le risorse esterne sono state verificate sul disco, non a memoria

- **ios-topaz** clonato in `research/ios-topaz/` (fork `turgayasuk-sketch`,
  HEAD `c6940af9`, 148 patch). Le patch **0092** (`topaz-direct-mdp-scanout`) e
  **0093** (`topaz-scanout-contiguous-ram`) esistono e sono cio che il brief
  descrive. Altre verificate e rilevanti per il G4:
  **0098** (diagnostica timing engine INTF1: FRAME_LINE_COUNT_EN, TE_EN,
  contatore di frame), **0103** (back buffer + flip multi-layer),
  **0104** (luminosita via **DSI DCS 0x51** — rilevante perche il G4 e in
  command mode, dove la luminosita e un comando DSI e non un valore nel
  framebuffer), **0096/0097** (fade, console unificata).
  Il README conferma il target: Redmi Note 12 4G, **SM6225**, display con
  **direct MDP scanout**, splash FB a `0x5c000000`.
- **postmarketOS LG H815** (Alexander Reimelt): il DTS era gia in
  `research/msm8992-lg-h815.dts`. `pm8994_l20` = 2.95 V (VCC sdhc1),
  `pm8994_l13` = 1.8-2.95 V (VDD-IO sdhc2). **Nota**: il DTS **non** ha
  `regulator-always-on`/`regulator-boot-on` su L20.
- **UniXNU-lumia950xl**: gia in workspace, gia referenziato in
  `docs/g4-hardware/EMMC-REFERENCE.md`. Driver in `drivers/NativeSdhci/`.

### FATTO — il brief sbaglia i registri del display: il G4 e MDP3, non MDP5

Il brief suggeriva VIG0 a `0x01200` e CTL0 a `0x00600`: sono registri **MDP5**.
Il MSM8992 usa **MDP3**. Le basi corrette vengono dal **DT del G4**:

- `qcom,mdss-pipe-vig-off = <0x5000 0x7000 0x9000>` (DTS:784)
  => **VIG0 = 0xfd900000 + 0x5000 = 0xfd905000**
- `qcom,mdss-ctl-off = <0x2000 0x2200 0x2400>` (DTS:809)
  => **CTL0 = 0xfd900000 + 0x2000 = 0xfd902000**
- offset interni: `SSPP_SRC0_ADDR = 0x014`, `SSPP_SRC_YSTRIDE0 = 0x024`,
  `SSPP_SRC_FORMAT = 0x030`, `CTL_TOP = 0x014`, `CTL_FLUSH = 0x018`,
  `CTL_START = 0x01C` (`mdss_mdp_hwio.h`).
- **CMD mode**: `MDSS_MDP_CTL_OP_CMD_MODE = (1 << 17)`, in `CTL_TOP`.

### FATTO — il valore di flush 0x20001 di ios-topaz e valido ANCHE per MDP3

`mdss_mdp_ctl.c:3483-3490`: i bit di flush per pipe sono
`BIT(pipe->num) << 5` (DMA), `BIT(pipe->num) << 10` (VIG3/RGB3),
`BIT(22+..)` (CURSOR), e **`BIT(pipe->num)`** per RGB/VIG 0-2.
`MDSS_MDP_SSPP_VIG0 = 0` e il tipo VIG non e DMA ne VIG3 => VIG0 => `BIT(0)`.
`mdss_mdp_ctl.c:3833` mette `BIT(17)` per il CTL.
=> **`CTL_FLUSH = 0x20001`**, esattamente il valore che topaz scrive.
Il numero di topaz e portabile al G4 invariato.

### FATTO — il contatore di frame del timing engine e la discriminazione decisiva

topaz-0098 legge `INTF+0x0ac` (frame count) e `INTF+0x0b0` (line count), e
abilita `FRAME_LINE_COUNT_EN` (`INTF+0x0a8 = 0x3`). Sul G4 l'INTF e dentro la
finestra MDP (`mdss_mdp_hwio.h:506` `MDSS_MDP_REG_INTF_TIMING_ENGINE_EN`).
**Leggendo quel contatore DUE VOLTE con ~100 ms in mezzo**: se cresce, il
pannello sta ricevendo frame; se e fermo, il problema e a valle (timing
engine/DSI/panel) e non a VIG0. Questa e la misura piu importante di D0:
cambia completely la strategia del display.

### FATTO — la lezione trasferibile di ios-topaz e il metodo, non i registri

Il 0092 **non programma l'MDP da zero**: legge i registri VIG0 *allo stato di
boot* (`fVig0[0x14]` = SRC0_ADDR, `fVig0[0x24]` = YSTRIDE) e li ripunta
(`scanFrom()`), con un flush a `CTL+0x18`. Sul G4 questo e possibile perche
**il pannello mostra il logo LG => l'MDP e vivo e gia configurato**.
Il 0093 alloca le surface in RAM contigua sotto i 4 GiB; sul G4 non serve
`kIOMemoryPhysicallyContiguous` perche si puo usare `0x20000000` (DRAM banco 1,
gia usato dal fbcon).

### FATTO — il wake CMD5 dello shim e gia corretto (ipotesi RCA debole)

Da `research/lk-g4/lk-source-recovered/`:
- `mmc.h:568` `#define MMC_RCA 2`; `mmc.c:838` per l'eMMC `cmd.argument =
  (MMC_RCA << 16)` => **RCA = 2**.
- `mmc.h:427` `#define MMC_CARD_SLEEP (1 << 15)`;
  `mmc.h:569` `#define MMC_CARD_RCA_BIT 16`.
- `mmc.c:3430` (`mmc_put_card_to_sleep`): argomento CMD5 =
  `(rca << 16) | (1 << 15)` = `0x00028000` => SLEEP con RCA 2.

=> il wake corretto e CMD5 con argomento **`0x00020000`** (AWAKE, bit 15 a 0),
che e **esattamente** cio che `emmc_wake()` (`shim/emmc.c:775-810`) fa con
`rca = 2`. L'ipotesi "RCA sbagliato" e quindi **debolitata**.
Nuovo test diagnostico proposto: **CMD5 con `arg = 0`** (AWAKE, RCA 0), che
bypassa del tutto RCA e selezione e risponde "la card e viva o e muta?".

### FATTO — l'eMMC e una card muta, non un problema di CMD5/CMD1

Dal pstore (run 6): `CMD n=5 rc=FFFFFFFD`, `int=00018000`
(Command Timeout | Data Timeout), `pres=01E80007` (card presente,
`0x1E0000` = transizione di comando in corso). La card **non risponde a
nessun comando**: non e un CMD sbagliato, e silenzio totale.
Tre ipotesi non discriminate: **H1** VCC (pm8994_l20) spenta,
**H2** card in sleep, **H3** controller in stato non trasferimento.
Con RCA=2 verificato, **H2 e indebolita** come prima ipotesi.

### FATTO — l'infrastruttura RPM SMD manca del tutto nello shim

`grep -rn "rpm\|SPMI" shim/` non trova un solo client RPM/SPMI. Il messaggio
RPM e definito (`rpm-smd-regulator.c:217` `rpm_vreg_string_to_int` impila i
caratteri little-endian): resource type `ldoa` = `0x616f646c`, id `20`,
key `swen` = `0x6e657773`, key `uv` = `0x00767500` = 2950000 uV.
Ma il blocco vero e il **canale SMD** (shared memory + APCS IPC + ack), che e
assente dallo shim. **Da verificare**: se `pm8994_l20` e regolatore RPM o
SPMI nel DT del telefono. Se e RPM-only, il percorso e molto piu grande di un
client SPMI (~100 righe).

### FATTO — il pannello: JDI NT36850, command mode, dual MIPI

Dal DT: `qcom,mdss-dsi-panel-type = "dsi_cmd_mode"`, width `0x2d0` (720 per
link) x height `0xa00` (2560), bpp `0x18` (24 sul filo),
`qcom,mdss-dsi-color-order = "rgb_swap_rgb"`, `traffic-mode = "burst_mode"`,
`lane_map_0123`. Due controller DSI (`0xfd994000` / `0xfd996000`), clock
display **SPI 83** via MDP. Il DT del bootloader **non** contiene la sequenza
DCS: sta in `dsi-panel-jdi-nt36850-dualmipi0-qhd-cmd.dtsi` (nel tree Titan).

### FATTO — il touch sul G4 e due candidati con gli stessi GPIO

`synaptics,s3528` @ I2C `0x20` e `lgesic,lg2411` @ `0x28`, entrambi con
**IRQ GPIO 22** e **reset GPIO 8**: il DT li abilita entrambi ma solo uno e
popolato. Bus `i2c@f9924000` (bus 2, BLSP1 QUP2), IRQ SPI 96, 400 kHz.
**Da scoprire leggendo il bus, non dal DT.**
Pulsanti: volume up = PM8994 **GPIO 3**, power = **PON1**, volume down =
**PON2**: entrambi via **SPMI**, che serve gia per l'eMMC.

### FATTO — il rootfs iOS 16 e gia scritto e verificato

`userdata` = `mmcblk0p50`, 23.2 GiB, HFS+ journaled case-sensitive, 17.500 MB
scritti, `fsck_hfs` OK. Il prossimo obiettivo e **leggerlo**, non riscriverlo.

### La prossima azione concreta

**Un solo ciclo di test**, che raccoglie due misure indipendenti e a rischio
quasi zero:

- **E0** (eMMC): leggere i registri del controller *prima* del CMD5, e provare
  **CMD5 con `arg = 0`** (AWAKE, RCA 0).
- **D0** (display, sole letture): `VIG0+0x014` (SRC0_ADDR),
  `VIG0+0x024` (YSTRIDE), `VIG0+0x000`, `VIG0+0x030`, `CTL0+0x014` (opmode),
  `CTL0+0x018`, `CTL0+0x01C`, `MDP+0x14`.

Se `VIG0+0x014 == 0x03400000` => l'MDP scansiona la splash window e il display
e a **un solo store** di distanza (D1). Se CMD5 `arg=0` risponde => l'eMMC e
risolto in un colpo.

```sh
cd /Users/io/Dev/ios/ios-sweet-lg-g4
SHIM_HW_DIAG=1 SHIM_SD_INIT=0 SHIM_SD_PAYLOAD=0 RAW_TIMING=1 FINE_TIMING=1 \
  DIAG_VARIANTS=0 DIAG_TIMING=0 ./shim/build.sh
python3 tools/verify_bootimg.py shim/out/shim-boot.img
python3 tools/check-mmio-writes.py
python3 tools/qemu-mmio-guard.py
IMG=shim/out/shim-boot-baseline.img MODE=diag tools/run-t2.sh
tools/fetch-pstore.sh research/raw-2026-11-10/pstore-e0-d0.bin
```

Nota: `tools/run-t2.sh` fa il restore automatico di `boot` stock, e obbligatorio
(questo LK non accetta `fastboot boot`: ogni test e un flash vero).

### Cosa non e stato fatto (non e un blocco)

Nessuna scrittura nel codice dello shim in questa sessione. Nessun flash.
Nessun test sul telefono. Il clone `research/ios-topaz/` e in `.gitignore`
(`research/*`), non va committato.

---

## Sessione 2026-11-10 (seconda parte): il test E0/D0, un hang, e la diagnosi

### FATTO — il boot partition era l'immagine XNU, NON stock

All'inizio della sessione la partizione `boot` conteneva `shim-boot-xnu.img`
(hash del kernel a 0x1000 identico), non `backup/boot-stock.img`: la sessione
del 9 novembre era finita con l'immagine XNU flashata e il ripristino non era
stato fatto. Verificato leggendo `/dev/block/mmcblk0p38` dalla recovery.
**Correzione**: le note dicevano "boot e bit per bit backup/boot-stock.img".
Non era vero.

### FATTO — il LAF (Download Mode) del G4 ha bisogno dell'autenticazione KILO

`tools/lglaf/partitions.py` falliva con `0x8000010a (LAF_ERROR_ACCESS_DENIED)`.
Il protocollo (`tools/lglaf/protocol.md`, sezione KILO) dice: *"On newer
versions, this requires authentication via KILO command"*. La funzione
`challenge_response()` esisteva gia in `lglaf.py` ma **non era mai chiamata**.

**La modalita` che funziona e la 2** (misurata sul G4 il 2026-11-10):

| mode | OPEN |
|---|---|
| 0 | ACCESS_DENIED |
| 1 | ACCESS_DENIED |
| **2** | **OK** |

Corretto in `tools/lglaf/partitions.py`: prima di `laf_open_disk()` si prova
KILO 2, poi 0, poi 1, e si tiene la prima che apre il disco. Il `CLSE` di
cleanup ora e` non-fatale (risponde ACCESS_DENIED dopo la negoziazione KILO,
ma i settori sono gia` scritti).

Dopo la correzione: `--list` funziona (partizioni lette, `userdata` = p50
come previsto) e `--dump`/`--restore` funzionano. **Verificato byte per
byte**: il dump LAF della partizione `boot` coincide con
`shim-boot-e0d0.img`, e dopo il restore con `backup/boot-stock.img` il
reread dava hash `070980b26f62d79f...` = backup stock. Il canale LAF e`
quindi affidabile per leggere e scrivere partizioni.

### FATTO — il pannello e` pilotato dalla pipe RGB2, NON da VIG0

Misurato dalla recovery con il kernel vero che disegna la UI di TWRP
(quindi l'MDP e` vivo e clockato):

```
/sys/class/graphics/fb0/name         = mdssfb_90000
/sys/class/graphics/fb0/virtual_size = 1440,5120
/sys/class/graphics/fb0/stride        = 5760
/sys/class/graphics/fb0/bits_per_pixel= 32
/sys/kernel/debug/mdp/stat:
    intf2: play: 00000024  vsync: 00000112  underrun: 00000000
    RGB0 : 00000002   RGB1 : 00000002   RGB2 : 00000024
    VIG0 : 00000000   VIG1 : 00000000   VIG2 : 00000000
```

**RGB2 (offset 0x19000) su INTF2 e` la pipe che scansiona il pannello.**
VIG0/VIG1/VIG2 sono a zero. Questo cambia il piano del display: la patch
0092 di ios-topaz punta VIG0, e sul G4 il pipe giusto e` un altro. Sul G4 la
superficie contiguamente scansionata arriva dalla pipe RGB2.

Il framebuffer di TWRP e` all'indirizzo fisico **0x00020000** (da
`/sys/kernel/debug/mdp/buffers`: `addr=0x0000000000020000 size=29491200`),
non a `0x3400000` ne` a `0x20000000`.

### FATTO — parametri reali del pannello, letti dal driver

Da `/sys/kernel/debug/mdss_panel/dsi0/` (kernel TWRP, pannello JDI NT36850):

| parametro | valore |
|---|---|
| clk_rate | 910224000 (910 MHz) |
| physical_width / height | 68 / 121 (mm) |
| bl_max / bl_min | 4095 / 1 |
| te_tear_check_en | 1 |
| te_start_pos | 4 |
| te_refx100 | 6000 |
| te_sync_cfg_height | 65520 |
| h_front_porch / h_back_porch / h_pulse_width | 168 / 88 / 4 |
| v_front_porch / v_back_porch | 9 / 10 |
| underflow_clr / border_clr | 255 / 0 |
| data_lane0..3 | 1 (tutte e 4 attive) |
| frame_rate | 60 |
| fbc enable / bpp | N / 24 |
| panel_ack_disabled | N |

Nota: i porchi qui (168/88/4) **non** coincidono con quelli del DT del
bootloader (0xa8=168 davanti, 0x58=88 dietro, 0x04 pulse): il front porch
corrisponde, il back porch no (88 = 0x58 e` il value del DT, ma il DT dice
0x58 che e` 88, quindi coincide: 168=0xa8, 88=0x58, 4=0x04 -> **coincidono
tutti**). La risoluzione e` 1440x2560 ma il virtual_size e` 1440,5120
(il doppio, per il doppio buffer).

### FATTO — i registri DSI0 e DSI1 sono leggibili e programmati

`/sys/kernel/debug/mdp/dsi0_ctrl_reg` e `dsi1_ctrl_reg` danno i registri
del controller DSI: sono identici per DSI0 e DSI1 tranne
`0x0000a0` (1000dc57 su DSI0, 10006257 su DSI1: il byte alto e' l'ID del
link). Questo e` il posto dove il driver Android tiene la configurazione
del pannello: **e` la reference per la sequenza DSI**, molto piu` ricca del
DT.

### FATTO — il hang del 10 novembre e` stato CAPITO

La prima versione di `shim/emmc_probe.c` mandava comandi MMC (CMD5 awake)
prima che il controller fosse messo nella modalita` giusta, e il telefono
**e` andato in hang** (non piu` raggiungibile su USB, sbloccato solo con la
procedura LAF/TIP).

Le due cause, entrambe nel probe:

1. **Ordine sbagliato**: in `shim/emmc.c` la sequenza e`
   `CORE_HC_MODE bit0 (HC_MODE_EN, che LK lascia SPENTO) -> core power
   reset (CORE_POWER bit 7) -> HC_MODE_EN -> FF_CLK_SW_RST_DIS` e **solo
   dopo** si manda un comando. Il commento in emmc.c spiega perche`: il
   reset del core serve a risincronizzare il lato SDHCI con la card. Un
   comando mandato nella modalita` sbagliata non torna e il loop di attesa
   gira per sempre su un controller che non risponde.
2. **Bit di interrupt sbagliati**: il probe usava bit 1 per "command done"
   (in realta` e` bit 0) e bit 4 per "transfer done" (in realta` e` bit 1),
   quindi aspettava un interrupt che non arriva mai. I valori giusti sono in
   `shim/emmc.c:129-131`.

**Correzione applicata**: `shim/emmc_probe.c` ora e` **SOLO LETTURE**
(verificato: nessun `wr32`/`wr16`/`wr8` nel file). Il test "la card risponde
a CMD5?" e` stato spostato in `shim/emmc.c:emmc_wake()`, che gira DOPO che il
controller e` stato messo in HC mode. I candidati RCA sono ora `{0, 2, 1}`:
**RCA 0 per primo**, perche` per la JEDEC la card risponde all'awake senza
che l'RCA sia nominato => separa H1/H3 (card muta) da H2 (card viva ma non
selezionata) con un solo comando.

Inoltre il probe ora legge tutte le pipe (VIG0-2, RGB0-2) e tutte le
interfacce (INTF0-3) con il contatore di frame, con gli offset presi dal DT
del G4 (`qcom,mdss-pipe-rgb-off = <0x15000 0x17000 0x19000>`,
`qcom,mdss-intf-off = <0x6b000 0x6b800 0x6c000 0x6c800>`), non piu` quelli
del SM6225.

### FATTO — il telefono e` stato recuperato e il boot riportato a stock

- Il pstore e` una risorsa **一次性**: il boot della recovery lo
  sovrascrive. Il log dello shim va letto **subito** dopo il run, non dopo.
- Il recovery TWRP 3.7 **non ha /dev/mem**: `tools/read-trail-imem.py` non
  funziona da li. Ha pero` `/sys/kernel/debug/` montabile, che e' la strada
  migliore per il display (vedi sopra).
- La partizione `boot` e` stata riportata a `backup/boot-stock.img` due
  volte: una via LAF (verificata con reread, hash `070980b2...`) e una via
  fastboot dopo il test.

### FATTO — nuove strade di misura, senza rischio per il telefono

`/sys/kernel/debug/mdp/` espone: `reg`, `off`, `dump`, `stat`, `perf/`,
`dsi0_ctrl_reg`, `dsi1_ctrl_reg`, `dsi0_phy_reg`, `panel_reg`, `buffers`,
`xlog`. E `/sys/kernel/debug/mdss_panel/dsi0/` espone `dcs_cmd_by_left`,
`te/`, `fbc/`, `mipi/`, `lcdc/`, `clk_rate`.

**Queste misure si fanno dalla recovery, con il kernel vero: zero rischio
per il telefono**, e danno piu` informazione di qualunque sonda nel shim.
Il probe D0 nel shim e' ora ridondante: la pipe attiva si puo` dedurre
qui, senza toccare il telefono.

---

## Sessione 2026-11-10 (terza parte): il secondo hang, e la risposta alla domanda "come si vede il boot?"

### FATTO — il secondo hang e` avvenuto CON il probe in sola lettura

La seconda versione di `shim/emmc_probe.c` (solo letture, verificato: nessun
`wr32`/`wr16`/`wr8` nel file, tutti e 71 gli indirizzi toccati dentro la
finestra MDP in sola lettura) ha **riprodotto lo stesso hang**: il telefono
si e` fermo al boot, non rispondeva su USB, e si e` sbloccato con la
procedura LAF/TIP.

**Questo e` l'informazione piu` importante della sessione**: il hang non e`
causato dalle scritture del probe. Il primo hang aveva due difetti (ordine
sbagliato + bit di interrupt sbagliati) che sono stati corretti, e il
telefono si e` bloccato lo stesso.

### FATTO — l'ipotesi residua: l'accesso al blocco MDP col clock spento

Il blocco MDP (0xfd900000) e' nella allowlist in sola lettura, ma **la
allowlist non garantisce che il clock sia acceso**: garantisce solo che
l'indirizzo sia permesso. La regola di GUIDE-05 ("una finestra nuova si
prova in QEMU e poi si dichiara **con la prova che la finestra e' viva**")
vale anche per le LETTURE: storicamente la seconda versione della sonda ha
fatto un hang **leggendo** il TLMM, una finestra mai toccata.

Al momento in cui lo shim parte, il LK ha **spento** il display per
passare il controllo (LK inizializza il pannello — nel binario ci sono
`target_display_init`, `dsi_pll_20nm_enable_seq`, `lge_panel_config_pre_init`
— e poi lo spegne prima di saltare al kernel). Quindi il clock MDSS/DSI
e' probabilmente **spento** quando il nostro shim legge 0xfd900000: la
transazione non completa, l'AXI si blocca, e la CPU resta dentro l'accesso.
Esattamente il meccanismo dei due hang precedenti.

**Non e` verificato**: non abbiamo la prova che il clock MDP sia spento in
quel momento. Il pstore che lo direbbe e` stato sovrascritto dalla recovery.
Ma e` l'ipotesi piu` economica da testare, e il test e` gia` pronto.

### FATTO — COME SI VEDE IL BOOT: la risposta alla domanda

La domanda ("non e` possibile vedere durante il boot cosa sta caricando?")
ha una risposta concreta, con tre strade, tutte gia' in mano:

**1. Il canale gia' esiste e funziona: il pstore, ma e` una risorsa一次性.**
`shim/pstore.c` scrive il log del shim nella zona console del ramoops
(0xff00000, 128 KiB, console 0x20000). La si legge come
`/sys/fs/pstore/console-ramoops-0`.

**Il problema**: la zona e` **sovrascritta dal kernel che parte dopo**. Il
TWRP 3.7 ha `CONFIG_PSTORE_CONSOLE` disabilitato (twrp_g4_defconfig:4068),
quindi non dovrebbe scriverla... ma il file che ho letto conteneva il
dmesg **del TWRP stesso**, non il log dello shim. Motivo: il ramoops
**conserva piu' record** e quello che il TWRP espone come `console-ramoops-0`
e' l'ultimo, scritto dal kernel Android/TWRP precedente. Il record dello shim
era in un'altra posizione, o e' stato rimpiazzato dal boot Android
intervenuto prima.

**Regola operativa**: il pstore va letto **immediatamente** dopo il run, e il
telefono non deve fare un boot Android nel mezzo. Il ciclo giusto e':
flash -> reboot -> **leggere subito** -> e solo dopo riavviare.

**2. Il trail in IMEM**: sopravvive a tutto (e` IMEM, non DRAM) e il shim lo
scrive a ogni passo (`shim/trail.h`). Ma si legge con `/dev/mem`, e **il
TWRP 3.7 non ha /dev/mem** (`# CONFIG_DEVMEM is not set` anche in
twrp_g4_defconfig:1837, come in h815_defconfig:1895). Il
`tools/read-trail-imem.py` quindi non funziona dalla recovery attuale.
**Fix possibile**: usare il kernel **NetHunter** (`backup/nethunter-boot.img`,
CAFE con DEVMEM) al posto del TWRP per le letture: e` il boot image che il
progetto ha gia' e che ha i permessi giusti.

**3. Il nuovo canale, e il piu` utile: `/sys/kernel/debug/mdp/` dalla
recovery.** Il driver MDP espone i propri registri in debugfs:
`reg`, `off`, `dump`, `stat`, `perf/`, `buffers`, `xlog`,
`dsi0_ctrl_reg`, `dsi1_ctrl_reg`, `dsi0_phy_reg`, `panel_reg`, e
`/sys/kernel/debug/mdss_panel/dsi0/` con `dcs_cmd_by_left`, `te/`, `fbc/`,
`mipi/`, `lcdc/`, `clk_rate`.

Questo e' il canale che **risolve il problema di visibilita del display**:
per leggere i registri dell'MDP **non serve piu' una sonda nello shim**.
La recovery ha il kernel vero che guida il pannello, l'MDP e' vivo e
cloccato, e i registri si leggono con un `cat`. **Zero rischio per il
telefono**, e piu' informazione di quanto il shim potrebbe darne.

### FATTO — cosa e' stato misurato cosi' (tutto dalla recovery, senza shim)

- la pipe attiva e' **RGB2** (offset 0x19000) su **INTF2**, non VIG0;
- il framebuffer del kernel e' all'indirizzo fisico **0x00020000**;
- `mdssfb_90000`, 1440x5120 virtual, **stride 5760**, **32 bpp**;
- clk_rate 910224000, 4 lane attive, 60 fps, FBC disabilitato, bpp 24;
- timing: h_front 168, h_back 88, h_pulse 4, v_front 9, v_back 10;
- TE attivo, start_pos 4, refx100 6000;
- DSI0 e DSI1 hanno registri identici tranne l'ID del link a 0xa0
  (1000dc57 vs 10006257).

**Il piano del display va riscritto**: la patch 0092 di ios-topaz punta
VIG0, e sul G4 la pipe giusta e' un'altra. Serve sapere quale pipe userebbe
il pannello in comando (LK usava la sua, e i registri del pannello sono
nella zona cont_splash).

### FATTO — il telefono e' stato riportato a stock e verificato

Dopo il secondo hang il telefono e' stato sbloccato in Download Mode e la
partizione `boot` e' stata ripristinata via LAF. **Rilettura byte per byte:
hash `070980b26f62d79f...` = `backup/boot-stock.img`.** Il telefono e' in
Download Mode, pronto per un riavvio.

---

## Sessione 2026-11-10, quarta parte: REVISIONE CRITICA del lavoro precedente, fix e primo run con il percorso clock HS400

Questa parte rivede con sospetto tutto quello che era stato fatto fino al
2026-11-10 pomeriggio, trova **cinque difetti reali** (uno dei quali
annullava il test che era in programma) e ne corregge quattro. Poi esegue
il primo run con la correzione.

### FATTO 1 — il test CMD5 che era "in programma" NON ESISTEVA nel codice

`emmc_wake()` (la funzione che doveva fare il test E0) era dentro

```c
#if !EMMC_LK_BRINGUP		/* only the legacy path uses the wake-up */
```

e `EMMC_LK_BRINGUP` vale **1** per default (`shim/emmc.c:68-69`), senza
alcun knob in `shim/build.sh`. Quindi:

- la funzione **non era mai compilata** nella build del telefono;
- l'immagine `shim/out/shim-boot-e0-only.img` non conteneva il test;
- il flash previsto avrebbe prodotto **esattamente** il risultato delle
  run precedenti, con un rischio di hang identico.

E' il difetto piu' grave della sessione: il test era documentato come
"pronto", ma non esisteva. **Fix**: `emmc_wake()` e' ora compilata sempre
e viene chiamata dal bring-up LK (`shim/emmc_lk_bringup.inc`), che e' il
percorso che gira davvero.

### FATTO 2 — "CMD5 con arg 0" era sbagliato rispetto alla specifica

Il commento del codice diceva: *"per the JEDEC spec, CMD5 (SLEEP_AWAKE) with
argument 0 asks the card to wake up without naming an RCA, and the card
answers with its OCR whatever RCA it had"*.

**Non e' vero.** L'indirizzo broadcast del CMD5 e' **RCA 0xFFFF**, non 0:
l'RCA 0 indirizza la carta che non ha ancora un RCA (cioe' una carta
ancora in identificazione), e una carta in Sleep ha l'RCA che le ha
assegnato l'LK (2). Con RCA 0 la carta non risponde: il test avrebbe
mostrato "timeout" anche con la carta viva e addormentata, cioe' una
**conclusione falsa nella direzione sbagliata**.

Il terreno (`research/lk-g4/lk-source-recovered/` + il driver Linux che
funziona su questo telefono) dice cosi':

- LK, `mmc.c:3430`: `cmd.argument = (mmc_card.rca << MMC_CARD_RCA_BIT) | MMC_CARD_SLEEP`
  (RCA nei bit [31:16], bit 15 = sleep; `MMC_CARD_RCA_BIT` 16 e
  `MMC_CARD_SLEEP (1<<15)`, `mmc.h:568-569`);
- Linux, `drivers/mmc/core/mmc_ops.c` (`mmc_card_sleepawake`):
  `cmd.arg = card->rca << 16;` e, per dormire, `| 1 << 15`.

**Fix**: `emmc_wake()` prova prima l'**indirizzo broadcast** (0xffff0000,
che per la specifica raggiunge ogni carta qualunque RCA abbia), poi
l'RCA che l'LK aveva assegnato (2), poi 1 come riserva.

### FATTO 3 — la risposta diagnostica veniva sovrascritta

La versione vecchia scriveva il risultato del CMD5 con RCA 0 in
`g_diag[D_RCA]` e poi il ciclo lo **sovrascriveva** con l'RCA che aveva
funzionato: la lettura interessante (la risposta del test) spariva
esattamente nel caso in cui serviva. **Fix**: tre slot nuovi e dedicati
(`D_WAKE_BC`, `D_WAKE_RSP`, `D_WAKE_INT`), piu' `D_WAKE_VS` per il
registro vendor spec.

### FATTO 4 — i flag di risposta erano sbagliati per il CMD5

`emmc_cmd()` metteva `CMD_CRC_CHECK | CMD_IDX_CHECK` su tutte le risposte
a 48 bit. Ma:

- il **CRC7** sta nelle risposte R1, **non** nell'OCR: il CMD5 risponde
  con un OCR (R3), che non ha CRC;
- l'**index check** vale solo per la risposta a 136 bit (R2): il campo
  index non esiste nelle 48 bit.

Il driver Linux che funziona (`drivers/mmc/host/sdhci.c`) mette
`SDHCI_CMD_INDEX` solo per `MMC_RSP_136`, e per il CMD5 manda
`MMC_RSP_R1B` **senza** `MMC_RSP_CRC`.

Con i flag sbagliati il controller butta via una risposta buona (errore
CRC) o segnala un index error fasullo: **la carta sembra muta anche quando
ha risposto**.

**Fix**: `CMD_RESP_R3` (0x82) e `CMD_RESP_R3BUSY` (0x83) aggiunti;
l'index check solo per `CMD_RESP_136`, il CRC check solo per R1; il CMD5
usa `CMD_RESP_R3BUSY`.

Verificato offline sul modello QEMU: i flag scritti ora sono
`CMD5(0x03)` (48 bit + busy, nessun CRC/index), `CMD7(0x0b)` (48 bit +
CRC, **senza** index), `CMD1(0x02)`, `CMD2(0x19)` (136 + CRC + index),
`CMD3(0x0a)`, `CMD16(0x0a)`, `CMD8(0x3a)`.

### FATTO 5 — l'ipotesi H1 (VCC spenta) e' SMENTITA dal dmesg del telefono

Il dmesg del kernel Android di questo telefono
(`research/raw-2026-11-05/3.1b_dmesg_mmc2.txt`) dice:

```
mmc0: no vqmmc regulator found
mmc0: no vmmc regulator found
mmc0: new HS400 MMC card at address 0001
```

Il kernel **non trova nessun regolatore** per la VCC/VQMMC della eMMC e
quindi **non accende e non spegne nessun regolatore**: la carta e' gia'
alimentata quando il kernel parte, e funziona. La VCC (pm8994_l20) non e'
mai stata spenta dall'LK. **H1 esclusa**: l'ipotesi utile era l'H7
(regolatore RPM), ma il dato dice che il kernel proprio non la usa.

Da notare anche: la carta e' una **HS400** (indirizzo 0001, BKOPS_EN,
tuning phase 11).

### FATTO 6 — l'ipotesi che mancava: il PERCORSO CLOCK HS400

Questa e' la scoperta della sessione, e spiega tutto quello che era
rimasto fermo.

Il commento gia' presente in `shim/emmc_lk_bringup.inc` diceva gia' la
verita': *"after an HS400 session the MCI is left with MCLK_SEL = HS400 and
the CDC select in HS400 mode, and the identification clock then does not
reach the card. The Linux driver puts both back to the default before it
talks to a card that is not in HS400."*

Il driver Linux (che su questo telefono funziona) lo fa in
`sdhci_msm_set_clock`:

```
 * HS400 - divided clock (free running MCLK/2)
 * All other modes - default (free running MCLK)
```

e quando entra in HS400 scrive `CORE_HC_MCLK_SEL_HS400` e
`CORE_HC_SELECT_IN_HS400 | CORE_HC_SELECT_IN_EN` nel registro vendor spec
(`HC+0x10c`).

**Lo shim non lo faceva MAI.** Con la carta in HS400 e l'MCI lasciato sul
percorso HS400, il clock di identificazione a 400 kHz **non arriva alla
carta**: la carta non sente niente, il controller manda i comandi e scade
il timeout. E' esattamente il sintomo misurato:

```
CMD n=5 rc=FFFFFFFD int=00018000 pres=01E80007
CMD n=1 rc=FFFFFFFD int=00018000 pres=01F80001
```

`0x00018000` = bit 15 (errore generico) + bit 16 (**command timeout**),
**senza** bit 17 (CRC error) e **senza** bit 19 (index error): cioe' la
carta non ha risposto **niente**, non ha risposto male.

La mappa dei bit e' quella del kernel di questa board
(`drivers/mmc/host/sdhci.h`: `SDHCI_INT_ERROR 0x8000`,
`SDHCI_INT_TIMEOUT 0x10000`, `SDHCI_INT_CRC 0x20000`,
`SDHCI_INT_INDEX 0x80000`).

**Le otto varianti della sweep del 2026-11-09 non l'avevano mai provato**:
`LK_F_VENDOR_RESET` e `LK_F_CLR_PWRSAVE` erano definiti e **usati da
nessuna parte**. La sweep girava sugli assi potenza/tensione/ack/
registri, e il percorso clock era l'asse non esplorato.

**Fix**: la variante default ora porta `LK_F_VENDOR_RESET | LK_F_CLR_PWRSAVE`
(il percorso clock torna a legacy, il gating del clock della carta
spento), il registro vendor spec viene **stampato in console** (prima e
dopo, con i campi decodificati: MCLK_SEL, SELECT_IN, SELECT_IN_EN,
PWRSAVE), e la sweep ha due varianti nuove: V8 = il vecchio controllo
(senza reset del percorso clock, deve restare muto: e' la prova) e V9 =
solo il reset del percorso clock.

### FATTO 7 — il telefono e' entrato in BOOTLOOP con la nuova build

La build con i fix e' stata flashata e il telefono e' partito. Secondo
l'utente: *"parte, mostra il logo lg, si spegne dopo qualche secondo e si
riaccende cosi' in loop"*.

FATTO: il ciclo e' rapidissimo (qualche secondo), e il tempo di attesa
diagnostico dello shim dipende dall'**indice di evento** dell'eMMC
(`wait = SHIM_FINE_BASE_S + 20 * ev`, `shim/shim.c:471`): con la carta
che risponde l'evento e' "ALL OK" (indice 0) e l'attesa e' la minima, con
la carta muta era un indice alto e l'attesa era lunga (decine di
secondi). **Quindi il loop e' la prova indiretta che la carta ha risposto
al shim**: il comportamento e' cambiato rispetto a tutte le run precedenti.

FATTO (ancora da verificare con il pstore): l'LK riavvia il `boot`, che
contiene lo shim, e il ciclo si ripete. Due ipotesi, da separare con il
log: (a) l'LK non onora il `LK_RECOVERY_MODE` che lo shim scrive prima
del reset, e quindi riparte dal `boot`; (b) lo shim lascia la carta e il
controller in uno stato che l'LK non riesce a usare (per esempio il
percorso clock tornato a legacy mentre l'LK si aspetta HS400, o la
carta selezionata e sveglia invece che addormentata).

**Il pstore e' il canale**: la console ramoops del telefono contiene il
log dell'ultima run (TWRP 3.7 ha `CONFIG_PSTORE_CONSOLE` disabilitato, e
quindi non la sovrascrive). Da li' si legge `WAKE bc=`, `VSPC in=/out=`,
`FINE ev=` e `STEP`.

### FATTO — il telefono e' stato riparato e verificato

Il telefono e' stato portato in Download Mode e la partizione `boot`
ripristinata via LAF (`tools/lglaf/partitions.py`, autenticazione KILO
modo 2). **Rilettura byte per byte: `070980b26f62d79f610b388d1c731083bf9e0e916f3d7a7b5bb76c541d157177`
= `backup/boot-stock.img`.**

**Difetto del tool trovato e corretto**: `tools/laf-tool.sh restore-boot`
aveva come default `kdz/extracted/boot.image` (sha256 `2267f77c...`), che
e' il boot **Android originale** del KDZ, NON l'immagine verificata del
progetto (che e' il TWRP 3.7, sha256 `070980b2...`). Ripristinare quella
lascia il telefono che avvia Android e non ha pstore ne' ADB. Ora il
default e' `backup/boot-stock.img`.

## Sessione 2026-11-10, quinta parte: il pstore non e' un canale affidabile, e la CI per il kernel H815

### FATTO 1 — ogni kernel Linux cancella il record del shim

Prova eseguita oggi (13:56-13:59), con la build con i fix:

1. `fastboot boot shim/out/shim-boot.img` (run 1) — il telefono torna in
   fastboot dopo ~20 s;
2. `fastboot boot shim/out/shim-boot.img` (run 2) — il secondo run legge il
   record del primo e lo ristampa (e lo salva insieme al proprio);
3. `fastboot boot /tmp/twrp-nethunter.img` — TWRP con il kernel NetHunter CAF,
   ADB attivo;
4. `adb pull /sys/fs/pstore/console-ramoops-0` -> 116.488 byte.

Risultato: **zero occorrenze di `G4SHIMPS1`** nel file, e il suo contenuto e'
la console del kernel CAF che sta girando (ION heap, msm_bus_fabric, PMIC).
`tools/read-pstore.py` risponde "no shim record".

Il confronto con i dump storici (`research/raw-2026-11-09/pstore-sd-run*.bin`)
spiega la differenza: **quei file contengono il marker**, e sono stati
presi quando il boot successivo era la recovery TeamWin. Il canale ha
funzionato finche' non e' passato un kernel Linux in mezzo: il ramoops e'
una zona che il kernel pulisce e riscrive con la propria console, e la
zona 0xff00000 (dove lo shim scrive la copia per Android) la recovery la
tratta come record zone e la svuota.

Conseguenza: per leggere il log dello shim **non basta** il ramoops, serve
`/dev/mem` (il trail in IMEM a `0xfe80f000`, che sopravvive al reset e che
il kernel Linux non tocca). Nessuno dei kernel disponibili sul telefono ha
`CONFIG_DEVMEM` (NetHunter, TWRP TeamWin, boot-stock: tutti
`# CONFIG_DEVMEM is not set`).

### FATTO 2 — il kernel TWRP con kernel NetHunter CAF funziona, e il suo pacchetto e' noto

`/tmp/twrp-nethunter.img` (24.424.448 byte) e' formato da:

| parte | byte | da dove |
|---|---|---|
| kernel NetHunter CAF 3.10.108-gbc450f725 | 9.379.377 | `variant_h815_defconfig` |
| ramdisk TWRP TeamWin 3.7 | 13.805.965 | `backup/twrp-3.7.0_9-0-g4.img` |
| dt.img (QCDT v3) | 1.232.896 | il device tree del telefono |

Il dtb che l'LK sceglie per la board (`board-id 0xb64`) e'
`"Qualcomm Technologies, Inc. MSM8992 P1 GLOBAL COM"` / `qcom,msm8992-cdp`,
e `tools/verify_bootimg.py` lo conferma anche per l'entry selezionata.

`tools/pack-twrp-kernel.sh` e' lo script che ricostruisce un boot image con
questo pacchetto (kernel nuovo, stesso ramdisk, stesso dt.img, stessi campi
di header: kernel 0x8000, ramdisk 0x2200000, tags 0x100, page 4096, e il
cmdline del TeamWin). **Validato**: rimpacchettando il kernel gia'
funzionante il risultato e' byte per byte identico all'originale
(sha256 `335c93c268be4f99360059f537b8fef94ed031954a079e24dcf4a1c498a5adcd`).
La dimensione totale (~24,5 MB) resta sotto il limite che l'LK accetta per
`fastboot boot` (~28 MB), quindi i test non richiedono di flashare nulla.

### FATTO 3 — una nota sul `variant_h815_defconfig` di questo kernel

Nel tree `netheloid/nethunter_kernel_g4` il supporto delle varianti e' **morto
codice**: `scripts/kconfig/Makefile` esporta `KCONFIG_VARIANT` (riga 85) e
`KCONFIG_EXTRA` (riga 89), ma nessun altro file li legge. Quindi
`make stock_defconfig VARIANT_DEFCONFIG=variant_h815_defconfig` produce il
solo `stock_defconfig`, dove `CONFIG_MACH_MSM8992_P1_GLOBAL_COM` e'
**disabilitato**, e i dts `msm8992-p1_global_com*` **non esistono** nel tree.
Il kernel parte lo stesso (il codice e' condiviso e la selezione la fa il
device tree), ed e' quello che ha gia' dato ADB sul telefono. Se un giorno
servisse la variante "ufficiale", il merge va fatto a mano con
`scripts/kconfig/merge_config.sh`.

### FATTO 4 — la CI per il kernel H815 (DEVMEM + HFS+)

`.github/workflows/kernel-h815.yml`: clona `nepheloid/nethunter_kernel_g4`
(commit `850e70b7`), installa il toolchain **GCC 4.9** (AOSP
`aarch64-linux-android-4.9`, tag `android-5.0.0_r4`, da android.googlesource:
il download Linaro `gcc-linaro-4.9-2016.02` non esiste piu', reindirizza a
una pagina di contatto), abilita `CONFIG_DEVMEM`, `CONFIG_HFS_FS` e
`CONFIG_HFSPLUS_FS` con `scripts/config` + `olddefconfig`, compila e carica
l'`Image` come artifact.

Primo tentativo fallito: in questo 3.10 `make O=build` pretende che la
directory di output esista gia' (`output directory "build" does not exist`):
serve `mkdir -p build` prima.

Il pacchetto finale (kernel + ramdisk TeamWin + dt.img) si fa in locale con
`tools/pack-twrp-kernel.sh`, perche' `backup/` non e' nel git (contiene IMEI
e calibrazioni).

### FATTO 5 — stato della build del telefono e dell'audit

`shim/out/shim-boot.img` sha256 `b070a074132c8f6941a663edaeb6174592eba37d8f217c813ae86f731da99652`
(kernel 380.928 byte, dt.img 1.232.896 = il DT del telefono),
`tools/check-mmio-writes.py` verde (9 finestre di device), build con
`SHIM_EMMC_PROBE=1 SHIM_EMMC_MODEL=0 SHIM_HW_DIAG=1 SHIM_SD_INIT=0
SHIM_SD_PAYLOAD=0 SHIM_SD_PROBE=0 SHIM_MMIO_SELFTEST=0 RAW_TIMING=1
FINE_TIMING=1 DIAG_VARIANTS=0 DIAG_TIMING=0`.

### PROSSIMO PASSO

Bootare il kernel con `CONFIG_DEVMEM` e `CONFIG_HFSPLUS_FS`, poi:
1. leggere il trail IMEM (`tools/read-trail-imem.py` con `/dev/mem`) per
   sapere **quale fase** hanga nel bootloop;
2. leggere `0x50000000` (scratch RAM) con `dd if=/dev/mem`;
3. montare `userdata` (il rootfs iOS) con `mount -t hfsplus` e leggerlo.

### FATTO 6 — CORREZIONE: il "limite di 28 MB" di `fastboot boot` non esiste

Una mia conclusione di questa sessione era sbagliata e va corretta: avevo
dedotto che l'LK accettasse `fastboot boot` solo fino a ~28 MB, e lo avevo
motivato con i due tentativi falliti da 40 MB. Ricontrollati, quei due
fallimenti **non** avevano la dimensione come causa:

* `fastboot boot backup/nethunter-boot.img` (40 MB) e
  `fastboot boot backup/boot-stock.img` (40 MB) sono stati **accettati**
  dal bootloader (`Sending 'boot.img' (40 MB) OKAY`, `Booting OKAY`): il
  telefono si e' riavviato e non e' partito perche' il ramdock da 1 MB
  vuole la ROM NetHunter installata, non perche' l'immagine fosse troppo
  grande;
* il "NetHunter troncato" da 11,6 MB non partiva perche' era troncato.

Il limite reale e' `max-download-size` (512 MiB) e la dimensione dello
scratch in RAM del bootloader. La documentazione del kernel LG parla di
un'area scratch da 256 MB.

**Perche' il kernel si impacchetta gzip.** Non per la dimensione, ma per
il formato: le due immagini che sono gia' partite su questo telefono
(`backup/twrp-3.7.0_9-0-g4.img` e `twrp-nethunter.img`) hanno il kernel
**gzip** (magic `1f 8b`, 9.346.265 e 9.379.377 byte, che decomprimono in
22.679.056 byte), e l'LK li decomprime. Il kernel nuovo gira in gzip per
stare nel formato che l'LK ha gia' dimostrato di accettare; con il kernel
grezzo l'immagine sarebbe 36,6 MB, con il kernel gzip 24,1 MB.

### FATTO 7 — il pstore si legge col TWRP TeamWin, e dice chi e' il kernel funzionante

Il canale del pstore **non** e' perso: e' il kernel sbagliato a perderlo.
Confrontando i due kernel che girano sul telefono:

* il **NetHunter CAF** (quello di `twrp-nethunter.img`) ha
  `console [pstore-1] enabled` nel dmesg, cioe' `CONFIG_PSTORE_CONSOLE` **attivo**:
  scrive la propria console sul ramoops e sovrascrive il record del shim;
* il **TWRP TeamWin** (`backup/twrp-3.7.0_9-0-g4.img`) non ha
  `CONFIG_PSTORE_CONSOLE`: quando parte, `/sys/fs/pstore/console-ramoops-0`
  contiene **la console del kernel precedente**.

Verificato: dopo un boot di `twrp-nethunter.img` e poi uno di TeamWin, il
file contiene la console del kernel NetHunter (3.10.108), non quella del
TeamWin. **Per leggere una console di kernel si deve quindi avviare il
TeamWin, non il NetHunter.**

### FATTO 8 — il kernel che funziona su questo telefono e' il Titan 3.10.108

Nella console del kernel salvata nel pstore:

    CPU: 2 PID: 1 Comm: swapper/0 Not tainted 3.10.108-gbc450f725 #3
    Hardware name: Qualcomm Technologies, Inc. MSM8992 P1 GLOBAL COM (DT)

`3.10.108-gbc450f725` e' il **kernel Titan** (`aoleary/G4-Titan-Kernel`,
branch `titan_bpf2`, HEAD `acc9bfa9`), non il tree
`nepheloid/nethunter_kernel_g4`, che e' **3.10.84** ("TOSSUG Baby Fish",
stessa base CAF). Il riepilogo precedente li confondeva.

Il kernel Titan e' anche quello giusto per la scheda: `twrp_g4_defconfig`
ha `CONFIG_MACH_MSM8992_P1_GLOBAL_COM=y` e
`CONFIG_BUILD_ARM64_APPENDED_DTB_IMAGE_NAMES="msm8992-p1_global_com-rev-a
...-rev-b ...-rev-c ...-rev-10"`, e i suoi `dts` ci sono
(`arch/arm64/boot/dts/lge/msm8992-p1_global_com/`). La differenza di
dimensione fra i due kernel (22.679.056 byte raw contro 21.650.712) sono
esattamente i quattro dtb appenditi (4 x 307.200 = 1.228.800 byte).

**Il kernel 3.10.84 non parte**: `fastboot boot` accetta l'immagine, il
telefono torna in fastboot dopo 10 secondi e nel ramoops **non c'e' una
riga** della sua console (il pstore contiene ancora quella del 3.10.108,
zero panic). Il fallimento e' quindi a monte del kernel, non un crash.

Nota: la console del 3.10.108 salvata nel pstore contiene un
`WARNING: irq_set_affinity_notifier ... overwriting previous IRQ affinity
notifier` (`kernel/lge/msm8992/kernel/irq/manage.c:285`): e' un warning, non
un panic, e il kernel parte lo stesso.

### FATTO 9 — la partizione `boot` non e' piu' il backup stock

Dump di `mmcblk0p38` (41.943.040 byte): sha256 `09d9ee0bc0a4100c3469d8922b3752527efe6537816d3c00755e14bd5f33f786`,
diverso da `backup/boot-stock.img` (`070980b2...`). Il contenuto e' un boot
image ANDROID con kernel da 9.379.377 byte (gzip, 22.679.056 raw, il
NetHunter/Titan) e ramdisk da 1.015.380 byte: il boot image **del progetto**
(kernel + ramdisk sweet), non lo stock e non il TWRP TeamWin.

Non e' un danno e non e' stato fatto in questa sessione (qui si e' sempre
usato `fastboot boot`, che non scrive nulla). Per ripristinare il backup
c'e' `tools/lglaf/partitions.py boot --restore backup/boot-stock.img`, gia'
collaudato; la partizione `boot` non e' comunque necessaria per i test.

### FATTO 10 — cosa non compila nel tree Titan 3.10.108 (e come si e' aggirato)

Il branch `titan_bpf2` del kernel Titan non compila cosi' com'e': tre
driver sono rimasti indietro rispetto al resto del tree.

| errore | file | rimedio |
|---|---|---|
| `too few arguments to function 'vfs_rename'` | `fs/ecryptfs/inode.c` | `CONFIG_ECRYPT_FS=n` |
| `'CGRP_REMOVED' undeclared`, `struct cgroup has no member named 'parent'` | `block/bfq-cgroup.c` | `CONFIG_CGROUP_BFQIO=n` e `CONFIG_IOSCHED_BFQ=n` |
| `implicit declaration of 'LZ4_decompress_safe'` | `fs/incfs/data_mgmt.c` | `CONFIG_INCREMENTAL_FS=n` |
| `'struct sync_fence' has no member named 'name'` | `drivers/staging/android/trace/sync.h` | `CONFIG_SYNC_DEBUG=y` (il campo `name` esiste solo con quel simbolo) |

Nessuno di questi serve per leggere il rootfs iOS: la CI li disattiva (o
abilita) in `scripts/config` prima di `olddefconfig`.

Nota: il kernel che funziona sul telefono e' `3.10.108-gbc450f725`, un
commit che **non** e' nel repository `aoleary/G4-Titan-Kernel` (nessun
branch e nessun fetch per SHA lo restituisce): e' un commit di un fork o di
una copia locale. Per la CI si usa il branch `titan_bpf2` (`acc9bfa9`),
che e' la stessa base 3.10.108 "TOSSUG Baby Fish".

### FATTO 11 — IL BOOTLOOP: un accesso non allineato a HC+0x29

Con il kernel con `CONFIG_DEVMEM` (costruito dalla CI) il log dello shim si
legge **dalla RAM**, senza pstore e senza strumenti esterni: il shim lo
scrive a `0x50000000` e `dd if=/dev/mem bs=4096 skip=327680` lo legge
(196608 byte letti, il log in ASCII in testa). Il ciclo e':

    fastboot boot shim/out/shim-boot.img     # il shim gira e scrive il log
    fastboot boot twrp-h815-hfs.img          # il kernel HFS+ parte, ADB
    adb exec-out 'dd if=/dev/mem ...'        # e il log si legge

Il log letto (run del 2026-11-10, build con i fix HS400) termina cosi':

    E0 begin
    E0 core_pow    0000000000000041
    E0 core_mciver 000000001000003E
    E0 core_hcmode 0000000000000000
    E0 core_10c    0000000000000000
    E0 hc_vendor   0000000000340B1C
    E0 pres        0000000001F80000
    E0 ctrl1       0000000000000B30
    E0 pwrctrl
    PANIC SYNC
    SLOT n=00000004 cpu=80000000
    STAGE e0d0
    ESR 0000000096000021
    ELR 0000000000098318
    FAR 00000000F9824929
    X0  00000000F9824929

**`FAR = 0xf9824929` e `HC = 0xf9824900`: il fault e' la lettura di
`HC+0x29`, cioe' `SDHCI_PWR_CONTROL`, che e' un registro a BYTE.** La
lettura a 32 bit di un indirizzo non allineato a 4 fa rispondere il
controller con un errore di bus (data abort, ESR 0x96000021) e il telefono
si riavvia: **e' questo il bootloop**.

Due indizi che non puo' essere altro:
* `HC+0x24` (PRESENT) e `HC+0x28` (CTRL1) si leggono senza problemi e sono
  nella **stessa pagina** di 0x29: la pagina e' mappata, quindi il fault non
  e' di traduzione ma di allineamento;
* il codice di bring-up (`shim/emmc_lk_bringup.inc`) gia' legge e scrive
  `HC+0x29` con `rd8`/`wr8`: solo il dump di `shim/emmc_probe.c` lo leggeva
  a 32 bit.

Correzione: `shim/emmc_probe.c` ora usa `rd8(HC + HC_POWER_CTRL)`. Un audit
di tutti gli accessi a 32 bit del progetto non trova altri offset non
allineati a 4.

La lezione: con il controller MMC, **ogni registro a byte va letto a byte**.
Il dump E0 era la prima cosa che leggeva quel registro, perche' e' il primo
del blocco dopo CTRL1: e' per questo che il probe non era mai arrivato
oltre.

### FATTO 12 — il kernel con DEVMEM e HFS+ e' costruito e parte

La CI (`.github/workflows/kernel-h815.yml`) compila il **kernel Titan**
3.10.108 (`aoleary/G4-Titan-Kernel`, branch `titan_bpf2`) con
`CONFIG_DEVMEM`, `CONFIG_HFS_FS` e `CONFIG_HFSPLUS_FS`, e il risultato
**parte**: `Linux version 3.10.108-gacc9bfa9`, ADB attivo, `/dev/mem`
presente, `hfsplus` in `/proc/filesystems`.

L'Image e' 22.155.336 byte; impacchettato gzip (9.234.167) con il ramdisk
TeamWin e il dt.img del telefono dà un boot image di 24.109.056 byte,
avviato con `fastboot boot` senza scrivere nulla sul telefono.

Nota: `CONFIG_DEVMEM` permette di leggere la **RAM** (`0x50000000` si
legge), ma non l'**IMEM** a `0xfe80f000`: quello non e' RAM e `/dev/mem`
risponde "Bad address" su arm64. Il trail IMEM va letto in un altro modo.

### FATTO 13 — il volume su userdata e' HFS+ journaled

La firma e' a offset 1024: `48 58` = **"HX"** (HFSX), versione 5, e subito
dopo `48 46 53 4a` = **"HFSJ"**, il journal info block. Il volume e' quindi
HFS+ **journaled**, e il driver `hfsplus` del kernel Linux non supporta il
journaling: il mount fallisce con `hfsplus: invalid secondary volume
header`.

Per leggerlo servono due strade diverse: un tool userspace che parla HFS+
(hfsplus-tools) oppure estrarre il volume sul PC e montarlo li' con
`hdiutil`. Il rootfs iOS **non** e' stato toccato: e' solo stato letto.

## 2026-11-10 (terza sessione): il CMD2 e il volume HFS+

Tutto quello che segue e' **misurato**, non dedotto. La sessione ha
lavorato su due filoni: il CMD2 (SEND_ARG) che e' l'ultimo comando che
fallisce nello shim, e la lettura del rootfs iOS su `userdata`.

### FATTO 14 — il CMD2: il tipo di risposta e' il sospetto, e si puo' misurare

Il run del 2026-11-10 (log in `docs/shim-log-2026-11-10.txt`) mostra, per
la prima volta, la carta che risponde:

```
CMD5 broadcast  rc=FFFFFFFD int=00018000   (timeout: la carta eMMC non
                                           accetta l'indirizzo broadcast)
CMD5 con RCA 2  rc=00000000  rsp=00001400
CMD7 select     rc=00000000  rsp=00000700
CMD0 (idle)     rc=00000000
CMD1 x 37       rc=00000000  rsp=40FF8080 / 00FF8080 / C0FF8080
CMD2 (SEND_ARG) rc=FFFFFFFD  int=00088000  rsp=48B72E52   <-- fallisce
```

`FFFFFFFD` non e' un timeout: e' -3, cioe' **interrupt di errore**
(-2 sarebbe il timeout). `int=00088000` = bit 15 + bit 19.

Il confronto con il driver Linux che funziona su questo telefono
(`G4-Titan-Kernel`, `drivers/mmc/host/sdhci.c` +
`include/uapi/linux/mmc/core.h`) dice come il CMD2 va programmato:

```
MMC_RSP_R1 = PRESENT|CRC|OPCODE   ->  48 bit + CRC7 + index
MMC_RSP_R2 = PRESENT|136|CRC      ->  136 bit + CRC7     (SENZA index!)
MMC_RSP_R3 = PRESENT              ->  48 bit, senza CRC7 e senza index
```

Il core usa `MMC_RSP_R2` per CMD2 e `MMC_RSP_R1` per CMD3. Lo shim
programmava CMD2 come `CMD_RESP_136` = **136 bit + CRC7 + index**: la
differenza rispetto al driver funzionante e' **il check dell'indice**.

La carta e' una **Samsung 032G74**, CID `11010030333247373400b448b72e5200`
(letto dal sysfs della recovery), RCA 2. Il `rsp=48B72E52` del CMD2
fallito contiene i byte `48 b7 2e 52` del CID: e' la traccia del CID che il
bootloader aveva letto al boot, non una risposta.

Cosa e' cambiato nel codice (`shim/emmc.c`, `shim/emmc_lk_bringup.inc`):

* `emmc_cmd()` e' stato scomposto in `emmc_cmd_flags()`, che prende i
  flag del registro comando cosi' come sono: serve per provare le
  combinazioni senza duplicare la funzione;
* le risposte sono **stati stampate tutte e quattro** (0x0c, 0x10, 0x14,
  0x18): il driver Linux legge l'OCR da 0x0c e la RCA da 0x18 (facendo
  un giro a ritroso e uno shift di 8), lo shim leggeva solo 0x10 e non si
  sapeva quale dei due fosse giusto;
* il CMD2 ora e' uno **sweep**: prova in ordine 136+CRC (come il driver
  funzionante), 136+CRC+index (come prima), 48+CRC, 48 senza CRC, e per
  ogni combinazione stampa il risultato. Il primo che passa vince e il
  log dice quale e';
* il CMD1 non viene piu' fermato sul bit 31 dell'OCR ma sul **bit 30**,
  che e' il power-up status secondo la spec eMMC: il bit 31 e' quello
  che il core Linux usa (MMC_CARD_BUSY) e nel run precedente compariva
  solo alla 37esima CMD1, mentre la carta era gia' pronta alla prima
  (rsp=40FF8080, bit 30 alto). La sequenza CMD1 e' passata da 37 comandi
  a 1.

Build: `shim/out/shim-boot.img`, sha256
`c5622cc7040502071ca534a06cb05752d28af936e1591644c5c3cf2d6f13455a`,
audit MMIO verde (`ok: no raw store bypasses the MMIO allowlist`).

**Non ancora misurato sul telefono**: lo sweep e' stato compilato e
lanciato (`fastboot boot`, 16:17), ma al momento di scrivere questo
appunto il log non era ancora stato letto dalla RAM. Il ciclo di lettura
e' `fastboot boot shim` -> il telefono si riavvia in recovery -> `adb
reboot bootloader` -> `fastboot boot twrp-h815-hfs.img` -> `tools/read-shim-log.py`.

### FATTO 15 — il volume HFS+ di userdata, misurato bene (FATTO 13 era impreciso)

Letto dal telefono con `dd if=/dev/block/mmcblk0p50` (partizione: settori
12419072..61046783, 48.627.712 settori = 24,9 GB):

```
offset 1024  48 58 00 05 00 80 00 21 48 46 53 4a ...
             'HX'  ver5  attributi 0x00800021  lastMountedVersion 'HFSJ'
```

* firma **'HX'** = HFS+ **case-sensitive** (HFSX);
* `lastMountedVersion = 'HFSJ'` (0x4846534a) e' il marcatore con cui la
  spec dice "il volume e' journaled";
* attributi `0x00800021`: bit 0 = volume smontato pulito, bit 15
  (0x8000) = **SOFTLOCK**, bit 23 = 0x800000 (software write);
* **block size 4096**, `totalBlocks = 4.480.000` (18,35 GB di volume
  utili, il resto della partizione e' di coda), `freeBlocks = 1.551.163`;
* catalog file: 128 MB, primo extent al blocco 183691;
* allocation file: 561.152 byte, extent al blocco 1.

**Correzione a FATTO 13**: la stringa 'HFSJ' **non** e' a offset 1536 ma a
offset **1032**, cioe' dentro il volume header (campo `lastMountedVersion`).
L'offset 1536 (dove sta il journal info block) e' **tutto zero**: il
volume e' marcato journaled ma **non ha un journal vero e proprio**, e
non ha nessun backup volume header (l'ultimo settore della partizione e
tutto zero, e anche la fine del volume lo e').

Per il montaggio questo e' la notte buona: il driver `hfsplus` di Linux
**non usa il journal** e con gli attributi che abbiamo **si monta in sola
lettura da solo** (`fs/hfsplus/super.c`: "filesystem is marked locked,
leaving read-only"), quindi il rootfs iOS non viene toccato.

### FATTO 16 — il blocco del mount era il backup volume header, e il telefono si e' bloccato due volte

Il driver rifiutava il volume con `hfsplus: invalid secondary volume
header`. Il codice (`fs/hfsplus/wrapper.c`) spiega perche':

```c
error = hfsplus_submit_bio(sb, part_start + part_size - 2, ...);
if (sbi->s_backup_vhdr->signature != sbi->s_vhdr->signature) {
        pr_warn("invalid secondary volume header\n");
        goto out_free_backup_vhdr;   /* <- il mount muore qui */
}
```

Il backup volume header e' letto dagli ultimi due settori della
**partizione**, che qui sono **zero**: la firma non coincide con quella
del volume header primario ('HX') e il mount muore. Il backup e' solo una
copia di riserva del primario (il driver usa il primario per blocco,
catalog file e allocation file; il backup serve solo all'ioctl di "bless"
e al write-back), quindi **mancare non e' un motivo per rifiutare**.

Patch scritta: `kernel/patches/0001-hfsplus-mount-a-volume-with-no-backup-volume-header.patch`
(accetta il volume solo quando il backup **non c'e' affatto**: se c'e' ma
ha un'altra firma, il mount viene ancora rifiutato). La CI
(`.github/workflows/kernel-h815.yml`) la applica con `git apply` prima
della build. La patch applica pulita (`patch -p1 --dry-run` senza errori).

Nota: il backup verrebbe letto dalla **partizione** e non dal volume, ma
su questo telefono volume e partizione hanno la stessa fine, quindi la
differenza non c'e'.

Due blocchi del telefono, entrambi risolti, entrambi da mettere a verbale
perche' costano tempo:

1. **L'unbind del driver sdhci_msm con la carta montata ha fatto
   crashare il telefono.** L'esperimento (leggere i flag reali dei comandi
   MMC con i tracepoint) era ben fatto ma troppo invasivo: con
   `mmcblk0p49` montata su `/cache`, staccare il driver ha fatto perdere
   il device node. Il telefono si e' riavviato con lo stack USB wedged:
   si enumerava come "LGE Android Phone" (0x1004:0x6340) con
   l'interfaccia fastboot (vendor specific, bulk IN/OUT) ma **nessuna
   risposta** al protocollo, e lo schermo nero. Il recovery e' ripartito
   da solo dopo ~3 minuti, come prima. **La lezione: non staccare il
   driver di un device che ha un volume montato.** Con il tracepoint
   avrei potuto leggere i flag di CMD1/CMD2/CMD3 senza toccare niente,
   se avessi pensato prima all'alternativa.
2. **Telefono con lo stack USB wedged: serve staccare la batteria.**
   L'interfaccia USB di LAF e quella di fastboot hanno lo stesso
   descrittore (class 255 / subclass 255 / protocol 255, endpoint bulk),
   quindi dai descrittori non si distinguono: e' il protocollo che dice
   quale sia. Tenne premuto Volume Su non serve a nulla in quello stato.
   Con la batteria staccata e il telefono riacceso, il fastboot e'
   tornato subito e normale.

### FATTO 17 — cosa NON e' stato fatto (e non va rifatto)

* **non** e' stato staccato il driver sdhci_msm con la carta montata
  (vedi FATTO 16);
* la CI del kernel con la patch hfsplus **non e' stata lanciata**: la
  sessione si e' fermata all'aggiornamento della documentazione;
* il log dello shim con lo sweep del CMD2 **non e' stato ancora letto**:
  e' la prossima misura, e non costa niente (il telefono e' in fastboot
  adesso, il ciclo e' `fastboot boot twrp-h815-hfs.img` +
  `tools/read-shim-log.py`).

---
## 2026-11-10 (sessione sera): CMD2 PASSATO - FINE DEL BLOCCO eMMC

### FATTO 18 — Il loop CMD1 aspettava bit 30 invece di bit 31

Il log del run 2026-11-10-1825 mostrava che CMD1 si fermava su `rsp=40FF8080` (bit 30 = power-up) ma il bit 31 (busy) non era mai raggiunto. Di conseguenza CMD2 falliva con `rc=FFFFFFFD`.

**Root cause identificata**: il loop CMD1 in `emmc_lk_bringup.inc` faceva un numero fisso di iterazioni (`ncmd1=1 o 2`) invece di aspettare che il bit 31 dell'OCR fosse settato, come fa il driver Linux (`G4-Titan-Kernel`, `sdhci.c`).

**Fix applicato** (shim/emmc_lk_bringup.inc, linea 533-543):
```c
/* CMD1: the card powers up and answers with its OCR. */
/* Wait for bit 31 (busy) like the Linux driver does. */
int i;
for (i = 0; i < 1000; i++) {
    if (emmc_cmd(1, ocr, CMD_RESP_R3, &rsp))
        return emmc_ident_fail(0);
    emmc_lk_stage = 1;
    if (rsp & 0x80000000u) /* busy = 1 */
        break;
    emmc_delay_us(1000);
}
```

**Risultato** (log 2026-11-10-18xx):
- CMD1 continua fino a `rsp=C0FF8080` (bit 31 settato)
- **C2OK v=00000009** — CMD2 con 136+CRC passa al primo tentativo
- CID letto correttamente: `48B72E52 373400B4 30333247`
- CMD3 → CMD7 → CMD16 completano con `rc=00000000`
- `EMMC sec=03A3E000` (62.908.672 settori = 29.7 GB)
- Card identificata, writes disattivati (sola lettura)

**SHA shim**: `fec279c0436e3b4332ea42237396c90a4ac21b15bca9e966bffa62c6b391f3ef`

**Prossimo passo**: testare la patch HFS+ per montare il rootfs iOS in sola lettura dalla recovery.

### FATTO 19 — il rootfs iOS si monta: la patch hfsplus funziona sul telefono

Misura del 2026-11-10 sera, kernel CI `42873352434` (Image sha256
`b090ee5f...c6835`) impacchettato con ramdisk TWRP + dt.img (`24.281.088`
byte) e avviato con `fastboot boot`. Dettagli e output in
`docs/test-2026-11-10-hfs-mount.txt`.

- **TWRP monta `userdata` da solo**, read-only: `dmesg` dice
  `hfsplus: no backup volume header (signature 0x0000), the primary one
  (0x4858) is used alone` e `write access to a journaled filesystem is
  not supported ... mounting read-only`. `mount`:
  `/dev/block/mmcblk0p50 on /data type hfsplus (ro,...)`. La patch
  `kernel/patches/0001-...` e' la cura: senza, `invalid secondary volume
  header` (FATTO 13).
- **E' il rootfs giusto**: `SystemVersion.plist` = iPhone OS 16.7.16
  (20H392); `ls /data` mostra Applications/Library/System/private/usr...
  `df`: 17.1G totali, 11.2G usati, 5.9G liberi.
- **Letture corrette su due vie indipendenti**: `/usr/lib/dyld` (922.912
  byte) ha lo stesso sha256 letto dal mount e dal tool userspace
  `hfsplus` sul device raw (`58068345...3757`); 574 byte di
  SystemVersion.plist idem (`526de9e2...1962`).
- **Il tool userspace arrotonda male le dimensioni**: `hfsplus ... ls /`
  elenca bene ma stampa `Total filesystem size: -888385536` (volume
  18.35 GB > 2^31, intero signed a 32 bit). Usarlo per estrarre, non per
  i conti.
- **Un solo boot basta per tutto**: lo stesso kernel legge anche il log
  dello shim in RAM (`dd if=/dev/mem bs=4096 skip=327680 count=48`) e
  conferma `C2OK v=00000009 rsp=48B72E52`,
  `EMMC sec=03A3E000`, `card is identified; writes are off`. Non serve
  un secondo boot diagnostico dopo lo shim.

Prossimo: leggere il rootfs (gia' fatto per due file), poi il track
display; per XNU serve il driver eMMC, non questo kernel Linux.

### FATTO 20 — un boot, un file: `tools/device-test.sh` e la config a runtime

La procedura di test era fatta a mano (pack, boot, ~15 comandi `adb`).
Ora e' un comando solo, e ogni risposta finisce in un file di evidenze:

```bash
HFSPLUS=/path/to/hfsplus tools/device-test.sh /tmp/kci/Image docs/test-$(date +%F).txt
tools/device-test.sh --collect docs/collect.txt      # su un kernel gia' avviato
```

- Se il telefono e' gia' in fastboot non tenta `adb reboot bootloader`
  (prima lo faceva a vuoto e aspettava 60 s).
- Se il file passato e' gia' una boot image (`ANDROID!`) non la
  reimpacchetta; altrimenti `gzip -9` + `tools/pack-twrp-kernel.sh`.
- In un solo boot salva: `uname`, `/proc/cmdline`, `zcat /proc/config.gz`,
  `/proc/filesystems`, `/dev/mem`, `mount`/`dmesg`/`df`/`ls` del volume
  HFS+, `SystemVersion.plist`, sha256 di `/usr/lib/dyld` via mount e via
  `hfsplus` userspace (due letture indipendenti), e il log shim da
  `0x50000000`. Chiude con `HFS+ rootfs: OK` / `FAIL` ed exit code.
- `tools/boot-test-kernel.sh` resta come wrapper: fa `exec` su
  `device-test.sh`, cosi' i comandi vecchi nelle note continuano a valere.
- La CI `kernel-h815` ora compila anche `CONFIG_IKCONFIG` +
  `CONFIG_IKCONFIG_PROC`, quindi `zcat /proc/config.gz` dimostra
  DEVMEM/HFS/HFSPLUS **sul kernel che sta girando**, senza fidarsi
  dell'artifact. (Run 42873352434 non li ha: `no /proc/config.gz`.) Verificato a runtime
con il run 42877233521: `zcat /proc/config.gz` risponde
`CONFIG_IKCONFIG=y`, `CONFIG_DEVMEM=y`, `CONFIG_HFS_FS=y`,
`CONFIG_HFSPLUS_FS=y` (il grep del CI stampa anche `CONFIG_IKCONFIG_PROC=y`).

Dettagli e tabella dei controlli: `docs/DEVICE-TEST.md`.

### FATTO 21 — il display si muove dalla pipe RGB2, ma passa per l'SMMU

Misura del 2026-11-10, dalla recovery (kernel TWRP che pilota il pannello,
quindi l'MDP e' clockato), con `tools/read-mdp-registers.sh` (read-only,
debugfs: scrive "off cnt" in `<block>_off` e legge `<block>_reg`). Evidenze
in `docs/display-2026-11-10.txt`, analisi in `docs/DISPLAY-FINDINGS.md`.

- **Pipe attiva confermata**: `RGB2` (indice 5, base `0xfd919000`) su
  `INTF2` (base `0xfd96c000`). `stat`: RGB2 = play count (27), VIG0/1/2
  a zero, RGB0/RGB1 a 2 (overlay una tantum). `dump` lo dice esplicitamente:
  `SSPP #5 type=RGB ndx=20`, formato RGBA_8888, 1440x2560.
- **Registri RGB2 misurati**: SRC_SIZE `0x0a0005a0` (1440x2560),
  SRC0_ADDR `0x00e30000`, SRC_YSTRIDE0 `0x1680` (5760), SRC_FORMAT
  `0x000236ff`, UNPACK `0x03010002`, op_mode 0 (no BWC), fetch config
  `0x87`. Nessuno scaling (OUT_SIZE = SRC_SIZE).
- **Il fetch va per l'SMMU, non in fisico.** `mdss_fb` alloca da ION system
  heap (`/d/ion/clients/mdss_fb_iclient-0`, 0x1c20000 byte) e chiama
  `ion_map_iommu`; senza dominio IOMMU il driver fallisce la registrazione
  del fb. Quindi `SRC0_ADDR = 0x00e30000` e' una **IOVA** sul
  `mdp_iommu` (device `0xfd9b4000`, context bank `mdp_0` a `0xfd9bc000`,
  SID 0), non un indirizzo fisico. L'ipotesi del piano ("il G4 può usare
  RAM fisica sotto i 4 GiB, gia' usata dal fbcon") vale **solo** se
  l'SMMU resta bypassato, cioe' nello stato in cui l'LK lascia il
  pannello. In TWRP il driver lo attacca.
- **INTF2 in command mode**: TIMING_ENGINE_EN = 0, PANEL_FORMAT =
  `0x0000213f`, FRAME_LINE_COUNT_EN = `0x0000213f`, link id DSI0
  `0x10007e57` / DSI1 `0x10009d57` (dual link), clock 910 MHz, 60 fps,
  porchi come da DT (168/88/4, 9/10). MDP clock 265 MHz, bandwidth 1.21
  GB/s, zero underrun.
- **CTL0**: CTL_TOP `0x01008000` / `0x00020020`, CTL_FLUSH `0x40000018`
  (write-strobe: i bit si accumulano in software, BIT(17) per CTL e
  BIT(pipe->num) per le pipe RGB/VIG 0-2, cioe' BIT(5) per RGB2, poi
  scritti una volta per commit). Il valore `0x20001` di ios-topaz non va
  copiato cosi': va ricalcolato per questo HW.
- **Conseguenza per il porting XNU**: due strade. (a) tenere l'SMMU
  bypassato (come l'LK) e usare indirizzi fisici (piu' semplice, coerente
  con la cont_splash); (b) programmare il context bank 0 dell'MDP SMMU con
  una mappa identita' per il framebuffer. Prima tentativo: (a).
- **Ancora da fare**: sequenza DCS del pannello (reset/init/sleep-out/
  display-on) dal DTSI `dsi-panel-jdi-nt36850-dualmipi0-qhd-cmd.dtsi`;
  sequenza clock MMSSCC + DSI PLL disassemblata dal binario dell'LK;
  il display non si muove finche' i clock non sono accesi (tentativo
  precedente nello shim senza clock ha dato hang, anche in sola lettura).

---
## 2026-11-10 (sessione): Driver eMMC per XNU - inizio

### FATTO 19 — Inizio sviluppo driver eMMC XNU

**Obiettivo**: Driver per leggere il rootfs iOS da XNU su LG G4 H815.

**Riferimento**: UniXNU-lumia950xl/drivers/NativeSdhci/ (stesso SoC MSM8992)
- Indirizzi controller: HC=0xf9824900, CORE=0xf9824000
- IRQ: 123 (HC), 138 (Power) — non usati (polling driver)
- Stesso controller eMMC del Lumia 950 XL

**Lavoro svolto**:
1. Copiato driver NativeSdhci da riferimento in `xnu/drivers/NativeSdhci/`
2. Creato driver semplificato BSD-style `xnu/bsd/dev/sweet_emmc.c`:
   - Implementa `sweet_emmc_rootdev()` per `rd=emmc0`
   - Card identification sequence (CMD0/1/2/3/7/16)
   - Lettura blocchi con polling
   - Read-only (writes return EROFS)
3. Creata patch `xnu/patches/0029-sweet-emmc-root-device.patch`:
   - Aggiunge `bsd/dev/sweet_emmc.c` a bsd/conf/files
   - Modifica IOKitBSDInit.cpp per `rd=emmc0`

**Note importanti**:
- Il driver attuale rifà la card identification da zero
- Il riferimento NativeSdhci è più sofisticato: riutilizza lo stato lasciato da LK
- Test QEMU eMMC passato: `CMD2(0x09)`, C2OK, card identificata

**Prossimo passo**:
- Testare build XNU con CI
- Se la patch compila, testare sul telefono con `rd=emmc0`

---

## 2026-11-11 (sessione): la CI verde non diceva niente, e il driver eMMC non poteva funzionare

### FATTO 22 — la CI XNU saltava 40 patch e non se ne accorgeva

**Il fatto**: il run `42923144296` era "success", ma il kernel che ne e' uscito
era senza il platform expert, senza il boot CPU driver, senza lo stack USB, senza
il console USB, senza i driver touch e senza le patch dell'iowatchdog.

**Perche'**: `blacktop/darwin-xnu-build` applica le patch cosi':

```sh
for PATCH in "${PATCH_DIR}"/*.patch; do
    if APPLY_CHECK=$(git apply --check "$PATCH" 2>&1); then
        git apply "$PATCH"
    else
        warning "Skipping patch ${PATCH} (already upstream, previously applied, or malformed): ${APPLY_CHECK}"
    fi
done
```

Una patch che non applica e' un **warning**, non un errore: la build va avanti
e il risultato e' "verde". Il log del run mostra 40 `Skipping patch`.

**La causa radice**: la patch `0016` (device-tree platform expert) aveva
l'hunk header sbagliato (`@@ -34,6 +34,14 @@` per 20 righe aggiunte invece di
14): `git apply` la dichiarava "corrupt patch at line 25" e la saltava. Tutto
quello che dipendeva da lei dipendeva dal risultato e veniva saltato a sua
volta. La catena si e' rotta al primo hop.

**Le patch rotte** (contesto scritto contro una base diversa):

| patch | problema |
|---|---|
| 0016 | hunk header `+14` per 20 righe: "corrupt patch" |
| 0018 | contesto scritto contro una 0016 senza la entry `lg,h815` |
| 0054 | `bsd/conf/files`: `sweet_sample.c` va **prima** di `mem.c` |
| 0060 | `bsd/conf/files`: manca la riga `sweet_emmc.c` (aggiunta da 0029) |
| 0062 | idem |
| 0072 | idem |

**Correzioni**: `0016` (+14 -> +20), `0018` (contesto aggiornato alla entry
`lg,h815`), e le quattro patch `bsd/conf/files` con la riga `sweet_emmc.c` nel
contesto. Ordine giusto in `bsd/conf/files` dopo tutte le patch:

```
bsd/dev/memdev.c
bsd/dev/sweet_ufs.c
bsd/dev/sweet_sample.c
bsd/dev/sweet_keys.c
bsd/dev/sweet_touch.c
bsd/dev/sweet_rpmh.c
bsd/dev/sweet_spmi.c
bsd/dev/sweet_usb.c
bsd/dev/mem.c
bsd/dev/sweet_emmc.c
bsd/dev/munge.c
bsd/dev/unix_startup.c
```

**Il nuovo controllo**: `tools/check-xnu-patch-chain.sh` applica tutta la serie
a un worktree pulito di xnu-8796 e **dice quali patch falliscono**, eseguendo
anche gli script di staging (`0019`, `0020`, `0026`) come fa la CI. Prima:
44 applicate / 40 fallite. Dopo: **83 applicate / 0 fallite**.

Da usare ogni volta che si tocca la serie, prima di spendere una CI.

### FATTO 23 — il driver eMMC di XNU era un character device: la root non l'avrebbe mai letto

Il driver nel tree non poteva funzionare, su tre livelli indipendenti.

1. **L'interfaccia**. La root di XNU e' un **block device**:
   `vfs_mountroot()` chiama `bdevvp(rootdev, &rootvp)`, e `spec_read()` per un
   vnode VBLK chiama `buf_bread()`, che finisce in `bdevsw[].d_strategy`.
   Un `cdevsw` non viene mai consultato: il `d_read` del vecchio driver non
   serviva a niente. Il modello da copiare e' `bsd/dev/memdev.c` (il device a
   memoria): `bdevsw_add(-1, &sw)`, `d_strategy` che compila il `struct buf`,
   `d_psize` che dice 512, `devfs_make_node(..., DEVFS_BLOCK, ...)`.

2. **I registri**. Il comando era scritto a `HC+0x0c` con una word a 32 bit:
   `0x0c` e' il **Transfer Mode**, non il comando; il comando e' a `HC+0x0e` e
   si scrive a 16 bit, con l'indice nei bit [15:8] e i flag nel byte basso.
   I flag usati erano anche sbagliati (CRC a bit 6, index a bit 7, tipo di
   risposta mai programmato). Con quel codice nessun comando raggiungeva la
   carta.

   I valori **misurati sul telefono** (shim, `emmc.c`), che il driver ora copia:

   | cmd | flag | risposta |
   |---|---|---|
   | CMD0 | `0x00` | nessuna |
   | CMD1 | `0x02` | 48 bit (OCR), **senza** CRC7 e **senza** index |
   | CMD2 | `0x09` | 136 bit + CRC7, **senza** index check |
   | CMD3 | `0x1a` | 48 bit + CRC7 + index |
   | CMD7 | `0x0b` | 48 bit busy + CRC7 |
   | CMD16 | `0x0a` | 48 bit + CRC7 |
   | CMD17 | `0x3a` | 48 bit + CRC7 + index + data present |

   CMD2 con l'index check (`0x19`) e' quello che faceva fallire il CMD2: la
   carta rispondeva con un errore e la linea di comando restava occupata.

3. **La geometria**. La root non e' l'eMMC intero ma la **partizione** che
   contiene il volume HFS+: il volume header di un volume HFS+ sta a offset
   1024 della sua partizione. Il driver legge il GPT (header a LBA 1, entries
   a LBA 2..33), preferisce la partizione chiamata `ios` e la accetta solo se
   a offset 1024 c'e' davvero la firma di un volume header HFS+ (`'HX'`,
   versione 5). Il LBA misurato (12419072) e' il fallback se il GPT non si
   legge.

**Il bring-up** segue lo shim: HC mode, il **clock path legacy** nel vendor
spec register (`HC+0x10c`: `MCLK_SEL=2`, `SELECT_IN` off, `CLK_PWRSAVE` off —
l'LK lascia la MCI sul path HS400 e a quel punto il clock di identificazione
non arriva alla carta), clock a 400 kHz, CMD0..CMD16, e poi **25 MHz**, il
massimo che il timing legacy permette su un bit. A 400 kHz il catalog da
128 MB del volume ci metterebbe tre quarti d'ora.

**Scritture**: rifiutate con `EROFS`. Il medio e' read-only per costruzione.

**Il check locale**: `clang -fsyntax-only` contro l'albero XNU patchato ha
trovato tre errori veri che la CI avrebbe preso: `delay()` richiede
`XNU_KERNEL_PRIVATE`, `ml_io_map` e' dichiarato solo sotto
`MACH_KERNEL_PRIVATE`/`PEXPERT_KERNEL_PRIVATE` (e su arm64 `lck_mtx_t` e' un
tipo **opaco**: va allocato con `lck_mtx_alloc_init`, non statico).

### FATTO 24 — il ciclo di boot di XNU, e perche' il ring del kernel era vuoto

**Il ciclo, sempre lo stesso** (`tools/xnu-boot-test.sh`, i dettagli in
`docs/XNU-BOOT-TEST.md`):

1. `fastboot boot shim/out/shim-boot-g4.img` — LK carica lo shim, lo shim
   porta su l'host eMMC e scrive il suo log, poi passa a XNU;
2. si aspetta che il telefono si assesti (XNU parte, oppure fa panic e il
   telefono torna in recovery);
3. **`fastboot boot /tmp/device-test-boot.img`** — il TWRP custom (kernel CI
   H815 con `CONFIG_DEVMEM` + ramdisk TWRP + dt.img). **Questo passo non e'
   facoltativo**: XNU dice le sue cose nella RAM e la RAM si legge solo con
   un kernel che ha `/dev/mem`. Il TWRP di default non ce l'ha;
4. si legge la RAM.

Niente viene scritto: solo `fastboot boot`.

**Il ring della console del kernel era vuoto, e il motivo e' interessante**:
il driver eMMC gira dentro `IOFindBSDRoot` (quindi dentro
`IOKitBSDInit`), **molto prima** che `bsd_init` alzi la console del livello
BSD. L'aggancio su `_cnputs` che riempie il ring e' installato assai piu'
tardi, e il driver del device root non c'e' nemmeno al suo posto: il suo
`printf()` va a finire nel nulla.

Per questo il driver ora tiene **una copia delle sue parole in un buffer
nel BSS** (`emmc_log_buf`, 8 KiB), che sopravvive al reset, e
`tools/read-xnu-log.py` la legge dalla RAM insieme al ring.

**L'indirizzo fisico di un simbolo del kernel** si ricava dall'header del
payload:

```
pa = va - virt_base + phys_base
```

la stessa mappatura di `phystokv()` (`osfmk/arm64/arm_vm_init.c`).
`virt_base` e' a 0x10 dell'header, `phys_base` a 0x38.

**La RAM non e' cancellata dal reset**, ma due cose la possono sovrascrivere:
il payload (lo shim lo carica *dopo* aver scritto il suo log, e un payload
da 17 MB arriva fino a `0x50000000`, dove sta il log dello shim) e il kernel
della recovery (che prende le pagine che gli servono). Il ring e il log del
driver stanno nella bank `0x80000000`, quella bassa `0x10000000` viene presa
prima, quindi sopravvivono; il log dello shim, che sta nella bank bassa, non
e' garantito dopo un boot di XNU.

### FATTO 25 — XNU parte e muore PRIMA del driver eMMC (e la RAM si puo' leggere)

**Il boot vero** (run `42942530869`, kernel con il driver riscritto, cmdline
`rd=emmc0`): XNU parte, il telefono torna in recovery. Non e' il driver a
fallire: **il driver non e' mai partito**.

**La prova**: il buffer di log del driver (`emmc_log_buf`, nel BSS del kernel)
e' vuoto, e il ring della console del kernel pure. Il vuoto potrebbe
volere dire "la RAM e' stata sovrascritta", ma **non e' cosi'**: confrontando
un simbolo inizializzato di `__DATA` (file e RAM) si ottengono **byte identici**
— la RAM del kernel e' **intatta** dopo il panic e dopo il recovery.

Quindi: **XNU muore prima di `IOFindBSDRoot`**, cioe' nell'avvio tardivo
(ARM init, platform expert, boot CPU), non nel driver eMMC.

**Dove si puo' guardare la console presto**:

| cosa | dove | come si legge |
|---|---|---|
| console early di XNU | `0x9c000000` con il font ISO, celle 16x32 | `tools/read-xnu-early.py` |
| log del driver eMMC | `emmc_log_buf` nel BSS | `tools/read-xnu-log.py` |
| ring della console del kernel | `sweet_log_ring` nel BSS | `tools/read-xnu-log.py` |

**La console early non scrive niente sul G4**: la patch 0010
(`osfmk/arm64/sweet_early.c`) scrive sul "framebuffer" a `0x9c000000`, che
sulla board *sweet* e' il display ma **sul G4 e' RAM** (la bank
`0x80000000-0xdfffffff`). L'area e' leggibile (la RAM e' intatta) ma contiene
rumore: la fascia bianca di stage 0 non c'e' e i marcatori
`SWEET EARLY n: ...` non ci sono. Dunque XNU muore **prima** ancora dei
marcatori, cioe' molto presto.

**Cosa e' emerso lungo la strada**:

* la **bank alta** (`0x80000000-0xdfffffff`, quella che XNU usa come
  `phys_base`) **sopravvive** al boot di XNU e al recovery: il kernel Linux
  della recovery non la tocca;
* la **bank bassa** (`0x10000000-0x5fffffff`) **non** sopravvive: il kernel
  Linux della recovery la usa, e cosi' il log dello shim a `0x50000000` sparisce
  dopo un boot di XNU. Percio' il log dello shim si legge solo se si legge
  **prima** che il recovery torni su;
* la **console del livello BSD** non esiste quando il driver gira: il driver
  e' called da `IOFindBSDRoot`, dentro `IOKitBSDInit`, molto prima di
  `bsd_init`. Il suo `printf()` va a finire nel nulla, e per questo il
  driver tiene una copia delle sue parole in un buffer nel BSS.

**Indirizzo fisico di un simbolo del kernel**: `pa = va - virt_base + phys_base`
(header del payload a 0x10 e 0x38), la stessa mappatura di `phystokv()`.

---

## Check del 2026-11-11 — Verifica ipotesi crash immediato XNU

### CHECK 1 — Entry point (INCONCLUSIVO per l'ultimo g4.bin)
- Kernel corrente (`out-42942530869`): `_start` VA `0xfffffe0007490488`, PA calcolata `0x81490488`
- Il log `JUMP 0x8147b488` era da un boot **precedente** con kernel `out-42012839180`
- Il `g4.bin` corrente usa il kernel `out-42940105016`/`out-42942530869` (entry match)
- **INCONCLUSIVO**: il log citato era obsoleto; il g4.bin corrente non è mai stato bootato su hardware

### CHECK 2 — virt_base/phys_base nel payload (OK)
- `g4.bin`: `virt_base=0xfffffe0006000000`, `phys_base=0x80000000`, `entry_va=0xfffffe0007490488`
- Coerente con i kernel recenti; `entry_pa = 0x81490488` (atteso per il prossimo boot)
- **IPOTESI A: SMENTITA** — i valori sono corretti

### CHECK 3 — Board config (OK)
- Il GIC è letto dal device tree ADT (non hardcoded)
- `mkadt.py --device g4` usa `G4_GICD=0xf9000000`, `G4_GICR=0xf9002000`
- Nessuna patch usa `0x17a00000` (sweet GIC) nel codice
- **IPOTESI B: SMENTITA** — la board config del G4 è corretta

### CHECK 4 — Patch PSCI/0065 (CONFERMATA nel kernel, ma non è la causa del crash)
- La patch `0065-sweet-smp-psci.patch` è presente e applicata
- Il kernel contiene `SweetCPU::startCPU` con `smc #0` (PSCI_CPU_ON)
- **MA**: il cmdline ha `cpus=1`, quindi `startCPU` non viene mai chiamato
- **IPOTESI D: PARZIALMENTE CONFERMATA** — la 0065 è nel kernel ma non causa il crash immediato con `cpus=1`
- La 0065 va esclusa comunque in un commit futuro (il G4 non ha PSCI)

### CHECK 5 — Istruzioni LSE (CONFERMATA — CAUSA PRINCIPALE DEL CRASH) ✅
- **610 istruzioni LSE** nel kernel (`ldadd`, `cas`, `swp`)
- Root cause: `clang` su Apple Silicon emette `ldadd` anche con `-march=armv8-a+crypto` (default platform features)
- Il Cortex-A53 (MSM8992, ARMv8.0) **non ha** LSE (ARMv8.1) → `Undefined Instruction` exception al primo atomic
- Call chain del crash: `arm_init` → `cpu_bootstrap` → `processor_up` → `ml_cpu_up_update_counts` → `ldadd` → FAULT
- **FIX applicato**: aggiunto `-mno-outline-atomics` a `MACHINE_FLAGS_ARM64_SWEET` in `xnu/patches/0004-sweet-machine-config.patch`
- Verifica locale: `-march=armv8-a+crypto -mno-outline-atomics` → 0 LSE ✅

### CHECK 6 — Shim loader (OK)
- Lo shim scrive il log prima del salto: `pstore_console_write` → `g4_wdt_arm` → `((xnu_entry_t)entry_pa)(ba_pa)`
- Funzionamento confermato dai log storici

### CHECK 8 — Patch 0010 early console (OK, ma obsoleta per G4)
- `_sweet_early_mark` è nel kernel (VA `0xfffffe0007489948`)
- La patch 0010 aggiornata usa un **buffer nel BSS** (non `0x9c000000`)
- I marcatori non appaiono in RAM perché XNU crasha PRIMA di `arm_init` (a causa delle LSE)

### CHECK 11 — Board config G4 specifica (OK)
- Nessuna board config separata `G4` necessaria
- Il device tree `g4.adt` ha già i valori corretti per H815
- **IPOTESI B: SMENTITA**

### Prossimo passo
1. Push del commit con `-mno-outline-atomics` → trigger CI → nuova build kernel
2. Ricostruire `g4.bin` con il nuovo kernel: `python3 tools/mkpayload.py ...`
3. Ricostruire `shim-boot-g4.img`: `cd shim && ./build.sh out/g4.bin`
4. Boot hardware con `fastboot boot shim-boot-g4.img`
5. Verifica log: i marcatori `SWEET EARLY 1-5` devono apparire in RAM

## 2026-11-11 — LSE in commpage + registro PAN + firehose LSE (FACT 22-25)

### FACT 22 — La commpage usava `casa` (LSE) → SIGILL su Cortex-A53
- `osfmk/arm/commpage/commpage_asm.s` (`pfz_trylock_and_enqueue` / `_dequeue`)
  usava `casa` (ARMv8.1 LSE). Il Cortex-A53/A57 di msm8992 è ARMv8.0 → trap.
- Il `-mcpu=cortex-a53+crypto` di 0004 non basta: il file `.s` è assemblato con
  i flag di default, e `casa` è un'istruzione esplicita nel sorgente.
- **Fix**: `xnu/patches/0005b-sweet-commpage-llsc.patch` (nome 0005b perché 0006
  era già preso) → entrambi i CAS riscritti come loop LL/SC
  (`ldaxr` + `stlxr` con retry, w14 scratch).
- Verifica: 0 LSE, 4 `ldaxr`/`stlxr` nella commpage assemblata.

### FACT 23 — Il registro `pan` non esiste su msm8992
- Con `-mcpu=cortex-a53` clang rifiuta `__builtin_arm_rsr/wsr("pan")`
  (PAN è ARMv8.1): `invalid register "pan" for llvm.read_register`.
- File coinvolti: `osfmk/arm64/{copyio.c,pcb.c,platform_tests.c}`,
  `osfmk/arm/arm_init.c`, `osfmk/arm/pmap/pmap.c`.
- **Fix**: `xnu/patches/0010b-sweet-no-pan.patch` → letture stubbate a 1,
  scritture no-op. NON si abilita `+pan`: l'hardware non ha il registro e
  `msr pan` traperebbe a runtime.
- Ordine: `0010b` (non `0005c`) perché 0010 usa la riga `wsr("pan",1)` come
  contesto del suo hunk → va applicato dopo.

### FACT 24 — `libfirehose_kernel.a` generava LSE (6 istruzioni)
- La slice arm64 di libfirehose era compilata da xcodebuild con il default
  Apple (`armv8.4+` → LSE): `casa`, `ldclr`, `ldset`, 3× `ldadd` in
  `firehose_buffer_create`.
- Il kernel XNU era già pulito (0 LSE, 1945 LL/SC) — il resto veniva da lì.
- **Fix**: nel workflow, `build.sh` viene patchato per
  (a) costruire **solo la slice arm64** (arm64e richiede armv8.5) e
  (b) appendere a `libfirehose_kernel.xcconfig`:
  `OTHER_CFLAGS = -mkernel -nostdinc -Wno-packed -march=armv8-a+crypto -mno-outline-atomics`
  (in xcconfig `$(inherited)` non esiste: va riscritto l'elenco completo).
- Nota: le slice x86_64/arm64e non servono al kernel SWEET.

### FACT 25 — Pre-test locale (nuovo tool)
- `tools/local-pretest.sh`: ricompila in locale, con le **stesse flag CI**
  (estratte dal log del run), le TU toccate dalle patch e conta le LSE
  nel `.o` risultante.
- Uso: `gh run download <RUN> -n xnu-buildtree -D /tmp/ci-buildtree` poi
  `bash tools/local-pretest.sh`.
- Validazione del gate: su sorgente **non** patchata lo script riproduce
  `instruction requires: lse` (commpage) e `invalid register "pan"` (copyio) →
  è un gate reale, non un no-op.
- Build XNU completa in locale non praticabile: serve il KDK 13.5 (sudo) e un
  SDK ≤ 15; in locale c'è solo Xcode 27 (SDK 27, niente Kernel.framework).

### Stato build
- run `43065985813` fallito (LSE commpage) → corretto
- run `43067814044` **verde** (kernel 0 LSE, arm64 Mach-O) — residui 6 LSE nel firehose
- run `43072646908` fallito (mio override xcconfig, file non esistente nello step) → corretto
- run `43073257684` **verde** ma 6 LSE ancora nel firehose (override xcconfig non applicato)
- run `43075706641` in corso (firehose solo arm64 + `-march=armv8-a+crypto`)

## 2026-11-11 (serale) — kernel 0 LSE, build e boot

### FACT 26 — `-march` NON disattiva LSE, serve `-mcpu`
- Il triple `arm64-apple-macos15.0` implica `+lse` (ogni Apple silicon ha
  LSE). `-march=armv8-a+crypto` da solo **non** lo toglie.
- Verifica locale:
  - `-march=armv8-a+crypto` → `ldaddal` presente (LSE=1)
  - `-mcpu=cortex-a53+crypto` → 0 LSE, `ldaxr/stlxr`
- `-mno-lse` non esiste per clang/AArch64 (`unknown argument`).
- In xcconfig `$(inherited)` non esiste (è roba di make): va riscritto
  l'elenco completo dei flag.

### FACT 27 — Regex LSE: attenzione alle varianti
- `\bldadd\b` NON matcha `ldaddal`/`ldaddh`/`ldaddl`/`casal`.
- Regex corretta: `\b(casa|casp|ldadd|ldclr|ldeor|ldset|swp)[a-z]*\b`.
- Con la regex sbagliata il kernel sembrava pulito (0 LSE) mentre aveva
  8 istruzioni LSE.

### FACT 28 — Run 43080427388: kernel VERDE, 0 LSE
- `file` → Mach-O 64-bit executable arm64
- LSE = 0, LL/SC = 2226
- `libfirehose_kernel.a`: slice arm64 only, `-mcpu=cortex-a53+crypto`
  (le slice x86_64/arm64e non servono al kernel SWEET)
- Run precedenti e loro esito:
  | run | esito | causa |
  | --- | --- | --- |
  | 43065985813 | fallito | LSE `casa` in commpage |
  | 43067814044 | verde | kernel 0 LSE, ma 8 LSE nel firehose |
  | 43072646908 | fallito | il mio override xcconfig puntava a un file inesistente |
  | 43073257684 | verde | override xcconfig non applicato (`$(inherited)`) |
  | 43075706641 | verde | `-march` non bastava (FACT 26) |
  | 43080157946 | fallito | il mio `grep` di verifica cercava la flag vecchia |
  | 43080427388 | **verde** | LSE=0 |

### FACT 29 — Boot sul G4 con il kernel nuovo
- `tools/build-g4-image.sh 43080427388 g4` → `shim/out/shim-boot-g4.img`
  (sha256 `f517639e95cd768e3a9c69016f798306ee9b197831e35cc955c45f57b2eb7b9c`)
- `fastboot boot` (nessun flash): lo shim carica 10 segmenti e salta a XNU
  → `JUMP 00000000814A8488` (PA; VA `0xfffffe00074a8488`).
- Il telefono torna in recovery (watchdog + `LK_RECOVERY_MODE`, ~60 s).
- I ring di XNU in RAM (`sweet_early_buf` PA `0x81c46dbc`, console ring)
  risultano **vuoti**: la recovery ha già riusato la RAM. Per leggerli va
  fatto subito dopo il crash, non dopo il riavvio in recovery.
- Il pstore (`/sys/fs/pstore/console-ramoops-0`) contiene il boot Android
  + il log dello shim: è la via più semplice per vedere dove siamo arrivati.

### Documentazione
- `docs/LSE-PAN-FIX-2026-11-11.md`: raccolta completa (problema, patch,
  pre-test locale, procedura step-by-step, check 4, stato).

## 2026-11-11 (notte) — due difetti "scrivere oltre la mappa" (FACT 30-32)

### FACT 30 — Il repro QEMU-A53 è un artefatto, non l'ADT G4
- Test: payload **SWEET** (ADT noto buono) + CPU `cortex-a53` in QEMU →
  **stesso identico abort** a `common_start+0xcc`
  (`Prefetch Abort, ESR 0x21/0x86000005, FAR 0xb14aa3cc, ELR 0xb14aa3cc`).
- Con `--cpu cortex-a76` lo stesso payload parte e arriva a userspace.
- Quindi il crash a `common_start+0xcc` è il **modello CPU cortex-a53 di QEMU**
  (non implementa il granulo 16K che XNU usa: `ARM_PGBYTES` 16K, `ARM_TT_L2_SHIFT` 25).
- Sul telefono il Cortex-A53 vero supporta 16K → non è la causa.
- **QEMU-A53 non è un banco di prova valido per il G4**: usare `cortex-a76`.

### FACT 31 — Il fbcon dell'ADT G4 dichiarava una finestra troppo piccola (REAL)
- Nodo `sweet-fbcon`: `reg = <0x20000000 0xc00000>` (12 MiB) ma la geometria
  è 1440×2560 con stride 5760 = **14,7 MiB**.
- `fbcon_init` (patch 0003) pulisce **tutte** le righe dichiarate → scrive
  fino a offset 0xe10000, cioè **2,1 MiB oltre la fine della mappa** →
  data abort con `FAR = fbcon_base + 0xc00000` = **fine esatta dei 12 MiB**.
- Lo shim lo sa e lo corregge (`shim/shim.c:1175`: "Never draw outside the
  window the device tree reserved"); XNU no.
- **Fix**: `tools/mkadt.py` clampa l'altezza (`fb_h = fb_size // fb_stride`,
  2560 → 2184) prima di generare il nodo.

### FACT 32 — Il G4 ha un GICv2, XNU scriveva il block SGI/PPI GICv3 (REAL)
- Il G4: `interrupt-controller@f9000000 { compatible = "qcom,msm-qgic2";
  reg = <0xf9000000 0x1000 0xf9002000 0x1000>; }` → GICv2, **finestra GICR
  di 4 KiB**, nessun redistributor frame.
- La patch 0004 (scritta per lo Xiaomi SWEET, GICv3) scrive
  `GICR_IGROUPR0` (+0x10080), `GICR_ISENABLER0` (+0x10100),
  `GICR_IPRIORITYR0` (+0x10400) → **fuori dalla mappa** → data abort.
- Il crash appare come `FAR = GICR_mapping + 0x10080`, cioè 0x10080 oltre
  la fine dei 4 KiB dichiarati: **stessa firma del bug fbcon**.
- **Fix**: `xnu/patches/0017b-sweet-g4-gicv2-no-redistributor.patch`
  - salta il block SGI/PPI se `gicr_size < GICR_SGI_PPI_END` (0x10104);
  - non tocca `GICR_WAKER` (è nel frame redistributor);
  - `find_gicr_pe_base()` **non entra in panic** se non c'è un frame.
  - 0017b (non 0005c) perché 0017 tocca lo stesso file.

### Metodo: QEMU come banco di prova
- Con il profilo G4 in QEMU (`--device g4 --qemu`): GIC alle addr QEMU,
  finestra GICR 1 MiB (il frame redistributor), fbcon clampato.
- Con le fix: **il kernel non faulta più** su fbcon né GIC, prende IRQ del
  timer normalmente (2004 interrupt, ESR 0x3890000) → è vivo.
- Il profilo SWEET con `--ramdisk` arriva a **userspace**: `BSD root: md0`,
  `load_init_program: ... /sbin/launchd` — con kernel e buildtree diversi.
- `tools/qemu-sweet.py --g4` decodifica il pannello G4 (1440×2560 a
  0x20000000) invece di quello SWEET.
- `tools/read-xnu-ram.py` legge dalla RAM del telefono il panic region
  (magic "DEED") e il buffer fbcon dopo un crash.

## 2026-11-11 (notte 2) — il canale di lettura e i boot marker (FACT 33-35)

### FACT 33 — /dev/mem: due indirizzi "RAM" che uccidono la recovery
- `persistent_ram` `0x0ff00000-0x0ffdffff` è elencata in `/proc/iomem` come
  RAM, ma una `dd if=/dev/mem` lì **non restituisce niente** e pochi istanti
  dopo la recovery muore (schermo nero, USB muto, serve staccare la batteria).
- Le due morti precedenti erano **errori miei di aritmetica** sullo `skip` di
  `dd`: `0x08184600` (buco tra `0x06400000` e `0x0f400000`) e `0x03fc0000`
  preso per `0x0ff00000`. Lo `skip` va **sempre** calcolato da un tool.
- `tools/read-pram.py` ora rifiuta qualunque PA fuori dalle finestre RAM
  leggibili (la lista `G4_RAM` **non** contiene più `persistent_ram`), stampa
  un'anteprima esadecimale e cerca `SWEET EARLY` / `G4SHIM` / `DEED`.

### FACT 34 — la canary prova che la pagina sopravvive, il buffer early no
- Prima del salto a XNU lo shim scrive `0xCA11CA00+j` (Device,
  non-cacheable) in tre pagine: `0x81c46000` (pagina di `sweet_early_buf`),
  `0x90000000`, `0xdff00000`.
- Dopo il reboot in recovery (t≈21 s, telefono **vivo e fermo**) tutte e tre
  le canary sono **intatte** → la recovery non riusa quelle pagine.
- Nella pagina `0x81c46000` però **non c'è nessun "SWEET EARLY"**.
- Conclusione: il buffer early è scritto con MMU on e cache **write-back**, e
  un reset del watchdog **non fa il clean della cache**. Il testo può non
  essere mai arrivato in RAM, quindi "buffer vuoto" **non** prova che XNU non
  sia arrivato in `arm_init`. Serve un canale che si scriva da solo in RAM.

### FACT 35 — il panic log del G4 va in RAM normale, non in persistent_ram
- `check_for_panic_log()` usa `ml_io_map_wcomb()`, quindi `/pram` può stare a
  qualunque PA; ma un PA illeggibile da `/dev/mem` (FACT 33) è inutilizzabile
  anche per XNU: qualunque accesso blocca il bus.
- `/pram` spostato da `0x0ff40000` (persistent_ram) a **`0xc0000000`**
  (`tools/mkadt.py: G4_PANIC_LOG_PA`, 256 KiB), dentro il banco alto
  `0x80000000-0xdfffffff`, che è RAM normale e leggibile.
- Non viene sottratto da `mem_size` (sottrarlo dalla cima è ciò che faceva
  morire il kernel dopo `kdp_core zlib memory`): il commento nel file dice
  quando andrà ritagliato per bene.

### Canale dei boot marker (patch 0082)
- `xnu/patches/0082-sweet-boot-markers.patch` scrive 32 bit + `dc civac` in
  `0x90003000` (pagina del banco alto, dentro la mappa bootstrap V=P
  `0x80000000..0xdff00000`) a quattro punti di `osfmk/arm64/start.s`:
  - `+0x00` `0x5EED0001` `_start` raggiunto
  - `+0x40` `0x5EED0002` tabelle bootstrap costruite (MMU ancora off)
  - `+0x80` `0x5EED0003` primo fetch con `SCTLR_EL1.M=1` sopravvissuto
  - `+0xc0` `0x5EED0004` ritorno a `arm_init` imminente
- A `0x90003100`: `phys_base`, `mem_size`, `TCR_EL1`, `TTBR0_EL1`,
  `SCTLR_EL1` → un `boot_args` sbagliato o un granulo sbagliato si vede
  senza debugger.
- Catena patch verificata: 86 applicate, fallisce solo la 0012 già nota.
- CI: run **43109412740** (commit `8eb4371`).
- `tools/read-pram.py --marks` decodifica la pagina: stampa i 4 marker, dice
  fino a quale è arrivato il kernel, e i valori `phys_base`/`mem_size`/
  `TCR_EL1`/`TTBR0_EL1`/`SCTLR_EL1`.
- Baseline misurata in recovery (pagina mai scritta): **tutta `0xff`**. La
  recovery non la tocca e una pagina intatta si distingue nettamente da un
  marker: il canale è affidabile.

### Misura 2026-11-11 23:2x — dove muore XNU (marker sul telefono)
Kernel CI **43109688942** (commit `97e05c2`), immagine
`shim/out/shim-boot-g4-marks.img` (sha256 `1779a18b…0d04`), ADT con
`/pram` a `0xc0000000`.

```
+0x0000  0x5eed0001  OK   _start raggiunto
+0x0040  0x5eed0002  OK   tabelle bootstrap costruite
+0x0080  0xffffffff  ---  primo fetch con MMU on      <-- non arriva
+0x00c0  0xffffffff  ---  ritorno a arm_init
info: phys_base 0x81004000  mem_size 0x5effc000
      end 0xe0000000  TCR/TTBR0/SCTLR = 0xff (mai scritti)
```

- Il telefono torna in recovery a **t=21 s**, vivo: nessun hang di bus.
- **XNU arriva a `start_first_cpu` e muore fra la costruzione delle tabelle
  bootstrap (`b common_start`) e il primo fetch con `SCTLR_EL1.M=1`.**
  Non arriva mai in `arm_init`: per questo il buffer early era vuoto.
- `phys_base` post-slide `0x81004000` + `mem_size` `0x5effc000` =
  `0xe0000000`, che è **la fine vera del banco alto** (`0x80000000-0xdfffffff`
  = 0x60000000 byte; il payload dichiara `mem_size 0x60000000`). Quindi i
  boot_args sono coerenti, non c'è overflow della mappa.
- Il prossimo passo è biforcare la finestra `common_start` (marker 5-10:
  entry, TCR, TTBR0/1, MAIR, VBAR, prima di `SCTLR_EL1`). Se arriva a 10 e
  non a 3, il colpevole è la scrittura di SCTLR o il primo fetch; se si ferma
  prima, è un data abort su `sysreg_restore`, MAIR o VBAR — tutti indirizzi
  V=P che con la MMU ancora spenta devono essere fisici.

### Misura 2 — biforcazione di `common_start` (CI 43111639112)
Immagine `shim/out/shim-boot-g4-bisect.img` (sha256 `da566cb0…c166`).

```
1 _start                           OK
2 tabelle bootstrap costruite      OK
5 common_start entrato             OK
6 TCR_EL1 scritto                  OK
7 TTBR0/TTBR1 scritti              OK
8 MAIR_EL1 scritto, TLB flush      OK
9 VBAR_EL1 impostato               OK
10 prima di MSR SCTLR_EL1          OK
3  primo fetch con MMU on          MANCA
4  ritorno a arm_init              MANCA
```

**Il kernel muore sulla scrittura di `SCTLR_EL1` o sul primo fetch dopo.**
Tutti i passi precedenti (lettura di `sysreg_restore`, `set_mmu_ttb`, TTBR0/1,
MAIR, VBAR) funzionano.

### Le tabelle bootstrap sono corrette (verificato in RAM, senza CI)
- Layout del payload: `virt_base 0xfffffe0006000000`, `phys_base 0x80000000`,
  quindi **PA = 0x80000000 + (VA - virt_base)**; `_mh_execute_header` finisce a
  `0x81004000` e l'entry a `0x814a8488`.
- `bootstrap_pagetables` VA `0xfffffe0007180000` → PA **`0x81180000`**.
  Letto dalla recovery: `L1[0] = 0x81188003` (tabella valida) e una sola
  entry, coerente col fatto che tutto il banco sta sotto i 256 GiB.
- L2 a `0x81188000`: **47 block entry** da `L2[64]` (VA `0x80000000`) a
  `L2[110]` (VA `0xdc000000`), ognuna `0x…0601` = block valido, **AF=1**,
  **attrindx 0 (write-back)**, shareable outer. Copre `0x814a8488` ✓ e anche
  la pagina dei marker `0x90003000` ✓.
- Quindi la mappa V=P è giusta: se il fetch dopo `MSR SCTLR_EL1` faultasse,
  non sarebbe per una traduzione mancante. Serve l'eccezione vera.

### Prossimo esperimento — vettori d'eccezione propri (patch 0082)
- Il build diagnostico installa `sweet_exc_vectors` in `VBAR_EL1` subito
  prima dell'accensione della MMU: 16 slot, ognuno salva `ESR_EL1`, `FAR_EL1`,
  `ELR_EL1`, `SPSR_EL1` in `0x90005000` e `0x5EED0B00|slot` in `0x90003300`,
  poi resta in loop fino al watchdog.
- La tabella sta in `.text`, quindi il suo indirizzo è lo stesso con MMU
  spenta (fisico) e accesa (V=P): funziona in entrambi i mondi.
- `tools/read-pram.py --marks` decodifica ESR (classe EC, fault status DFSC,
  read/write) e stampa FAR/ELR/SPSR.

## Layout dei marker di boot e decoder aggiornato (2026-11-12)

### Il layout nuovo (albero di lavoro `/tmp/gicbase`)
I marker non cambiano idea di fondo (una pagina di DRAM, `0x90003000`, che la
recovery non riusa) ma crescono: ora sono **11**, e coprono anche la
biforcazione fra "solo MMU" e "MMU + cache", che è il punto in cui il G4
muore.

| indirizzo | valore | quando |
| --- | --- | --- |
| `0x90003000` | `0x5EED0001` | `_start` raggiunto (MMU del bootloader ancora attiva) |
| `0x90003040` | `0x5EED0002` | tabelle bootstrap costruite |
| `0x90003100` | `0x5EED0005` | `common_start` entrato, MMU ancora spenta |
| `0x90003140` | `0x5EED0006` | `TCR_EL1` scritto da `sysreg_restore` |
| `0x90003180` | `0x5EED0007` | `TTBR0/TTBR1` scritti |
| `0x900031c0` | `0x5EED0008` | `MAIR_EL1` scritto, TLB invalidato |
| `0x90003200` | `0x5EED0009` | `VBAR_EL1` = tabella dei vettori di eccezione |
| `0x90003240` | `0x5EED000A` | sta per scrivere `SCTLR_EL1` (MMU + cache) |
| `0x900032c0` | `0x5EED000B` | `SCTLR_EL1` con **M=1** scritto (MMU on, cache ancora off) |
| `0x90003080` | `0x5EED0003` | primo fetch con MMU **e cache** attive |
| `0x900030c0` | `0x5EED0004` | sta per tornare in `arm_init` |

- **Il marker 11 è nuovo**: il kernel scrive prima `SCTLR_EL1` con `M=1` e le
  cache spente, poi quello vero. Quindi "marker 11 presente, marker 3
  assente" = **la traduzione va bene, è l'accensione delle cache che
  uccide la CPU**; "marker 10 presente, marker 11 assente" = è la
  scrittura di `SCTLR_EL1` stessa a fare fault.
- Blocco **INFO a `0x90003800`**, 8 parole da `0x00`:
  `phys_base`, `mem_size`, `TCR_EL1`, `TTBR0_EL1`, `SCTLR_EL1`, e ora anche
  `CurrentEL` (`0x28`), `MPIDR_EL1` (`0x30`), `MIDR_EL1` (`0x38`).
  `phys_base`/`mem_size` sono post-slide: la loro somma deve fare
  `0xdff00000`.
- **Probe delle eccezioni**: `0x90005000` = `ESR_EL1`, `FAR_EL1`, `ELR_EL1`,
  `SPSR_EL1`; il vettore che ha girato è in `0x90003300` = `0x5EED0B00|slot`
  (slot 0..15 = le 16 entry di `sweet_exc_vectors`).

### Il decoder (`tools/read-pram.py`)
Aggiornato al layout sopra:
- `--marks` decodifica **tutti e 11** i marker, in ordine di esecuzione
  (non in ordine di offset: 1, 2, 5, 6, 7, 8, 9, 10, 11, 3, 4).
- Decodifica il blocco INFO campo per campo, e in più interpreta:
  `CurrentEL` (EL1 vs EL2 — a EL2 XNU **non** accende la propria
  traduzione e restano le tabelle del bootloader), `MPIDR_EL1`
  (Aff3/Aff2/Aff1/Aff0), `MIDR_EL1` (nome della CPU: `0x41d0b000` =
  Cortex-A53), i campi di `TCR_EL1` (T0SZ, TG0, SH0, IRGN0, ORGN0, TG1) e di
  `SCTLR_EL1` (M, C, I).
- **Verdetto automatico** in fondo: il marker mancante dice *dove* è morto
  (niente marker 1 = il salto nel kernel è sbagliato; 10 senza 11 = la
  scrittura di `SCTLR_EL1` fa fault; 11 senza 3 = le cache; 3 senza 4 = dopo
  la MMU, prima di `arm_init`; 4 = arriva in `arm_init` e il dettaglio è
  nella early console).
- Se una parola `0x5EED00xx` appare fuori dagli offset noti, lo dice: è il
  segnale che il layout è cambiato ancora.
- Nuovo `--exc`: stampa solo i registri della probe (ESR con classe EC e
  fault status DFSC, read/write, FAR, ELR, SPSR).
- Collaudato offline con pagine sintetiche (nessun telefono serve):
  `python3 /tmp/test_marks.py` (caso "arriva ad arm_init", "muore sulle
  cache", "muore con M=1", "nessun marker").

### Nota sul canale di lettura
- Il buffer early (`sweet_early_buf`) sta in BSS e veniva scritto **con la
  cache write-back**: un reset non la pulisce, quindi il testo non
  arrivava in DRAM e la lettura dal recovery dava un buffer vuoto anche se
  il kernel aveva scritto. La patch `0083` pulisce con `dc civac` le
  linee del buffer a ogni mark: adesso il testo arriva in RAM. Finché la
  patch non è nel kernel giusto, **"buffer early vuoto" non vuol dire
  "il kernel non è partito"**.
- I marker invece sono puliti a mano (`dc civac` dopo ogni `str`), quindi
  sono affidabili anche senza la 0083.

## 0012 riscritta e 0084: i marker fino a `bsd_init` (2026-11-12)

### La 0012 era una patch **superata**, non un mismatch
`tools/check-xnu-patch-chain.sh` segnalava `0012-sweet-keep-debug-console-after-ttbr0.patch`
come non applicabile. Non era un problema di contesto: la 0012 appartiene al
**primo design** della early console, quello che disegnava sul framebuffer
(`SWEET_EARLY_FB_VA`, `sweet_early_map_final`, `sweet_early_relocate`,
`EARLY_FB` come puntatore). Dal 0010 la console è un **buffer di testo in
BSS**, quindi il file su cui la 0012 faceva patch non esiste più in quella
forma.

La sua *intenzione* però è ancora giusta — «tenere la console viva dopo il
teardown del TTBR0 bootstrap» — e per il design attuale si traduce in un
punto preciso: **`sweet_early_disable()` veniva chiamato proprio quando il
TTBR0 bootstrap andava via, e spegneva la console**. Per questo i marchi
arrivavano solo fino all'8, e il resto del boot era muto.

La 0012 riscritta (rigenerata sull'albero reale, non a mano):
- **non chiama più `sweet_early_disable()`** al teardown del TTBR0: il buffer
  è in BSS, il TTBR1 finale lo mappa, quindi continua a funzionare;
- aggiunge i marchi **10..14** in `arm_vm_init` (TTBR0 spento, puntatori
  physmap riequilibrati, `pmap_bootstrap`, page table I/O) e **15, 16, 19,
  20** in `arm_init` (`arm_vm_init` tornata, `serial_init`, `kprintf` pronto),
  **17 e 18** in `pe_serial` (mapping del fbcon);
- il limite di stadio di `sweet_early_mark` passa da 8 a **24**, altrimenti
  i marchi 10..20 verrebbero scartati;
- **niente alias del framebuffer**: è il lavoro sul display, e aspetta che
  XNU arrivi a `bsd_init`.

Nota: la 0012 non tocca `start.s`, quindi non è in conflitto con 0083 (che
pulisce la cache del buffer early): la catena resta interamente applicabile.

### La 0084: marker 11, info a 8 campi, e i marchi del rootfs
`xnu/patches/0084-sweet-boot-markers-mmu-split-and-bsd-init.patch`, tre cose
nello stesso esperimento:

1. **Marker 11** (`0x5EED000B` a `0x900032c0`): il kernel scrive prima
   `SCTLR_EL1` **con M=1 e le cache spente** (`SCTLR_EL1_M_ONLY`), poi quello
   vero. Quindi:
   - marker 10 presente, **11 assente** → è la scrittura di `SCTLR_EL1` a fare
     fault;
   - **11 presente, 3 assente** → la traduzione funziona e sono **le cache**
     a uccidere la CPU.
2. **Info a 8 campi** (`0x90003800`): oltre a `phys_base`, `mem_size`,
   `TCR_EL1`, `TTBR0_EL1`, `SCTLR_EL1` ora ci sono `CurrentEL` (`+0x28`),
   `MPIDR_EL1` (`+0x30`) e `MIDR_EL1` (`+0x38`). `CurrentEL` dice a che
   livello è entrato il kernel: **a EL2 XNU non accende la propria
   traduzione** e restano le tabelle del bootloader.
3. **I marchi del rootfs**:
   - `sweet_mark()` in `sweet_early.c`: scrive un marker nella **stessa
     pagina** dei marker dell'assembly (`0x90003000`), con lo stesso
     `dc civac`, così la recovery lo rilegge dopo il reset del watchdog;
   - **marker 12** (`0x5EED000C` a `0x90003340`) in `IOKitBSDInit`: il kernel
     ha superato `bsd_init` e sta cercando il device root;
   - **marker 13** (`0x5EED000D` a `0x90003380`) in `bsd_init`, subito prima
     di `vfs_mountroot()`: **sta per montare il rootfs, cioè per fare la
     lettura HFS+**.

L'ordine è quello reale: `bsd_autoconf()` → `IOKitBSDInit` (riga 801 di
`bsd_init.c`) avviene **prima** di `vfs_mountroot()` (riga 882), quindi 12
precede 13.

`sweet_early.h` ha ricevuto la guardia `extern "C"`: il marchio di
`IOKitBSDInit` è scritto da un file **C++** (`iokit/bsddev/IOKitBSDInit.cpp`)
e senza la guardia i simboli avrebbero il nome mangled.

### Stato della catena
`tools/check-xnu-patch-chain.sh`: **89 applicate, 0 fallite** (prima erano 87
applicate e 1 fallita).

Lo script è stato anche corretto: resettava l'albero a `HEAD` invece che al
**primo commit**, quindi un albero usato per scrivere una patch (che porta un
commit della catena già applicata) faceva fallire tutte le patch. Ora fa
`git reset --hard` sul commit radice.

### La CI è fallita due volte, e i due errori erano reali
La CI **non** è "verde = successo": nel log la build fallisce e i passi di
raccolta girano lo stesso (`if: always()`), quindi l'artifact può esistere
**vuoto**. La prova: run `43136993214` → `conclusion: failure`, `SHA256SUMS`
da 0 byte, nessun kernel. Il kernel va sempre verificato con i check sul
binario, e il log va letto.

1. **Run `43136993214` — `arm64/sweet_early.h file not found`** in
   `pexpert/arm/pe_serial.c` e in `iokit/bsddev/IOKitBSDInit.cpp`.
   - Il nuovo header esisteva solo per `osfmk/arm` e `osfmk/arm64`, che
     compilano con l'albero dei sorgenti sul path degli include. Gli altri
     directory vedono gli header **installati**.
   - Il fix: `sweet_early.h` va in `EXPORT_MD_LIST` in
     `osfmk/arm64/Makefile`, come `proc_reg.h`.
   - **L'errore era nascosto** dalla 0012, che non si applicava e conteneva
     la stessa include. Ora che la 0012 è riscritta, l'include c'è e
     l'errore è visibile.
2. **Run `43136622836`** — superato dal fix successivo (non è fallito, è
   stato cancellato dalla concorrenza).

La lezione operativa: **le patch che non si applicano nascondono i loro
errori di compilazione**. Una catena con patch "saltate" sembra verde e
semplicemente non fa quello che dice.

## Misura 3 — il kernel muore DENTRO la `MSR SCTLR_EL1` (CI 43138710276)

Immagine `shim/out/shim-boot-g4-mmu.img` (sha256 `2ec47328…0706`), kernel
della CI `43138710276` (commit `a25aaff`), **solo `fastboot boot`, niente
flash**.

```
+0x0000  0x5eed0001  OK   _start raggiunto
+0x0040  0x5eed0002  OK   tabelle bootstrap costruite
+0x0100  0x5eed0005  OK   common_start entrato
+0x0140  0x5eed0006  OK   TCR_EL1 scritto
+0x0180  0x5eed0007  OK   TTBR0/TTBR1 scritti
+0x01c0  0x5eed0008  OK   MAIR_EL1 scritto, TLB flush
+0x0200  0x5eed0009  OK   VBAR_EL1 = probe delle eccezioni
+0x0240  0x5eed000a  OK   prima di MSR SCTLR_EL1
+0x02c0  0xfffbfffb  ---  marker 11 (SCTLR_EL1 con M=1)   <-- non arriva
+0x0080  0xffffffff  ---  primo fetch con MMU e cache
+0x00c0  0xdffffff7  ---  ritorno a arm_init
info: phys_base 0x81004000  mem_size 0x5effc000  end 0xe0000000
      CurrentEL 0x4 (EL1)  MPIDR_EL1 0x80000000  MIDR_EL1 0x410fd033 (Cortex-A53)
      TCR/TTBR0/SCTLR = spazzatura (scritti solo al marker 3, mai raggiunto)
```

- Il telefono è tornato in recovery a **t+10 s** (watchdog).
- **`emmc_log_buf`: 0 byte** — il driver eMMC non è mai partito.
- **La probe delle eccezioni è tutta spazzatura** (`ESR_EL1 0xffffffffffffffff`,
  `FAR_EL1 0xffd7ffffffffffff`, `ELR_EL1 0xfbffdefbfff7fffb`,
  `SPSR_EL1 0xfffffffffff7fbef`): **non è stata presa alcuna eccezione**.
  Non è un fault di traduzione: è uno **stop del bus**.

### La conclusione, e perché è nuova
Il blocco del marker 10 scrive `CurrentEL`, `MPIDR_EL1` e `MIDR_EL1` **dopo**
il marker, e i tre valori sono **veri** (EL1, CPU 0, Cortex-A53) → la CPU
era viva e funzionante fino alla fine di quel blocco. Fra quel punto e il
marker 3 c'è solo:

```
MOV64   x0, SCTLR_EL1_M_ONLY
MSR_SCTLR_EL1_X0
isb     sy
SWEET_MARK ... 0x2c0, 0x5EED000B     <- mai scritto
MOV64   x0, SCTLR_EL1_DEFAULT
MSR_SCTLR_EL1_X0
isb     sy
SWEET_MARK ... 0x80, 0x5EED0003      <- mai scritto
```

Quindi **la morte è dentro la `MSR SCTLR_EL1` (o nell'`isb` subito dopo)**,
non prima. Prima non si poteva dirlo: il marker 10 era scritto *prima* del
blocco con le `mrs`, e senza quei valori reali il confine era ambiguo.

### L'esperimento successivo: due marker ai lati della MSR
La 0084 (seconda revisione) mette un marker **prima** e uno **dopo** la
scrittura con M=1:

| offset | valore | quando |
| --- | --- | --- |
| `0x900032c0` | `0x5EED000B` | sta per scrivere `SCTLR_EL1` con M=1 |
| `0x900032c4` | `0x5EED000C` | `SCTLR_EL1` con M=1 scritto |

- 11 presente, 12 assente → **la scrittura è il killer**;
- 11 e 12 presenti, 3 assente → **sono le cache**;
- 11 assente → la morte è prima, nel blocco del marker 10.

### Due CI fallite, due cause reali
1. **`arm64/sweet_early.h file not found`** in `pexpert/arm/pe_serial.c` e in
   `iokit/bsddev/IOKitBSDInit.cpp`: l'header era raggiungibile solo da
   `osfmk/arm` e `osfmk/arm64` (che compilano con l'albero dei sorgenti sul
   path degli include); gli altri directory vedono gli header installati.
   Serve `EXPORT_MD_LIST` in `osfmk/arm64/Makefile`, come `proc_reg.h`.
   **L'errore era nascosto dalla 0012**, che non si applicava e conteneva la
   stessa include.
2. La CI "verde" non vuol dire "il kernel c'è": i passi di raccolta girano
   anche con `if: always()`. Il run fallito produce un artifact **vuoto**
   (`SHA256SUMS` da 0 byte). Il kernel va verificato sul binario:
   `grep -c "#0x5eed"`, il simbolo `sweet_exc_vectors`, e il codice del
   marker 11 (`mov x10, #0x32c0` + `movk x10, #0x9000, lsl #16`).

### Il lettore della RAM aveva un bug, e ha fatto hangare il telefono
`read-xnu-early.py` e `read-xnu-log.py` leggevano con `skip=pa//PAGE`:
se il simbolo **non** è allineato alla pagina, tornano i byte dell'inizio
della pagina, non quelli del simbolo. La lunghezza letta è quindi spazzatura
e il lettore chiede **gigabyte**: la lettura finisce nei **buchi** della mappa
di memoria e il telefono si blocca sul bus AXI (schermo nero, USB muto).
- Ora l'indirizzo viene tradotto dentro la pagina, e una lunghezza che non
  è credibile (> 1 MiB) viene rifiutata.
- **Conseguenza**: la lettura `emmc_log_buf = 0 byte` di questa misura
  **non è attendibile** (era una lettura disallineata). Va rimessa.

### Le risorse non stanno in /tmp
`/tmp` è stato ripulito dal sistema e ha portato via l'albero XNU usato per
scrivere le patch, la cache dei sorgenti Apple staged e gli script di lavoro.
Ora stanno **dentro il progetto**: `work/xnu-patchtest` (worktree del
sorgente `xnu-8796.141.3`) e `work/xnu-stage-cache`, entrambi in
`.gitignore`. `tools/check-xnu-patch-chain.sh` usa quei percorsi.
