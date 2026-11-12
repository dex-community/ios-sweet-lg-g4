# HANDOFF — iOS 16.7.16 su LG G4 H815

Sei l'AI che riprende un progetto a metà. Questo documento è l'handoff completo:
tutto quello che segue è **misurato sul telefono reale**, non dedotto. Non
ritentare le strade marcate come morte. Lavora in **italiano**.

---

## 1. Il progetto

Portare lo userspace iOS 16.7.16 (20H392) dell'iPhone X su un **LG G4 H815**
(MSM8992, Snapdragon 808: 4× Cortex-A53 + 2× A57, 3 GiB, bootloader LK).
Il rootfs iOS è GIÀ scritto sulla partizione `userdata` (mmcblk0p50) ed è
stato verificato con `fsck_hfs` sul Mac: obiettivo è LEGGERLO, mai riscriverlo.

- Repo di partenza: `dipa-studio/ios-sweet` (XNU su Xiaomi sweet SM7150).
- Repo di lavoro: `/Users/io/Dev/ios/ios-sweet-lg-g4`
  (GitHub: `dex-community/ios-sweet-lg-g4`, branch `main`).
- Riferimenti: `shrimpapplepro/UniXNU-lumia950xl` (driver eMMC MSM8992),
  `turgayasuk-sketch/ios-topaz` (display).
- Target finale: lock/home screen iOS con touch e tasti funzionanti.
- Ordine delle priorità: **eMMC prima** (blocca XNU), poi display, poi input.

Stato macro del progetto: lo shim bare-metal gira sul telefono e XNU arriva al
suo entry point (11 segmenti Mach-O caricati, jump a `__start`); manca il
driver eMMC per XNU, il display (MDP/DSI command mode) e l'input.

## 2. Stato attuale (al 2026-11-12)

> **AGGIORNAMENTO 2026-11-12 (tardo) — LA CAUSA È LA GRANA DELLE PAGINE:
> IL G4 NON HA LA 16 KiB, IL KERNEL VA COMPILATO A 4 KiB.**
>
> La morte all'accensione della MMU **non è un bug del codice**: è una
> **carenza dell'hardware**. Il Cortex-A53 del G4 è Armv8.0 e implementa la
> grana **4 KiB** e **64 KiB**, ma **non** la **16 KiB** (servirebbe
> `FEAT_LPA2`, che è Armv8.2, come sull'A75/A76 della board sweet
> originale).
>
> **La prova, dal binario e dal runtime:**
>
> | | valore | T0SZ | TG0 | T1SZ | TG1 |
> |---|---|---|---|---|---|
> | binario (`_sysreg_restore`) | `0x226511a511` | 17 | `0b10` = 16 KiB | 17 | `0b01` |
> | letto a runtime sul telefono | `0x2265112511` | 17 | `0b00` = **4 KiB** | 17 | `0b01` |
>
> Differenza = `0x8000`: **solo il campo TG0**. Il kernel *chiede* 16 KiB,
> il core *risponde* 4 KiB (comportamento implementation-defined dell'ARM
> ARM: DDI0487, `TCR_EL1`). Da lì le tabelle bootstrap, costruite per 16
> KiB (`ARM_16K_TT_L*_SHIFT`, `PGBYTES = 0x4000`), vengono percorse con
> gli shift sbagliati → indirizzo fisico spazzatura → il primo fetch dopo
> `MSR SCTLR_EL1` va in bus hang. E **non c'è eccezione**, perché il
> vettore è mappato dalle stesse tabelle rotte: da qui marker 11
> presente, 12 assente, `emmc_log_buf` a zero byte.
>
> **Correzione di una nota storica**: la riga "ID_AA64MMFR0 = ...101122:
> 16K granule YES" era una **mis-decodifica**. Nelle diagnostiche dello
> shim le etichette dei campi erano **spostate di un campo**: la mappa
> vera è `TGran4 [31:28]`, `TGran64 [27:24]`, `TGran16 [23:20]`, e la
> "TGRAN 16k=" che avevamo letto era in realtà **BigEndEL0**. Il
> `TGran16` vero del G4 è **0**. (Le diagnostiche e il decoder sono
> corretti; `mmu_granule_ok()` nello shim era invece **giusto**, ed è lei
> che faceva saltare la MMU dello shim sul G4.)
>
> **Il fix è `xnu/patches/0085-sweet-4k-granule.patch`**:
> 1. `SWEET.h`: via `__ARM_16K_PG__`, dentro `__ARM64_PMAP_SUBPAGE_L1__`
>    (è la configurazione di **BCM2837**, l'unica altra board ARM64 a
>    4 KiB che XNU supporta): `T0SZ_BOOT = 26`, spazio virtuale di 38 bit.
> 2. `MakeInc.def`: per `CURRENT_MACHINE_CONFIG = SWEET`,
>    `-UARM_LARGE_MEMORY` e `ARM64_LINK_ADDR = 0xfffffff007004000`.
>    **Obbligatorio**: con 4 KiB `T0SZ_BOOT` è 25/26 (39/38 bit) e il
>    layout `ARM_LARGE_MEMORY` (48 bit, link `0xfffffe0007004000`)
>    metterebbe il kernel **fuori** dallo spazio raggiungibile. Si passa al
>    layout a 37 bit, quello di BCM2837.
>
> La **mappa fisica non cambia** (entry a PA `0x814a8488` come prima):
> cambia solo la finestra virtuale (47 → 38 bit).
>
> **Da controllare nel nuovo binario**: `TCR_EL1` = `0x002a51a161a`
> (T0SZ 26, **TG0 `0b00` = 4 KiB**, T1SZ 26, TG1 `0b10`, IPS 40 bit),
> entry VA `0xfffffff0074a8488`, e il **marker 12** che deve comparire.
> Dettaglio completo in NOTES.md, sezione "La grana 16 KiB non esiste
> sul G4".

> **AGGIORNAMENTO 2026-11-12 — DOVE SI FERMA XNU: SULL'ACCENSIONE DELLA MMU.**
>
> Misurato sul telefono con i marker in RAM (patch `0082`, `tools/read-pram.py
> --marks`): il kernel arriva a **`common_start`**, scrive `TCR_EL1`,
> `TTBR0/TTBR1`, `MAIR_EL1`, `VBAR_EL1` (marker 5→9) e **muore sulla
> scrittura di `SCTLR_EL1` o sul primo fetch dopo** (marker 10 presente,
> marker 3 assente). Le tabelle bootstrap sono state **lette dalla RAM e
> verificate corrette** (47 block entry, AF=1, attrindx 0 write-back,
> coprono sia l'entry del kernel sia la pagina dei marker). Dettaglio in
> NOTES.md, sezione "Misura 2" e "Le tabelle bootstrap sono corrette".
>
> **Il layout dei marker è cresciuto** (albero di lavoro `/tmp/gicbase`, non
> ancora trasformato in patch nel repo):
> - 11 marker invece di 10: il nuovo **marker 11 (`0x900032c0`,
>   `0x5EED000B`)** dice che `SCTLR_EL1` è stato scritto **con M=1 e le
>   cache ancora spente** — distingue "la traduzione fa fault" da "sono le
>   cache a uccidere la CPU".
> - Blocco INFO a `0x90003800` con 8 parole: `phys_base`, `mem_size`,
>   `TCR_EL1`, `TTBR0_EL1`, `SCTLR_EL1`, `CurrentEL`, `MPIDR_EL1`, `MIDR_EL1`.
> - Probe delle eccezioni: `ESR/FAR/ELR/SPSR` a `0x90005000`, vettore in
>   `0x90003300` (`0x5EED0B00|slot`).
>
> **Il decoder è aggiornato**: `tools/read-pram.py --marks` decodifica tutti i
> marker, il blocco INFO (campo per campo, con `MIDR` → nome della CPU) e la
> probe delle eccezioni, e chiude con un **verdetto** ("muore con M=1", "muore
> sulle cache", "muore dopo la MMU prima di arm_init", …). Nuovo
> `tools/read-pram.py --exc` per i soli registri della probe.
> `--marks` resta l'unico modo sicuro di leggere la RAM: la guardia sui
> window RAM del G4 rifiuta le letture fuori dalla DRAM (hang del bus AXI).

> **AGGIORNAMENTO 2026-11-11 — IL KERNEL XNU ORA È COMPILABILE PER LA CPU DEL G4.**
>
> Il crash all'avvio di XNU era causato da istruzioni **LSE** (ARMv8.1
> Atomic Extension) e dal registro **PAN**: il G4 ha Cortex-A53/A57 =
> **ARMv8.0**, che non hanno né l'uno né l'altro. Tre fix, tutti veri:
>
> 1. `xnu/patches/0005b-sweet-commpage-llsc.patch` → `casa` nella commpage
>    diventa un loop LL/SC (`ldaxr`/`stlxr`).
> 2. `xnu/patches/0010b-sweet-no-pan.patch` → gli accessi a `pan` sono
>    stubbati (letture → 1, scritture → no-op). Non si abilita `+pan`:
>    l'hardware non ha il registro.
> 3. `libfirehose_kernel.a` (non è una patch: è il workflow) → slice arm64
>    solo, con `-mcpu=cortex-a53+crypto`. **Attenzione: `-march` NON basta**,
>    il triple `arm64-apple-macos15` implica `+lse`; solo `-mcpu` resetta le
>    feature.
>
> Run CI `43080427388`: **verde**, kernel Mach-O arm64, **0 istruzioni LSE**,
> 2226 LL/SC. Boot sul telefono: lo shim carica 10 segmenti e **salta a XNU**
> (`JUMP 00000000814A8488`), poi il watchdog riporta in recovery.
> **Non ancora stabilito dove si fermi XNU**: i ring in RAM
> (`sweet_early_buf`, console ring) erano vuoti quando li ho letti, perché
> la recovery aveva già riusato la RAM. Va catturato subito dopo il crash.
>
> **Documentazione completa e procedura replicabile:
> `docs/LSE-PAN-FIX-2026-11-11.md`** (problema, patch, pre-test locale,
> check 4, build, boot, come leggere i log).
>
> **Pre-test locale (nuovo, senza aspettare la CI):**
> `bash tools/local-pretest.sh` → ricompila in locale, con le flag esatte
> del CI, le TU toccate dalle patch e conta le LSE. Richiede prima
> `gh run download <RUN> -n xnu-buildtree -D /tmp/ci-buildtree`.

## 2b. Stato precedente (al 2026-11-10, 19:00)

> **AGGIORNAMENTO 19:00 — i due blocchi sono chiusi.**
> - CMD2 passa: `C2OK v=00000009 rsp=48B72E52` (136+CRC, il tipo del driver
>   Linux); CMD3/CMD7/CMD16 ok, `EMMC sec=03A3E000`, scritture disattivate.
> - La patch HFS+ funziona **sul telefono**: TWRP monta `userdata` da sola,
>   read-only (`hfsplus: no backup volume header ... primary used alone`),
>   ed e' il rootfs iPhone OS 16.7.16 (20H392).
> - Evidenze: `docs/test-2026-11-10-hfs-mount.txt`; procedura:
>   `docs/DEVICE-TEST.md` (`tools/device-test.sh`, un boot + un file).
> - Il kernel di test ha `-dirty` (patch applicata). Telefono attualmente
>   avviato con quel kernel + ramdisk TWRP; nulla e' stato flashato.
> - Prossimo step: **display (RGB2/INTF2)**; poi il driver eMMC per XNU.

- **Telefono**: LG H815, seriale `b448b72e`, bootloader sbloccato, **in TWRP
  recovery (TeamWin) con ADB attivo** (`adb devices` → `LGH815b448b72e recovery`).
- **Niente è stato flashato**: ogni test è `fastboot boot` (temporaneo).
  La partizione `boot` contiene il boot image del progetto (non lo stock:
  lo stock è in `backup/boot-stock.img`, ripristinabile via LAF se serve).
- **Commit corrente**: `ce5c04d` ("shim: sweep the CMD2 response type…"),
  committato IN LOCALE, **NON pushato** → la CI NON è partita. Push quando
  vuoi far girare la CI.

## 3. I due filoni aperti

### 3.1 eMMC: la carta risponde, manca UN comando

Il log dello shim del 2026-11-10 è in `docs/shim-log-2026-11-10.txt`:

```
CMD5 broadcast  rc=FFFFFFFD int=00018000   (timeout: ok, la carta non
                                           risponde all'indirizzo broadcast)
CMD5 con RCA 2  rc=00000000  rsp=00001400
CMD7 select     rc=00000000  rsp=00000700
CMD0 (idle)     rc=00000000
CMD1 x 37       rc=00000000  rsp=40FF8080 → 00FF8080 … → C0FF8080
CMD2 (SEND_ARG) rc=FFFFFFFD  int=00088000  rsp=48B72E52   <-- FALLISCE
```

Fatti stabiliti:
- La carta è una **Samsung 032G74**, CID `11010030333247373400b448b72e5200`,
  RCA 2 (letti dal sysfs della recovery).
- `FFFFFFFD` = -3 = **interrupt di errore** (non timeout; -2 sarebbe timeout).
- `int=00088000` = bit 15 (error summary) + bit 19.
- `rsp=48B72E52` contiene i byte del CID del boot precedente: è traccia
  residua, non risposta.
- Confronto con il driver Linux CHE FUNZIONA SU QUESTO TELEFONO
  (`G4-Titan-Kernel`, `drivers/mmc/host/sdhci.c` +
  `include/uapi/linux/mmc/core.h`):
  ```
  MMC_RSP_R1 = PRESENT|CRC|OPCODE  → 48 bit + CRC7 + index
  MMC_RSP_R2 = PRESENT|136|CRC     → 136 bit + CRC7  (SENZA index!)
  MMC_RSP_R3 = PRESENT             → 48 bit, senza CRC7 e senza index
  ```
  Il core usa `MMC_RSP_R2` per CMD2 e `MMC_RSP_R1` per CMD3. Lo shim
  programmava CMD2 come 136+CRC+index: **l'index check è il sospetto**.

**FATTO (verificato sul telefono, 19:00)** — `C2OK v=00000009` al primo
tentativo: CMD2 vuole **136 bit + CRC, senza index** (`MMC_RSP_R2`), come il
driver Linux. Il loop CMD1 aspetta il bit 31 (busy) e arriva a `C0FF8080`.
CMD3 -> CMD7 -> CMD16 completano, `EMMC sec=03A3E000`, writes off. Lo sweep
resta nel codice ma non serve piu': la combinazione vincente e' la prima.

Il fix (storico):
- `shim/emmc.c`: `emmc_cmd()` è stata scomposta in `emmc_cmd_flags()`, che
  prende i flag del registro comando così come sono; `emmc_cmd_log()` stampa
  ora TUTTE e quattro le parole della risposta (0x0c/0x10/0x14/0x18), perché
  il driver Linux legge l'OCR da 0x0c e la RCA da 0x18 e lo shim leggeva solo
  0x10 — non si sa quale il controller riempia.
- `shim/emmc_lk_bringup.inc`: l'identificazione è uno **sweep**: per ogni
  round (1 o 2 CMD1) prova in ordine `136+CRC` (come il driver Linux),
  `136+CRC+index` (come prima), `48+CRC`, `48 senza CRC`. Nel log le righe
  `C2TRY v=…` e `C2OK v=… rsp=…` dicono quale combinazione passa. CMD1 si
  ferma sul bit 30 dell'OCR (power-up, per la spec) e non sul bit 31.
- Immagine pronta: `shim/out/shim-boot.img`, sha256
  `c5622cc7040502071ca534a06cb05752d28af936e1591644c5c3cf2d6f13455a`,
  audit MMIO verde. Lanciata alle 16:17 via `fastboot boot`.
- **DA FARE SUBIO: leggere il log di quel run** (vedi §4 ciclo). Cerca
  `C2OK`: la combinazione che passa dice come programmare CMD2.

### 3.2 Rootfs iOS: RISOLTO — la patch del backup volume header funziona

> **VERIFICATO SUL TELEFONO (19:00):** con la patch il volume monta
> read-only, TWRP lo mette su `/data` da sola, `SystemVersion.plist` dice
> iPhone OS 16.7.16 (20H392) e `/usr/lib/dyld` ha lo stesso sha256 letto dal
> mount e dal tool userspace sul device raw. Dettagli in
> `docs/test-2026-11-10-hfs-mount.txt`.

Misure vere fatte con `dd` da TWRP su `/dev/block/mmcblk0p50`
(partizione: settori 12419072..61046783, 48.627.712 settori = 24,9 GB):

```
offset 1024: 48 58 00 05 00 80 00 21 48 46 53 4a ...
```
- firma **'HX'** = HFS+ **case-sensitive** (HFSX), versione 5;
- `lastMountedVersion = 'HFSJ'` a offset 1032 (è DENTRO il volume header,
  NON è il journal);
- attributi `0x00800021`: bit 0 = smontato pulito, bit 15 = **SOFTLOCK**;
- **block size 4096**, totalBlocks 4.480.000 (**18,35 GB** di volume utile),
  freeBlocks 1.551.163;
- catalog file 128 MB, primo extent al blocco 183691; allocation file
  561.152 byte al blocco 1;
- **journal info block a offset 1536: TUTTO ZERO** (il volume è marcato
  journaled ma il journal non c'è — CORREZIONE al FATTO 13 di NOTES.md);
- **backup volume header: NON ESISTE** — l'ultimo settore della partizione
  e la fine del volume sono tutti zero.

Perché non si montava: `fs/hfsplus/wrapper.c` (kernel Titan) legge il
backup, confronta la firma con il primario ('HX'), trova zero e rifiuta con
`hfsplus: invalid secondary volume header`. Il backup serve solo all'ioctl
di "bless" e al write-back; il driver usa il PRIMARIO per tutto.

**Patch già scritta e committata** (applica pulita, verificata con
`patch -p1 --dry-run`):
`kernel/patches/0001-hfsplus-mount-a-volume-with-no-backup-volume-header.patch`
— accetta il volume quando il backup NON c'è affatto; rifiuta ancora se il
backup c'è ma ha un'altra firma. La CI del kernel la applica con
`git apply` prima della build (step "Apply the patches" in
`.github/workflows/kernel-h815.yml`).

Il driver forza `MS_RDONLY` da solo su un volume journaled/locked
(`fs/hfsplus/super.c:349`), quindi il rootfs non si può rovinare per sbaglio.

## 4. Il ciclo diagnostico (la procedura che funziona)

```bash
cd /Users/io/Dev/ios/ios-sweet-lg-g4

# 1. avvia lo shim (CMD2 incluso, ~15 s poi si riavvia da solo)
fastboot boot shim/out/shim-boot.img

# 2. il telefono torna in TWRP recovery DA SOLO dopo ~3 minuti
#    (finestra: NON è rilevabile in fastboot durante il ciclo; aspetta l'ADB)
adb devices          # aspetta che torni "recovery"

# 3. un solo comando: pack + boot del kernel + batteria read-only
HFSPLUS=/path/to/hfsplus \
  tools/device-test.sh /tmp/kci/Image docs/test-$(date +%F).txt

# 4. leggi il file: cerca "HFS+ rootfs: OK" e, per lo shim, C2OK/EMMC
```

`tools/device-test.sh` fa tutto: se il telefono è già in fastboot non tenta
`adb reboot`; impacchetta Image gzip + ramdisk TWRP + dt.img; avvia con
`fastboot boot`; aspetta ADB; salva in un file `uname`, config runtime,
`/dev/mem`, mount/dmesg/df/ls del volume HFS+, `SystemVersion.plist`, gli
sha256 via mount e via `hfsplus`, e il log shim da `0x50000000`.
`--collect [file]` rifà la sola batteria su un kernel già avviato.
Dettagli: `docs/DEVICE-TEST.md`.


## 5. Prossimi passi in ordine

0. **Dove si ferma XNU?** (priorità 2026-11-11). Il kernel è pulito
   (0 LSE) e lo shim salta a XNU, ma i ring in RAM
   (`sweet_early_buf` PA `0x81c46dbc`, console ring in BSS) erano vuoti
   alla lettura: la recovery riusa la RAM. Il log dello shim si legge
   comunque dalla recovery:
   ```bash
   adb shell 'cat /sys/fs/pstore/console-ramoops-0' | tr -d '\r' | tail -30
   ```
   Per i ring di XNU: leggere **subito** dopo il crash con un kernel che ha
   `/dev/mem` (`tools/read-xnu-early.py`, `tools/read-xnu-log.py`), non dopo
   il riavvio in recovery. Poi aggiornare NOTES.md con l'ultima riga di
   progresso (processor_up / arm_init / bsd_init) e proseguire.
1. **Leggere il log dello sweep** (costo zero, §4). Se una `C2OK` c'è:
   fissare quel tipo di risposta nel codice (togliere lo sweep o lasciarlo
   con quell'ordine), aggiornare NOTES.md, committare.
2. **Pushare e far girare la CI del kernel** (`.github/workflows/kernel-h815.yml`):
   ora contiene lo step che applica `kernel/patches/*.patch`. Scaricare
   l'artifact (`gh run download <run-id> -n kernel-h815-hfsplus-devmem -D /tmp/…`),
   impacchettare con `tools/pack-twrp-kernel.sh` (kernel gzip + ramdisk TWRP
   + dt.img del telefono), bootare e provare:
   ```
   adb shell "mkdir -p /iosroot && mount -t hfsplus -o ro /dev/block/mmcblk0p50 /iosroot && ls /iosroot"
   ```
3. **Leggere il rootfs iOS** (ls/cat di `/iosroot`, dimensioni, verificare
   che sia il rootfs dell'iPhone X). NON scrivere MAI su `userdata`.
4. **Display** (secondo track): il pannello è pilotato dalla pipe **RGB2
   (offset `0x19000`) su INTF2** (MDP3), non VIG0. Misurato 2026-11-10:
   RGB2 SRC0_ADDR = `0x00E30000` **IOVA** (stride 5760, RGBA8888,
   1440×2560). Novità: il fetch passa per l'**MDP SMMU** (`mdp_iommu`
   @ 0xFD9B4000, ctx `mdp_0` @ 0xFD9BC000, SID 0); la strada "RAM fisica,
   niente SMMU" del piano vale solo se l'SMMU resta bypassato come l'LK lo
   lascia. Misure: `docs/display-2026-11-10.txt`,
   `docs/DISPLAY-FINDINGS.md`, NOTES FACT 21. Le misure si fanno dalla
   recovery con `/sys/kernel/debug/mdp/` e `/sys/kernel/debug/mdss_panel/dsi0/`,
   MAI con una sonda nello shim (l'MDSS è senza clock all'handover e una
   lettura MMIO hanga il bus). Prossimo: accendere i clock MDSS/DSI nello
   shim (fase D1) e ripuntare RGB2 (D3), oppure tenere l'SMMU bypassato e
   usare indirizzi fisici (strada più semplice).
5. **XNU con driver eMMC**: il riferimento è il driver di UniXNU-lumia950xl;
   `docs/g4-hardware/EMMC-REFERENCE.md` spiega cosa cambiare per il G4.

## 6. Indirizzi e costanti hardware (verificati)

- eMMC SDHCI: HC `0xf9824900`, CORE `0xf9824000` (blocco 0x800 byte).
- `HC+0x29` = SDHCI_PWR_CONTROL: registro **A BYTE** → sempre `rd8`/`wr8`
  (una lettura a 32 bit non allineata ha fatto data abort → bootloop).
- `HC+0x24` PRESENT, `+0x28` CTRL1, `+0x2c` CLK, `+0x30` INT_STATUS,
  `+0x34` INT_ENABLE, `+0x3c` CTRL2, `+0x40/44` CAPS, `+0x10c` VENDOR_SPEC,
  `+0x2f` SWRST; HC_RESPONSE block: 0x0c/0x10/0x14/0x18.
- CORE `+0x00` POWER, `+0x50` MCI_VER, `+0x78` HC_MODE (bit0 HC_MODE_EN,
  bit13 FF_CLK_SW_RST_DIS), `+0xdc/e0/e4/e8` PWRCTL.
- Vendor spec (`HC+0x10c`): `CORE_CLK_PWRSAVE` bit1, `CORE_HC_MCLK_SEL`
  mask `3u<<8` (default `2u<<8`), `CORE_HC_SELECT_IN_EN` `1u<<18`,
  `CORE_HC_SELECT_IN_MASK` `7u<<19`. Nel run: `mclk=2 sel=0 sen=0 pws=0`.
- RAM scratch log shim: `0x50000000` (96 pagine da 4 KiB).
- ramoops: console `0xFF80000`, pstore `0xFF00000` (1 MiB) — pstore
  leggerlo SOLO col TWRP TeamWin (il kernel NetHunter ha
  `CONFIG_PSTORE_CONSOLE` e lo sovrascrive appena parte).
- IMEM `0xfe80f000`: NON leggibile via `/dev/mem` (EFAULT, non è RAM).
- PSHOLD `0xfc4ab000`; watchdog: NON disattivarlo (60 s + auto-fallback
  `LK_RECOVERY_MODE` = la rete di sicurezza: il telefono torna da solo
  in recovery).
- Boot image funzionante: kernel gzip + ramdisk TeamWin
  (`backup/nh-parts/ramdisk.bin`, 13.805.965) + dt.img
  (`backup/nh-parts/dt.img`, 1.232.896, QCDT v3, board 0xb64 =
  "MSM8992 P1 GLOBAL COM"). Header: kernel 0x8000, ramdisk 0x2200000,
  tags 0x100, page 4096.

## 7. Comandi già pronti

```bash
# build shim (flag esatti)
cd shim && SHIM_EMMC_PROBE=1 SHIM_EMMC_MODEL=0 SHIM_HW_DIAG=1 SHIM_SD_INIT=0 \
  SHIM_SD_PAYLOAD=0 SHIM_SD_PROBE=0 SHIM_MMIO_SELFTEST=0 RAW_TIMING=1 \
  FINE_TIMING=1 DIAG_VARIANTS=0 DIAG_TIMING=0 ./build.sh
cd .. && python3 tools/check-mmio-writes.py   # deve dire: ok, 9 device windows

# impacchettare un kernel
gzip -9 -c Image > k.gz
./tools/pack-twrp-kernel.sh k.gz /tmp/twrp-h815-hfs.img
./tools/boot-test-kernel.sh /tmp/kci5/Image   # fa tutto: pack+boot+check

# boot + lettura log (§4)

# CI
gh run list -L 5
gh run download <run-id> -n kernel-h815-hfsplus-devmem -D /tmp/kci
```

## 8. Le regole di sicurezza (NON negoziabili)

1. **Mai scrivere su controller MMIO non inizializzati.** Ogni accesso passa
   per `mmio_rd32/mmio_wr32/mmio_rd8/mmio_wr8` (`shim/mmio.h`) con allowlist
   a runtime + audit statico (`tools/check-mmio-writes.py`) che DEVE restare
   verde. Un'entry nell'allowlist NON dimostra che il clock sia acceso:
   tre hang sono arrivati da probe su blocchi col clock spento.
2. **Non scrivere MAI su**: `misc`, `cache`, `aboot`, `sbl1`, `tz`, `rpm`,
   `hyp`, `persist`, `modem`, `laf`. E **mai riscrivere `userdata`** (il
   rootfs iOS si legge soltanto).
3. **Niente flash**: solo `fastboot boot` (temporaneo). `fastboot boot` NON
   scrive nulla.
4. **Mai staccare (unbind) un driver di un block device che ha un volume
   montato**: fatto il 2026-11-10 con `sdhci_msm` e `mmcblk0p49` montata su
   `/cache` → telefono crashato, stack USB wedged, riso solo staccando la
   batteria. Per le misure usa i tracepoint (`/sys/kernel/debug/tracing`,
   eventi `mmc/*`).
5. Watchdog e fallback LK_RECOVERY_MODE: non disattivarli.
6. Tasti fisici: SOLO Volume Su è affidabile (Power/Volume Giù difettosi).
   Per il Download Mode (LAF): telefono spento → Volume Su → USB.
   Attenzione: LAF e fastboot hanno lo STESSO descrittore USB
   (0x1004:0x6340, interfaccia 255/255/255 con endpoint bulk) — si
   distinguono solo dal protocollo che rispondono.
7. Aggiorna `NOTES.md` a ogni milestone (il diario è il primary artifact);
   committa il lavoro completato. `research/` non va committato; `backup/`
   è gitignored (contiene l'IMEI); dump partizioni, immagini Apple e tree
   scaricati restano fuori dal git.
8. Se un esperimento fallisce 10 volte di fila sullo stesso punto: fermati e
   chiedi.

## 9. Strade morte (NON ritentare)

- Kernel 3.10.84 (`nepheloid/nethunter_kernel_g4`): NON parte (nessuna
  traccia nel pstore). Il kernel giusto è il **Titan 3.10.108**
  (`aoleary/G4-Titan-Kernel`, branch `titan_bpf2`, defconfig `twrp_g4_defconfig`,
  toolchain GCC 4.9 AOSP `android-5.0.0_r4`, build su ubuntu-22.04 con
  python2 per `scripts/gcc-wrapper.py`, gcc-9 per gli host tool,
  patch `yylloc` per dtc/bison 3).
- `VARIANT_DEFCONFIG`/`EXTRA_DEFCONFIG` in questi tree: opzioni morte
  (esportate ma mai lette).
- Il "limite di 28 MB" di `fastboot boot` NON esiste (immagini da 40 MB
  accettate; i fallimenti passati erano per ramdisk/corruzione).
- `fastboot reboot` da fastboot resta in fastboot.
- Pstore col kernel NetHunter: sovrascritto (`CONFIG_PSTORE_CONSOLE`).
- `/dev/mem` su IMEM 0xfe80f000: EFAULT.
- `mount -t hfsplus` SENZA patch: "invalid secondary volume header" (la
  patch è la cura).
- `git clone https://github.com/slebnik/libfsap.git` e
  `planetbeing/hfsplus`: 404. Per i tool HFS+ userspace la strada scelta
  è `planetbeing/libdmg-hfsplus` (target CMake `hfsplus`); la CI
  `hfs-tool` è VERDE (run 42846850104, e7e9e67).
- Tenere premuto Volume Su quando lo stack USB è wedged: non fa nulla;
  serve staccare la batteria.
- Scrivere su controller senza clock: hang del bus.
- **2026-11-11, LSE/PAN (non ritentare):**
  - `-march=armv8-a+crypto` **NON** toglie le LSE: il triple
    `arm64-apple-macos15` implica `+lse`. Serve `-mcpu=cortex-a53+crypto`.
  - `-mno-lse` non esiste per clang/AArch64.
  - in xcconfig `$(inherited)` non funziona (è sintassi make): va riscritto
    l'elenco completo dei flag.
  - `+pan` sull'hardware del G4: il registro non esiste, `msr pan` traperebbe.
  - il regex `\bldadd\b` non matcha `ldaddal`/`ldaddh`/`ldaddl`: usare
    `\b(ldadd|casa|ldclr|ldset|swp)[a-z]*\b`, altrimenti il kernel sembra
    pulito quando non lo è.
  - la CI **non** segnala i patch che non si applicano (`build.sh` fa
    `git apply --check` e va avanti con un warning): usare
    `tools/check-xnu-patch-chain.sh`.
  - la build XNU completa **non** si fa in locale: servono KDK 13.5
    (sudo) e SDK ≤ 15 (in locale c'è solo Xcode 27).

## 10. File chiave (tutti nel repo)

| File | Cosa |
| --- | --- |
| `shim/emmc.c` | driver eMMC shim; `emmc_cmd_flags()` (nuovo), `emmc_cmd_log()` stampa 4 parole risposta; costanti `CMD_RESP_48_NOCRC/48_CRC/48_CRC_IDX/136_CRC/136_CRC_IDX` e `HC_RESP0..3` |
| `shim/emmc_lk_bringup.inc` | replay dell'init LK; sweep CMD2 (`C2TRY`/`C2OK`); power/vendor sequence; `rd8/wr8` su HC+0x29 |
| `shim/emmc_probe.c` | probe E0 (solo letture; HC+0x29 con rd8!) |
| `shim/mmio.h` | allowlist MMIO + accessori |
| `docs/shim-log-2026-11-10.txt` | il log completo dell'ultimo run |
| `kernel/patches/0001-hfsplus-…patch` | la patch del backup volume header |
| `.github/workflows/kernel-h815.yml` | CI kernel (applica le patch, builda Titan) |
| `.github/workflows/hfs-tool.yml` | CI tool HFS+ userspace (verde) |
| `tools/pack-twrp-kernel.sh` | kernel + ramdisk TWRP + dt.img → boot.img |
| `tools/boot-test-kernel.sh` | pack + boot + check automatici |
| `tools/read-shim-log.py` | legge 0x50000000 via adb |
| `tools/read-pram.py` | **leggere la RAM del telefono in modo sicuro**: `--marks` decodifica i marker di boot (11), il blocco INFO a `0x90003800` e la probe delle eccezioni a `0x90005000`, e dice dove è morto il kernel; `--exc` per i soli registri; la guardia rifiuta le letture fuori dalle window RAM del G4 (hang del bus) |
| `tools/local-pretest.sh` | **pre-test locale**: ricompila in locale le TU toccate dalle patch con le flag del CI e conta le LSE (0 = ok) |
| `tools/check-xnu-patch-chain.sh` | applica tutta la catena `xnu/patches/` su un xnu-8796 pulito e dice quali patch NON si applicano (la CI non lo segnala!) |
| `docs/LSE-PAN-FIX-2026-11-11.md` | **la raccolta del 2026-11-11**: LSE/PAN, le patch, il firehose, il pre-test, la procedura completa |
| `xnu/patches/0005b-sweet-commpage-llsc.patch` | `casa` → LL/SC nella commpage (LSE) |
| `xnu/patches/0010b-sweet-no-pan.patch` | stub degli accessi al registro `pan` (ARMv8.1) |
| `tools/check-mmio-writes.py` | audit MMIO (deve restare verde) |
| `NOTES.md` | il diario: 17 "FATTO" documentati, LEGGILO prima di toccare codice |
| `STATUS.md`, `README.md`, `GUIDE-*.md` | stato e procedure |
| `backup/` | boot-stock.img, TWRP TeamWin, nh-parts (gitignored) |
| `/tmp/twrp-h815-hfs.img` | kernel+DEVMEM già impacchettato (sul Mac) |
| `/tmp/kci5/Image` | Image del kernel CI corrente (sul Mac) |

## 11. Istruzioni di stile

- Verifica sempre se un errore è REALE o un'allucinazione: ricontrolla sui
  sorgenti/dati veri, metti in dubbio le conclusioni, annota le nuove
  conclusioni in NOTES.md e continua in parallelo.
- Una fase alla volta; il codice eMMC che funziona si tocca il meno possibile.
- Le misure vengono PRIMA delle ipotesi: ogni numero che scrivi in NOTES.md
  deve venire dal telefono, dal log o dai byte letti.
- Se il telefono scompare dall'USB: aspetta il ciclo completo (il fallback
  riporta in recovery da solo in ~3 minuti); se enumera ma è muto (fastboot
  non risponde), serve staccare la batteria.

Buon lavoro.
