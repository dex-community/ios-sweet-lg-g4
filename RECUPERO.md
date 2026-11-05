# RECUPERO — Stato del porting iOS 16 su LG G4 H815

Documento di passaggio. Contiene: dove siamo, cosa è stato verificato,
cosa è falso, il file da toccare, la logica, e come testare.

Ultimo aggiornamento: 2026-11-05.

---

## 0. Dove siamo, in una riga

Il driver eMMC **non è mai stato il problema**. Il problema era (ed è) un
**crash in una fase successiva del boot**, e il canale di log che usavamo
per diagnosticarlo era rotto, quindi tutte le letture precedenti erano
artefatti.

Stato attuale: **il shim crasha nella fase PSCI o GIC**, non nella fase
eMMC. La diagnosi è a un passo dalla certezza.

---

## 1. Repo GitHub (rollback)

Il codice è versionato e pushato, ogni commit è un punto di rollback:

```
https://github.com/dex-community/ios-sweet-lg-g4
```

Commits (dal più recente):

| commit | cosa contiene |
|---|---|
| `f54f681` | diagnosi: il crash era nel percorso di panic; `emmc_first_*`; panic handler non riscrive sulla card; contatore `emmc_step` |
| `583b02c` | base: shim LK, driver eMMC v2 (Fix A), guide 00-03, dump del LK di fabbrica |
| `45263be` | base di `dipa-studio/ios-sweet` (upstream, non modificato) |

Comandi:

```sh
cd /Users/io/Dev/ios/ios-sweet-lg-g4
git log --oneline
git diff f54f681          # cosa hai cambiato dopo l'ultimo commit
git checkout f54f681      # torni indietro
git push github main      # pubblichi
```

Il remote `origin` è il clone locale (`/Users/io/Dev/ios/ios-sweet`),
`github` è la repo su GitHub. Non spingere su `origin`.

---

## 2. Il problema reale (la scoperta di oggi)

### 2.1 Il canale del tempo era rotto

L'unico canale di log disponibile era **il tempo che il telefono resta
prima di riavviarsi**. Misurato dal PC: da `fastboot reboot` al rientro in
recovery.

Due build identiche tranne l'attesa forzata:

| build | attesa imposta | tempo misurato |
|---|---|---|
| `FORCE_WAIT_S=5` | 5 s | **40,2 s** |
| `FORCE_WAIT_S=60` | 60 s | **95,1 s** |

Differenza = 54,9 s ≈ 55 s. **Il canale funziona**, e la costante di
boot (LK + TWRP) è **~35,2 s**.

### 2.2 Ma il tempo letto era sempre 15 s

Con la v2 del driver il tempo totale era 50,1 s = 35,2 + **15,0 s**.

15 s è esattamente il valore con cui `exc_handler()` chiama `reset(15)`
(`shim/shim.c:374`). **Il shim andava in fault** e il panic handler faceva
reset.

La prova che non era il canale dello stadio: ho messo `STAGE_START=90` e
`STAGE_PROBE=91` come sentinella e il tempo **non è cambiato di un
secondo**. Uno stadio non letto non produce un tempo costante: produce
zero. Il tempo era il `reset(15)` del panic, non il canale.

**Conseguenza: tutte le letture precedenti ("stadio 2", "stadio 4",
"errore 0", le varie prove di IO-pad e di clock GCC) erano interpretazioni
sbagliate di un numero che non variava.** Non erano il driver a fallire:
era il driver a **crashare**, e il crash veniva letto come un "errore
misterioso".

### 2.3 Il crash era nel percorso di panic, non nel driver

`reset()` chiama `emmc_write_log()` per scrivere il log sulla card. Se il
crash viene dal driver eMMC, la seconda chiamata crasha di nuovo →
`exc_handler()` con `in_exc=1` → `reset(5)` → `emmc_write_log()` → crash →
ricorsione infinita.

Fix applicato: in presenza di un fault, `reset()` **non** scrive più sulla
card (`shim/shim.c:214`, `panic_stage`).

### 2.4 Dove crasha, ora

Ho aggiunto un contatore di fase (`PHASE(n)` in `shim/shim.c`) e uno fine
(`emmc_step` in `shim/emmc.c`). Il panic handler mette
`phase * 20 + emmc_step` secondi nel tempo di attesa.

Ultima misura: **194,1 s totali** = 35,2 + **158,9 s**.

| fase | numero | cosa fa |
|---|---|---|
| boot | 1 | entry |
| emmc | 2 | `emmc_write_log()` |
| loader | 3 | cerca un payload XNU nel ramdisk |
| fill off | 4 | misura il fill della memoria |
| mmu enable | 5 | `mmu_enable()` |
| fill on | 6 | misura il fill con MMU attivo |
| **psci** | **7** | `smc(PSCI_VERSION)` / `smc(SMCCC_VERSION)` |
| **gic** | **8** | `test_gic()` |
| fdt | 9 | dump del device tree |
| done | 10 | repaint e reset |

158,9 s ⇒ **fase 7 (psci) o fase 8 (gic)**. `emmc_step` non è
determinante perché a quel punto il driver eMMC ha già finito.

**Da verificare per primo:** se il crash è in `smc()` (fase 7) o in
`test_gic()` (fase 8). Il sospetto principale è `smc()`: l'H815 non ha un
node `psci` nel DT ("psci is broken" nel port AOSP di h815) e le CPU usano
`enable-method = "qcom,8994-arm-cortex-acc"`, quindi la chiamata PSCI
potrebbe dare fault.

---

## 3. Cosa è stato verificato sul telefono reale (fatto)

- `adb shell dmesg` funziona **senza root** (kernel LG stock 3.10.108).
- La TWRP è adb **root**.
- Android è **8.1.0 Oreo** (fingerprint `lge/omni_g4/g4:16.1.0/OPM8.181105.002/23:eng/test-keys`), non Android 13 ⇒ SuperSU 2.46 ha buone probabilità.
- `ro.boot.ddr_size` = 3221225472 (3 GiB).
- `ro.boot.bootdevice` = `f9824900.sdhci` ⇒ l'host SDHCI è a `0xf9824900`.
- eMMC: `mmc0: new HS400 MMC card at address 0001`, `032G74 29.1 GiB`.
- `cma: Found secure_region@0, memory base 0x0, size 150 MiB` ⇒
  `cont_splash_mem` (0x03400000) è **dentro la regione protetta dalla XPU**:
  **il framebuffer non è scrivibile** da EL1 non sicuro. Il display richiede
  il driver MDP/DSI in command mode (Fase 4 del porting).
- Tabella partizioni reale (da `/sys/class/block/mmcblk0pN/start`):
  `spare1`=p21 start 259072 (32768 settori), `spare2`=p36 start 385024
  (8192 settori), `boot`=p38, `recovery`=p39, `system`=p47, `cust`=p48,
  `cache`=p49, `userdata`=p50, `misc`=p22, `devinfo`=p17, `aboot`=p8.
- `spare1` e `spare2` sono **verificate vuote** (tutto zero) ⇒ sono i
  target sicuri per il trail.
- `misc` contiene l'HWINFO LG (`lge_one_binary`) ⇒ **non toccare**.
- Il pattern sparse in `cache` (`2080 0000` a ogni blocco) è **metadato
  ext4 preesistente**, non il nostro trail.
- Il bootloader è **LK**, non ABL. Entra in recovery da solo se si scrive
  `RECOVERY_MODE` (0x77665502) in `IMEM+0x65c` = `0xfe80f65c`
  (`RESTART_REASON_ADDR2`, perché l'H815 non è msm8994).
- `fastboot boot` **non** è supportato da questo LK 0.5 ⇒ ogni test
  richiede un flash vero di `boot`.

### 3.1 Il LK di fabbrica (ground truth)

Dump della partizione `aboot` (p8) dalla recovery, salvato in
`research/lk-g4/aboot-lk-g4-h815.elf`
(sha256 `0e120ff5d930a9d8bd3f113932e8af88d07ff12caa742e0911f884672cc21666`).

* ELF32 **ARM** stripped, entry `0x340f90`, segmento `PT_LOAD` a vaddr
  `0x0f900000` (file offset `0x8000`, filesz `0xe93d8`, memsz `0xfe53c`).
* LK è **PIC**: usa coppie `movw`/`movt`, non literal pool con indirizzi
  assoluti. (I miei primi scanner cercavano `ldr [pc]` e non trovavano
  niente: è il motivo per cui gli xref erano vuoti.)
* Indirizzi trovati nel binario:
  * `mmc_put_card_to_sleep` a `0x0f916a6c` — fa **CMD7(deselect) + CMD5(sleep)**
  * `sdhci_mode_disable` a `0x0f914940` — `bic #1` su `pwrctl_base+0x78`
  * `target_uninit` a `0x0f900574` — chiama i due precedenti
  * `boot_linux` a `0x0f930c24` — chiama `target_uninit` **incondizionatamente**
  * `sdhci_msm_init` a `0x0f913f10` — la sequenza di riattivazione HC mode
* Conclusione: **LK mette la card in sleep e disabilita l'HC mode prima di
  saltare al kernel**. Quando il shim parte, la card è deselezionata e il
  controller è in legacy MCI mode.
* `MMC_RCA = 2` (`mmc_sdhci.h:130`), confermato anche dal disassembly.

Script di analisi in `research/lk-g4/` (`an.py`, `an2.py`, `an3.py`).
Servono `capstone`:

```sh
python3 -m venv .venv && ./.venv/bin/pip install capstone
./.venv/bin/python research/lk-g4/an.py
```

Disassembly:

```sh
xcrun llvm-objdump -d research/lk-g4/aboot-lk-g4-h815.elf > /tmp/lk.asm
```

`llvm-objdump` **non** risolve i literal PC-relative: per gli xref cerca a
mano le coppie `movw`/`movt`.

---

## 4. I file da toccare

| file | righe | cosa contiene |
|---|---|---|
| `shim/emmc.c` | 986 | driver eMMC: bring-up, CMD16/CMD17/CMD24/CMD8, wake, diagnostica |
| `shim/shim.c` | 1005 | main del shim, `reset()`, `exc_handler()`, fasi, timing |
| `shim/g4.h` | 116 | indirizzi hardware G4 (eMMC, GIC, IMEM…) |
| `shim/console.c` / `.h` | | log in RAM; `con_disable()`/`con_enable()`/`con_repaint()` |
| `shim/mmu.c` / `.h` | | MMU a 4K/16K, page table |
| `shim/fdt.c` / `.h` | | lettura del device tree |
| `shim/loader.c` | | cerca e avvia un payload XNU |
| `shim/build.sh` | | build, con le knob `SHIM_HW_DIAG`, `FORCE_WAIT_S`, `RAW_TIMING`, `CON_SCALE`, `FB_BPP` |
| `tools/run-shim.sh` | 68 | flash + misura del tempo + decodifica |
| `tools/read-trail.sh` | 49 | legge il trail dalla recovery |
| `tools/mkbootimg_lg.py` | | costruisce l'immagine boot nel formato LK |
| `tools/verify_bootimg.py` | | verifica la struttura dell'immagine |
| `tools/qemu-g4.py` | | test di non-regressione su QEMU |

Documentazione: `GUIDE-00-SETUP.md`, `GUIDE-01-SHIM.md`,
`GUIDE-02-TELETEST.md`, `GUIDE-03-EMMC-DEBUG.md`, `STATUS.md`, `NOTES.md`
(il diario cronologico, **leggilo: contiene i vicoli morti**),
`docs/g4-hardware/`.

---

## 5. Le correzioni già fatte al driver eMMC (non ripeterle)

1. **Indirizzamento**: su card high-capacity (>2 GiB, la nostra ha 32 GB)
   l'argomento di CMD17/CMD24 è **il numero di blocco**, non l'indirizzo in
   byte. Riferimento: `mmc_sdhci.c:1805` in LK — `if (card->type ==
   MMC_TYPE_STD_MMC) cmd.argument = blk_addr * card->block_size; else
   cmd.argument = blk_addr;`. Il vecchio vincolo "LBA < 2^23" e l'esclusione
   di `cache` erano un artefatto di questo errore.
2. **`MODE_READ`**: il bit di lettura è il **bit 4** (0x10), non il bit 2.
   Il bit 2 è Auto-CMD12.
3. **Fine trasferimento**: `emmc_wait(XFER_DONE | CMD_DONE)` usciva su
   CMD_DONE senza attendere la fine del trasferimento né il busy.
4. **HC mode**: la riattivazione va fatta con la sequenza di
   `sdhci_msm_init` (`0x0f913f10`): clear bit 0 di `CORE+0x78`, core reset
   (bit 7 di `CORE+0x00`), `writel(1)`, `FF_CLK_SW_RST_DIS` (bit 13).
   **Saltare il core reset** lasciava il controller non sincronizzato.
5. **Ordine del wake**: **CMD7 (select) PRIMA di CMD5 (awake)**, non il
   contrario. CMD5 agisce solo sulla card selezionata; LK deseleziona
   prima di mettere in sleep (`mmc_put_card_to_sleep`).
6. **CMD5**: argomento `0x80000000 | (rca << 16) | 0x00008000`
   (bit 31 = host support, bit 15 = awake).
7. **RCA**: candidati `{2, 1}` (2 primo, `MMC_RCA = 2`), con fallback
   CMD3 che restituisce l'RCA reale.
8. **`emmc_first_*`**: `emmc_write_log()` gira due volte (main + `reset()`);
   la seconda sovrascriveva lo stadio della prima. Ora il risultato della
   prima è tenuto separato.
9. **Panic handler**: non scrive più sulla card dopo un fault (evita la
   ricorsione infinita).

---

## 6. Come testare

```sh
cd /Users/io/Dev/ios/ios-sweet-lg-g4

# build e verifica
./shim/build.sh
python3 tools/verify_bootimg.py shim/out/shim-boot.img      # PASS

# non-regressione QEMU (non tocca MMIO del telefono)
SHIM_HW_DIAG=0 CON_SCALE=2 FB_BPP=32 ./shim/build.sh
rm -f shim/out/qemu-ram.bin && python3 tools/qemu-g4.py

# build con diagnostica: il tempo dice fase*20 + passo eMMC
RAW_TIMING=1 ./shim/build.sh

# flash + misura (il telefono torna in recovery da solo)
./tools/run-shim.sh

# leggere il trail dalla recovery
./tools/read-trail.sh
```

Il telefono è **in recovery con adb root** in questo momento.

### Come decodificare il tempo

```
tempo_totale = 35,2 (costante di boot LK+TWRP) + attesa_shim
attesa_shim  = phase * 20 + emmc_step      (in modalità RAW_TIMING=1)
```

La costante di 35,2 s va ri-misurata se cambi qualcosa nel percorso di
boot; verifica con due build `FORCE_WAIT_S` diverse.

### Il loop è automatico

`reset()` scrive `RECOVERY_MODE` in IMEM ⇒ LK rientra in recovery da
solo. Non serve toccare il telefono.

---

## 7. Errori e vicoli morti (NON ripetere)

* **`fastboot boot` / `fetch` / `reboot recovery` / `oem <dump>`**: non
  supportati da questo LK 0.5.
* **`fastboot flash` di `userdata`**: non farlo senza conferma esplicita.
* **`cache` come target del trail**: contiene dati ext4 preesistenti.
* **`misc`**: contiene l'HWINFO LG.
* **Scansione della RAM bassa (0–4 MiB)**: data abort, la regione
  0–0x09600000 è protetta dalla XPU.
* **`/dev/mem` e `/proc/kcore` assenti** nella recovery: non si può
  leggere la RAM da fuori.
* **`adb exec-out dd if=... of=/tmp/x`**: `of=` viene valutato **sul
  telefono**. Usare `of=/sdcard/...` + `adb pull`.
* I device node partono dall'inizio della partizione: gli offset sono
  **relativi** (LBA 256 dentro la partizione, non LBA globale).
* `busybox dd` con `skip=` su un LBA globale oltre la fine partizione
  restituisce 0 byte. `busybox od` non supporta `-A`/`-t`.
* **`mkbootimg --dtb`**: formato sbagliato per LK, usare
  `tools/mkbootimg_lg.py`.
* **QEMU con `-monitor stdio` e input piped**: il monitor mangia l'input;
  usare memory-backend-file e leggere il file RAM.
* **`rm -rf` sulla working directory**: bloccato dal gate di sicurezza.
* **SuperSU/Magisk su Android 13/AICP**: SuperSU non supportato. (Qui
  Android è 8.1, quindi SuperSU 2.46 *dovrebbe* funzionare, ma la TWRP
  con root basta per il workflow.)

---

## 8. Prossimi passi, in ordine

1. **Isolare la fase 7 vs 8.** Disattiva temporaneamente il blocco PSCI
   (`shim/shim.c`, fase `psci`) e vedi se il tempo cambia: se il crash
   sparisce o si sposta, è `smc()`. Oppure salta direttamente alla GIC.
   Il DT dell'H815 non ha `psci` e usa
   `enable-method = "qcom,8994-arm-cortex-acc"` ⇒ `smc()` è il sospetto
   numero uno.
2. **Se il crash è in `smc()`**: non chiamare PSCI a freddo. Nel DT cerca
   il nodo `psci` / `arm,smc` e usa l'indirizzo e il metodo corretti, oppure
   saltalo (le CPU secondarie non servono per la Fase 2).
3. **Poi** tornare sull'eMMC: a quel punto il driver dovrebbe arrivare a
   `STAGE_DONE` e scrivere il trail. Leggerlo con `tools/read-trail.sh`:
   contiene il testo della console e un settore `G4SHIMDG` con lo
   snapshot dei registri host (CORE+0x78, HC+0x10c, CAPS, PRESENT STATE,
   SEC_COUNT, modo di indirizzamento, RCA).
4. **Poi** il porting vero: Fase 2 (build XNU per msm8992), Fase 3 (root
   iOS su userdata), Fase 4 (touch/pulsanti/display).

### Note per la Fase 2 (XNU)

* Build XNU locale **non fattibile** con Xcode 27 (ABI `iig`
  incompatibile): servono Xcode 15.x o CI. Manca la machine config
  `msm8992`.
* Granulo pagina **16K vs 4K** sul G4 reale non ancora confermato
  (`mmfr0_el1` / `tg0`): lo shim lo legge e lo stampa, ma il dump non è
  mai arrivato su schermo.
* Il framebuffer è **bloccato dalla XPU** ⇒ per il boot di XNU serve il
  driver MDP/DSI command mode, non il simple-framebuffer.

---

## 9. Stato dei backup

`backup/` contiene `boot-stock.img` e `nethunter-boot.img` (i boot
originali). **Non è tracciato in git** (contiene dati del dispositivo) e
**non va su GitHub**.

Il `boot` attualmente sul telefono è l'immagine shim. Per tornare al
boot stock:

```sh
adb reboot bootloader
fastboot flash boot backup/boot-stock.img
fastboot reboot
```
