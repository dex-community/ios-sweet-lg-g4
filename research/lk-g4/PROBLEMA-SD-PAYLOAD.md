# PROBLEMA aperto — il payload sulla microSD non viene caricato

Sessione 2026-11-09, dopo la consegna di `shim/sd.c` (bring-up della microSD).
Tutto il resto è verde e committato (`a83b5cb`). **Questo è l'unico pezzo che
non gira ancora**, ed è l'ultimo pezzo che manca per poter far partire iOS
senza l'eMMC.

---

## 1. Il problema in una riga

Il test `tools/qemu-sd.py` carica il payload dalla microSD e **non lo trova**:
la carta risponde, il CRC è pronto, ma l'header "G4PAYLD1" non viene letto.

---

## 2. Stato attuale (misurato)

### FATTO — i check che passano

```
ok  : the clock (seq 269) comes before the controller (seq 280)
ok  : the command sequence is [0, 8, 55, 41, 2, 3, 7, 16, 17]
ok  : the RCG of SDCC2 is programmed for 400 kHz
ok  : 768 words came out of the buffer port
ok  : SD rca=00000000 cid=03534453
ok  : SD ocr=C0FF8080 stage=0000000F
ok  : SD rc=00000000 stage=0000000F
ok  : SD err=00000000 rca=00000000
```

La bring-up della carta **funziona**: il clock va su prima del controller, la
sequenza dei comandi è quella del core CAF, il clock è a 400 kHz e il primo
settore viene letto.

### FATTO — i due check che falliscono

```
FAIL:
  - the payload was not read from the card (no "SDPAY" line in the console log)
  - the loader was not told to use the payload of the card
```

### FATTO — cosa c'è nel log

Il log runtime del shim (nel dump RAM del QEMU) contiene:

```
SD rc=00000000 stage=0000000F
SD err=00000000 rca=00000000
PSTORE hdr=44000000 sig=43474244
...
INITRD lo=41000000 hi=41000040
LOADER no SWEETXNU payload
```

cioè: **la carta è stata inizializzata con successo** (`SD rc=0`,
`SD err=0`), ma **fra "SD err=" e "INITRD" non c'è nessuna riga "SDPAY"**,
che è la riga che il ramo di caricamento del payload dovrebbe stampare.

### FATTO — il ramo esiste ed è compilato

- `shim/shim.c:1327-1344`: il blocco `#if SHIM_SD_PAYLOAD` che chiama
  `sd_payload_load()` e stampa `con_hex2("SDPAY", "rc=", ...)`.
- `aarch64-elf-objdump -d shim/out/shim.o | grep sd_payload_load` → **1
  riferimento**: la chiamata **è** nel binario.
- `shim/shim.c:1336` c'è anche un marker di debug:
  `con_hex2("SDPAY", "pre", (uint32_t)sd_init_error, "stage=", ...)`,
  che **non compare** nel log.

### FATTO — il modello dovrebbe servire l'header

- `shim/emmc_model.inc:861-862`: il modello legge `"/chosen/sd-payload"` dal
  device tree e lo mette in `model_sd_payload`.
- `shim/emmc_model.inc:866-895`: se `model_sd_payload` è 1, costruisce
  l'header "G4PAYLD1" nel blocco 0 e il payload (512 byte) nel blocco 1, con
  il CRC32 calcolato, e mette `model_sd2_disk_blocks = 2`.
- il device tree di test **contiene** `sd-payload` (verificato:
  `b'sd-payload' in data` → True).

---

## 3. I tre sospetti, in ordine di probabilità

### Sospetto 1 — il ramo non viene eseguito

La condizione è `if (sd_init_error == 0)`, e il log dice `SD err=00000000`.
**Se il ramo fosse entrato, la riga "SDPAY pre=..." ci sarebbe** (il marker è
prima della chiamata). Non c'è ⇒ il ramo **non** è stato eseguito.

Da controllare:
1. `sd_init_error` letto in `shim/shim.c` è **la stessa variabile** che
   `shim/sd.c` scrive? La dichiarazione è in `shim/sd.h`
   (`extern int sd_init_error;`) e `shim/sd.h` è incluso in `shim/shim.c`
   **solo** dentro `#if SHIM_SD_INIT`. Nella build del test `SHIM_SD_INIT=1`
   e `SHIM_SD_PAYLOAD=1`, quindi dovrebbe esserci: **verificalo**.
2. Il blocco è dentro un `#if` annidato che nella build è 0? Guarda
   `shim/shim.c:1320-1345` per i `#endif` che lo circondano.

### Sospetto 2 — l'header non è servito dal modello

Il CMD17 con LBA 2048 deve restituire il blocco 0 del disco simulato.
Da controllare:
1. `shim/emmc_model.inc`, `model_sd2_build_response()`, caso
   `MCMD_SD_READ_BLOCK`: l'indice è
   `idx = (lba >= MODEL_SD2_PAYLOAD_LBA) ? lba - MODEL_SD2_PAYLOAD_LBA : lba`
   → per 2048 deve dare 0.
2. `model_sd2_cmd_arg` è l'LBA? È impostato in `model_write_reg()` dal
   registro ARGUMENT **al momento della scrittura del COMMAND** — ma il
   CMD17 scrive l'ARGUMENT *prima* ✔.
3. `model_sd2_disk_blocks` vale 2 quando arriva il CMD17? Il blocco che lo
   imposta è dopo la lettura del DT ✔ (riga 870 > 862).
4. `model_sd_payload` vale 1 davvero? La lettura del DT è a riga 861-862, ma
   **controlla che `model_mode = MODEL_MUTE;` e `model_delay_ms = 100;` non
   siano DOPO** la lettura (erano lì prima: se tornassero dopo, la carta
   sarebbe muta — ma la carta risponde, quindi vanno bene).

### Sospetto 3 — la struct letta non è quella scritta

`shim/sd_payload.c` legge l'header in una `struct sd_payload_hdr`
(`shim/sd_payload.h`): `char magic[8]; uint32_t version, len, crc32, flags;`
e il modello scrive gli stessi campi a partire da `model_sd2_blocks[0] + 8`.
Verifica l'allineamento: `struct sd_payload_hdr` ha `magic[8]` poi 4 u32 →
allineamento 4, offset 8 ✔. Ma `sd_read_block()` copia il blocco in
`uint8_t block[512]` e poi in `hdr` — attenzione al **ordine degli indici**: in
`sd_read_block()` il blocco è letto in `out[0..511]` ✔.

---

## 4. I file da modificare

| file | cosa c'è | cosa toccare |
|---|---|---|
| `shim/shim.c` | 1327-1344: il ramo che chiama `sd_payload_load()`; 1336: il marker "SDPAY pre" | il punto di partenza: metti una stampa **prima** di `if (sd_init_error == 0)` e guarda cosa dice |
| `shim/sd_payload.c` | 34-110: `sd_payload_load()` | dove legge l'header e controlla il magic (errore -2 = magic diverso, -5 = lettura fallita, -6 = CRC diverso) |
| `shim/sd_payload.h` | la struct dell'header e i numeri di blocco | `SD_PAYLOAD_HDR_LBA` = 2048, `SD_PAYLOAD_LOG_LBA` = 8192 |
| `shim/sd.c` | 358-...: `sd_read_block()`; 620-640: `sd_init()` | `sd_read_block()` è la primitiva: se restituisce -1/-2/-3/-4, l'errore è qui, non nel payload |
| `shim/emmc_model.inc` | 396-420: il CMD17 del modello; 853-895: la costruzione del disco di prova; 541-580: `model_window()` | il lato "carta" del test: se il blocco 2048 non contiene "G4PAYLD1", il problema è qui |
| `tools/qemu-sd.py` | i check finali | per guardare dentro: stampa il contenuto del blocco 2048 che il modello serve |
| `shim/emmc_model.inc` (funzione `model_window`) | 541-580 | ricorda: le finestre SDCC2 sono servite **solo se `model_sd2_clocked`** |

---

## 5. Come riprodurlo (tutto offline, ~2 minuti)

```sh
cd /Users/io/Dev/ios/ios-sweet-lg-g4
python3 tools/qemu-sd.py
```

Il test ricostruisce lo shim con `SHIM_SD_INIT=1 SHIM_SD_PAYLOAD=1
SHIM_EMMC_MODEL=1`, avvia il QEMU per 45 s e legge il dump RAM.

Per guardare dentro:

```sh
# il log runtime del shim (l'indirizzo di con_log viene dall'ELF)
python3 - <<'EOF'
import subprocess
out = subprocess.run(['aarch64-elf-nm', 'shim/out/shim.elf'],
                     capture_output=True, text=True).stdout
addr = [int(l.split()[0], 16) for l in out.splitlines()
        if len(l.split()) == 3 and l.split()[2] == 'con_log'][0]
data = open('shim/out/qemu-sd/qemu-ram.bin', 'rb').read()
base = 0x200000 + addr
print(data[base:base + 0x20000].split(b'\x00')[0].decode('ascii', 'replace'))
EOF
```

---

## 6. Cosa NON si deve toccare (è già verde e committato)

- `shim/emmc.c`, `shim/emmc_lk_bringup.inc`: il percorso eMMC è collaudato
  (`tools/qemu-emmc.py --mode always` → 13 comandi, 768 parole ✔).
- `shim/mmio.h`: l'allowlist che ha impedito i due hang. Se aggiungi una
  finestra, **solo** dopo aver verificato che è viva (è il punto di tutto
  GUIDE-05-MMIO-SAFE.md).
- `shim/wdt.c`, `shim/trail.h`: la rete di sicurezza (watchdog e
  self-limiting).
- `tools/check-mmio-writes.py`: gira nella build e deve continuare a passare.

---

## 7. Il contesto (perché questo pezzo conta)

L'eMMC **non risponde**: CMD1 va in timeout (otto varianti di bring-up, tre
run, stesso risultato). La microSD, invece, funziona sotto Android e
risponde anche al nostro shim (è quello che dice il test qui sopra).

Quindi il piano è: **il payload di XNU (kernel + ADT) vive sulla microSD**,
il boot image porta solo lo shim, e lo shim lo legge dalla carta, ne verifica
il CRC e ci salta. È il pezzo che rende il porting indipendente dall'eMMC.

I numeri che servono dopo (misure dalla recovery, zero rischio):
`tools/read-mmc-registers.sh` + `tools/decode-mmc-registers.py`
(sono i pin muxati dei due slot e i clock veri).

Il telefono è in fastboot con `boot` **stock** (verificato per hash): per
riavviarlo serve un cold boot (stacca e ricollega il cavo), e da lì si
potrà provare la build `shim-boot-sd.img` sul telefono vero.
