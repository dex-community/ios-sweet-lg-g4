# GUIDE-03 — Diagnosi del driver eMMC sul LG G4

Stato: **non risolto**. Questa guida spiega dove si blocca il bring-up
eMMC, perché l'attuale strategia è sbagliata, e i test da eseguire in ordine.

Tutto gira sul telefono reale, loop automatico: il telefono torna in
recovery da solo, quindi ogni test è "flash → misura → leggi".

---

## 1. Il sintomo

L'unico dato che abbiamo è il **tempo di attesa** prima del reset:

```
fastboot reboot  →  (telefono rientra in recovery)   ≈ 41,6 – 42,3 s
```

Il tempo viene decodificato così (`shim/shim.c:194-210`):

```
wait_s = (stage * STAGE_STEP_S + |cmd_error| * 3) - elapsed_s
```

con `STAGE_STEP_S = 20` (`shim/shim.c:39`). La lettura "stadio 2, errore 0"
(= 40 s) punta a `shim/emmc.c:577` (`emmc_last_stage = STAGE_POWER`), cioè
**prima di ogni comando MMC**.

---

## 2. Perché l'attuale approccio è sbagliato

Il codice in `shim/emmc.c` fa un bring-up completo da zero:

1. reset software dell'host SDHCI (`HC_SWRST`, `shim/emmc.c:551`)
2. bus power on (`HC_POWER`, `shim/emmc.c:585`)
3. clock della card via GCC (`emmc_card_clock_400k()`, `shim/emmc.c:596`)
4. CMD0 → CMD1 → CMD2 → CMD3 → CMD7 (identificazione e selezione card)

**Ma l'eMMC è già inizializzato quando il nostro shim parte.** Il bootloader LK
leggera il nostro boot image dalla card e poi ci salta sopra:

* `/tmp/lk-lg/app/aboot/aboot.c:890` → `mmc_read(ptn + offset, hdr->kernel_addr, ...)`
* `/tmp/lk-lg/platform/msm_shared/mmc.c:2462` → `mmc_read()` → `mmc_boot_read_from_card()`

Quindi quando il shim gira, la card è **già identificata, alimentata,
cloccata e selezionata** (RCA assegnato, in stato Transfer).

Conseguenze:

* il reset software dell'host **distrugge** la configurazione che LK ha
  lasciato (DLL/CDC ricalibrati, bit vendor, MCLK selector, IO pad switch);
* rifare CMD0/CMD1/CMD2/CMD3/CMD7 è inutile;
* **non serve l'RCA**: sulla bus SD l'indirizzo della card non viaggia nei
  comandi, la selezione è avvenuta con CMD7. CMD24 scrive sulla card
  selezionata, punto.

Il reset software è anche il sospetto n.1 del blocco: dopo `HC_SWRST` i
registri vendor tornano al default e i pad dell'eMMC (1,8 V) possono
cadere — cosa che in un boot "normale" non succede perché LK non rifà quel
reset in quel modo.

---

## 3. I file da toccare

| File | Cosa contiene | Cosa toccare |
|---|---|---|
| `shim/emmc.c` | driver eMMC (bring-up + scrittura) | `emmc_host_init()` (riga 502), `emmc_card_init()`, `emmc_write_log()` (riga 818) |
| `shim/g4.h` | indirizzi e costanti hardware G4 | `HC_*`, `CORE_*`, `G4_*` |
| `shim/shim.c` | main del shim, reset, panic | `reset()` riga 161, `exc_handler()` riga 283, `shim_main()` riga ~790 |
| `shim/build.sh` | build | aggiungere le nuove knob di test |
| `tools/run-shim.sh` | flash + misura del tempo | nessuna modifica (è il tool di misura) |
| `tools/read-trail.sh` | lettura del trail dalla recovery | nessuna modifica |
| `research/lk-g4/` | **nuovo**: LK di fabbrica dell'H815 + script di analisi | vedi §5 |

---

## 4. Il piano, in ordine di costo

### Test 0 — validare il canale del tempo (OBBLIGATORIO, 5 min)

Non fidarti del "stadio 2" finché non hai verificato che il tempo misurato
dipende davvero da `wait_s`. La misura include anche il boot della TWRP, che
potrebbe dominare il totale e rendere il modello numerico meaningless.

In `shim/shim.c`, in `reset()`, aggiungi una forcing:

```c
#if SHIM_HW_DIAG
    wait_s = SHIM_FORCE_WAIT_S;   /* 0 = disabilitato */
#endif
```

con in `shim/build.sh`:

```sh
FORCE_WAIT_S=${FORCE_WAIT_S:-0}
# ... e aggiungi -DSHIM_FORCE_WAIT_S=$FORCE_WAIT_S
```

Poi:

```sh
FORCE_WAIT_S=5   ./shim/build.sh && ./tools/run-shim.sh   # tempo T1
FORCE_WAIT_S=60  ./shim/build.sh && ./tools/run-shim.sh   # tempo T2
```

* `T2 - T1 ≈ 55 s` → il canale funziona, e "stadio 2" vuol dire qualcosa.
* `T2 - T1 ≈ 0` → il canale è rotto: **tutte le conclusioni prese finora
  (stadio 2, stadio 4, IO-pad fix, GCC clock) non valgono niente** e va
  rifatto tutto da capo con un canale diverso.

### Test 1 — granularità fine dello stadio (10 min)

Lo stadio attuale copre tre scritture diverse. Serve una codifica a 1 s e
un posto per il codice di errore.

In `shim/emmc.c` (riga 257-269) sostituisci la tabella degli stadi con una
sola variabile monotona a 8 bit, e in `shim/shim.c` cambia la codifica:

```c
/* prima */
uint64_t deadline_s = (uint64_t)stage * STAGE_STEP_S + err_s;
/* dopo: 1 secondo per unità di stadio */
uint64_t deadline_s = (uint64_t)stage + err_s;
```

Metti i micro-stadi **prima** di ogni scrittura suspecta:

```c
emmc_last_stage = 40;  wr8(HC + HC_POWER,  ...);   /* emmc.c:585 */
emmc_last_stage = 41;  wr8(HC + HC_TIMEOUT, ...);  /* emmc.c:589 */
emmc_last_stage = 42;  wr8(HC + HC_CTRL1,  ...);   /* emmc.c:592 */
```

Con 1 s per stadio, un run solo dice **quale** delle tre è quella che muore.
Aggiungi anche `rc` al tempo (mettilo in un secondo canale, es. decine di
secondi: `deadline = stage + rc*100`).

> Nota: `reset_stage` (`shim/shim.c:908`) viene impostato **solo** sul
> percorso normale. Se il driver fa fault, `exc_handler()` chiama
> `reset(15)` con `reset_stage` ancora a `0xffffffff` → l'attesa è 15 s, non
> 40 s, e `reset()` rientra in `emmc_write_log()` (`shim/shim.c:186`) →
> ricorsione. Quindi "attesa 40 s" **prova che il driver non è andato in
> fault** ma è tornato con errore: buona notizia, ci dice che il blocco è
> un *ritorno anticipato*, non un crash. Verificalo con Test 0.

### Fix A — non reinizializzare la card (30 min, consigliato)

In `shim/emmc.c`, in `emmc_host_init()`:

1. **togli** il reset software (`shim/emmc.c:551-556`)
2. **togli** la scrittura di `HC_POWER` (585)
3. **togli** la scrittura di `HC_TIMEOUT` (589)
4. **togli** la scrittura di `HC_CTRL1` (592)
5. **togli** `emmc_card_clock_400k()` (596) e le impostazioni GCC
6. **togli** tutta `emmc_card_init()` (CMD0/1/2/3/7): non serve l'RCA

Lascia il controller esattamente come LK l'ha lasciato. Poi il percorso
minimo è:

```
CMD16  SET_BLOCKLEN = 512
CMD24  WRITE_BLOCK, arg = LBA * 512 (indirizzo in BYTE!), PIO, 512 byte
CMD18  READ_MULTIPLE_BLOCK, rilettura e confronto
```

`sha256sum` non serve. Attenzione: `CMD24` prende l'indirizzo **in byte**,
quindi il LBA deve essere < 2^23 (c'è già la guardia a `shim/emmc.c:617`).
Target: `spare1` (LBA 259328) e `spare2` (LBA 385280).

Il rischio residuo: LK lascia il controller in **HS400** (DDR + data strobe).
Se la PIO a HS400 non funziona, si può provare a far scendere il controller
a HS400 esplicitamente via i bit MCLK del registro vendor (`CORE_VENDOR_SPEC`,
bits 19:21 = MCLK_SEL_IN, bit 18 = MCLK_SEL_IN_EN), cioè esattamente il
problema che il commento a `shim/emmc.c:507-522` già descrive.

### Fix B — riusare il driver di LK (1-2 h, il più robusto)

LK ha già un driver eMMC che funziona: non lo riscrivere, **chiamalo**.

LK espone (in `/tmp/lk-lg/platform/msm_shared/mmc.c`):

```c
uint32_t mmc_write(unsigned long long data_addr, unsigned int data_len, unsigned int *in);   /* riga 2427 */
uint32_t mmc_read (unsigned long long data_addr, unsigned int *out, unsigned int data_len);   /* riga 2462 */
```

Attenzione all'**ordine degli argomenti diverso** fra read e write.

Le due funzioni usano le globali `mmc_host` e `mmc_card` di LK, che a quel
punto sono già valide (la card è stata identificata da LK).

Dal shim basta:

```c
extern uint32_t mmc_write(unsigned long long addr, unsigned int len, unsigned int *in);
...
mmc_write((unsigned long long)LBA * 512, 512, (unsigned int *)buf);
```

Il problema è **trovare l'indirizzo runtime** di `mmc_write`, perché il
binario è stripped. Vedi §5.

### Fix C — se A e B falliscono: un altro canale di log

La console non è usabile (il framebuffer è dentro `secure_region` protetto
dalla XPU) e senza scrivere sulla card non c'è nessun canale. Le alternative
da valutare, in ordine di costo:

* **LK fastboot**: far ripartire LK in `LK_FASTBOOT_MODE` (0x77665500,
  `shim/g4.h`) e usare il protocollo fastboot dal PC. Anche solo `getvar`
  che restituisce stringhe potrebbe bastare per pochi byte.
  (`fastboot boot`/`fetch`/`reboot recovery`/`oem` non sono supportati da
  questo LK 0.5 — verificare cosa c'è.)
* **root su Android**: il telefono è Android **8.1.0 Oreo** (fingerprint
  `lge/omni_g4/g4:16.1.0/OPM8.181105.002/23:eng/test-keys`), quindi
  SuperSU 2.46 ha buone probabilità di funzionare. Con root su Android si
  può guardare il driver SDHCI del kernel LG che **funziona** e copiare i
  valori di registro, e si ha `dmesg` completo.
  SuperSU è però vecchio e non supportato ufficialmente; la TWRP con root
  (già funzionante) basta per il workflow attuale.

---

## 5. Il LK di fabbrica (ground truth)

La partizione `aboot` dell'H815 contiene **il bootloader LK vero**. Dump
fatto dalla recovery (root):

```sh
adb shell 'dd if=/dev/block/mmcblk0p8 of=/sdcard/aboot.img bs=4096'
adb pull /sdcard/aboot.img
```

salvato in `research/lk-g4/aboot-lk-g4-h815.elf`
(sha256 `0e120ff5d930a9d8bd3f113932e8af88d07ff12caa742e0911f884672cc21666`).

Cosa contiene:

* **ELF32 ARM**, stripped, entry `0x340f90`, segmento `PT_LOAD` a vaddr
  `0x0f900000` (file offset `0x8000`, `filesz 0xe93d8`, `memsz 0xfe53c`)
* stringhe utili: `mmc init failed!`, `%s: target is not emmc boot`,
  `%s: mmc read failure`, `%s: mmc write failure - %s`, `MMC Boot: MBR read failed!`,
  `GPT: Could not read primary gpt from mmc`, `failed to set sdc%u_iface_clk`,
  `platform/msm_shared/sdhci.c`, `sdhci_msm_init_dll`
* **`ANDROID!`** dentro il segmento di testo: è la costante `BOOT_MAGIC`
  con cui LK confronta l'header del boot image
* LK carica il boot image da `MEMBASE + 0xFF000`
  (`/tmp/lk-lg/app/aboot/aboot.c:89-92`). Se `MEMBASE == 0x0f900000`, l'header
  sta a **`0x0f9ff000`**, esattamente subito dopo la fine di LK
  (`0xf900000 + 0xfe53c = 0xf9fe53c`). **Da verificare dal shim**: leggere
  16 byte a `0x0f9ff000` e controllare che inizino con `ANDROID!`; l'header
  contiene anche `kernel_addr` (dove LK ha caricato il nostro shim) e
  `ramdisk_addr`.

Cosa ci serve da LK:

* la sequenza di init eMMC **reale** (disassemblarlo, confronto con la
  nostra) → utilissima per Fix A;
* l'indirizzo di `mmc_write`/`mmc_read` → per Fix B.

### Script di analisi

`research/lk-g4/an.py`, `an2.py`, `an3.py` (richiedono `capstone`, in un venv):

```sh
python3 -m venv .venv && ./.venv/bin/pip install capstone
./.venv/bin/python research/lk-g4/an.py     # segmenti, stringhe, literal pool
./.venv/bin/python research/lk-g4/an3.py    # xref PC-relative
```

Nota: **LK è PIC**, usa `add rX, pc, #imm` e non literal pool con indirizzi
assoluti. Lo scanner in `an3.py` va corretto di conseguenza (la versione
attuale cerca solo `ldr` con pool assoluti e **non trova niente** — è il
prossimo passo).

Per disassemblare:

```sh
xcrun llvm-objdump -d research/lk-g4/aboot-lk-g4-h815.elf > /tmp/lk.asm
```

`llvm-objdump` **non** risolve i literal PC-relative: per trovare i riferimenti
alle stringhe cerca a mano le sequenze `add rX, pc, #imm`.

---

## 6. Come testare (loop)

```sh
cd /Users/io/Dev/ios/ios-sweet-lg-g4

# 1. build + verifica
./shim/build.sh
python3 tools/verify_bootimg.py shim/out/shim-boot.img      # PASS

# 2. non-regressione QEMU
SHIM_HW_DIAG=0 CON_SCALE=2 FB_BPP=32 ./shim/build.sh
rm -f shim/out/qemu-ram.bin && python3 tools/qemu-g4.py

# 3. flash + misura (il telefono torna in recovery da solo)
./tools/run-shim.sh

# 4. leggere il trail dalla recovery
./tools/read-trail.sh
```

Il telefono è **in recovery con adb root** in questo momento. Non serve
toccoare `userdata`.

### Attenzione

* `fastboot boot` **non** è supportato da questo LK: ogni test richiede un
  flash vero della partizione `boot`. I backup sono in `backup/`
  (`boot-stock.img`, `nethunter-boot.img`).
* Non scrivere mai su `misc` (contiene l'HWINFO LG) né su `cache`
  (contiene dati ext4 preesistenti).
* `cmd24` prende l'indirizzo **in byte**: LBA < 2^23. Target sicuri:
  `spare1` (LBA 259328) e `spare2` (LBA 385280), entrambi vuoti.
* `adb exec-out dd if=... of=/tmp/x` valuta `of=` **sul telefono**: usa
  `of=/sdcard/...` + `adb pull`. I device node partono dall'inizio della
  partizione, quindi gli offset sono **relativi** (LBA 256, non LBA globale).
