# GUIDE-06 — Misurare i due controller SD senza rischiare il telefono

Sessione 2026-11-09. Questa guida copre il passaggio che viene dopo i due
hang: **misurare** lo stato dei controller e dei pin, e poi **parlare** con la
microSD.

---

## 1. Perché le misure si fanno dalla recovery

Il 2026-11-09 il telefono si è bloccato due volte perché lo shim scriveva
su un controller che nessuno aveva inizializzato (si veda
`GUIDE-05-MMIO-SAFE.md`). La conclusione pratica:

> **Nessuna misura va fatta dallo shim su hardware che non ha già
> funzionato.** La recovery è un kernel vero, con driver veri: lì leggere i
> registri è sicuro, e i numeri descrivono un sistema **vivo** invece di uno
> stato a metà.

Lo shim resta il posto giusto per la **sonda sul momento del passaggio dal
bootloader**: lì il controller eMMC è vivo (il LK ci ha appena letto il boot
image) e la sola lettura è innocua. La recovery, invece, vede lo stato che
**Android** ha lasciato.

Le due misure sono complementari:

| misura | dove | cosa dice |
|---|---|---|
| il GCC RCG di SDCC1 | **shim** (fase A della sonda) | a che velocità parlava il bootloader alla carta eMMC |
| i pin TLMM e i due controller | **recovery** | come sono muxati i pin e come sono configurati i controller |

---

## 2. La misura dalla recovery

Il recovery ha root e `/dev/mem` (l'Android no: `/dev/mem` risponde ENXIO).
Lo script **legge e basta**:

```sh
# nel recovery, con adb
adb shell 'cd /sdcard && sh /sdcard/read-mmc-registers.sh /sdcard/mmc-registers.txt'
# oppure, dal computer, se il recovery ha la shell:
tools/read-mmc-registers.sh /sdcard/mmc-registers.txt
```

Poi il dump si interpreta:

```sh
python3 tools/decode-mmc-registers.py /percorso/mmc-registers.txt
```

Le finestre lette (tutte dal device tree del telefono):

| finestra | base | cosa |
|---|---|---|
| TLMM | `0xfd510000` | il mux dei 10 pin SDC |
| GCC | `0xfc400000` | i clock dei due slot |
| SDCC1 HC / CORE | `0xf9824900` / `0xf9824000` | il controller eMMC |
| SDCC2 HC / CORE | `0xf98a4900` / `0xf98a4000` | il controller microSD |

### Il formato del dump

`od -A x -t x4 -v`: l'indirizzo in esadecimale **senza padding a 8 cifre**
("0000000"), poi le word a 4 byte. Il decoder lo sa (è stato verificato con
un file di valori noti: vedi §5).

---

## 3. Cosa dice ogni tabella

### 3.1 Il mux dei pin (TLMM)

Dieci pin, il nodo `sdc` del device tree:

```
0 sdc1_clk   1 sdc1_cmd   2 sdc1_data   3 sdc1_rclk
4 sdc2_clk   5 sdc2_cmd   6 sdc2_data
7 sdc3_clk   8 sdc3_cmd   9 sdc3_data
```

Ogni pin ha due registri a 8 byte: **CTL** (direzione, pull, drive strength)
e **MUX** (la funzione nei bit 2:0). Il mux **sopravvive al reset**: quello
che Android ha selezionato è ancora lì quando lo shim parte.

- se i pin 4-6 sono su una funzione ≠ SDC2 → lo shim dovrà muxarli, e
  **il numero di funzione che il kernel usa è proprio il valore che
  leggiamo qui** (il DT non lo contiene: `qcom,pins = <0x196 0x04>` dà solo
  il numero del pin);
- se i pin 0-3 (SDC1) non sono su SDC1 → **questo spiegherebbe l'eMMC
  muta**: i pin della carta sarebbero demuxati.

### 3.2 Il clock vero (GCC)

Il divisore SDHCI è **ignorato** su MSM (`SDHCI_QUIRK2_ALWAYS_USE_BASE_CLOCK`,
sdhci-msm.c:3489): il clock della carta è il RCG del GCC.

La frequenza si calcola con la formula del driver stesso
(`drivers/clk/qcom/clock-local2.c:1142-1162`):

```
pre_div = parent / ((div_val + 1) >> 1)      div_val = CFG & 0x1f
n       = (~n_val) + m                        n_val è sign-extended
rate    = pre_div * m / n
```

con la tabella CAF `F(f, src, div, m, n)` (`clock-gcc-8992.c:43-52`) che
scrive `CFG[4:0] = 2*div-1`, `N = ~((N-M))`, `D = ~N`.

**Per 400 kHz dall'XO** (l'entrata `F(400000, gcc_xo, 12, 1, 4)` della tabella
`ftbl_sdcc2_4_apps_clk_src`, clock-gcc-8992.c:1125):

```
CMD_RCGR = 0x00000001     CFG_RCGR = 0x00000017
M = 0x00000001   N = 0xfffffffc   D = 0xfffffffb
```

→ 19.2 MHz / 12 × 1/4 = **400 kHz** (verificato: il decoder, alimentato con
questi valori, restituisce 400000 Hz).

### 3.3 I controller

| registro | cosa |
|---|---|
| HC+0x40 CAPS1 | il base clock (bit 15:8) e le capacità |
| HC+0x11c | le **tensioni** (3.3V bit 24, 3.0V bit 25, 1.8V bit 26): non sono in CAPS1, è qui che il driver CAF le legge (sdhci-msm.c:3232-3240) |
| HC+0x10c | il vendor spec: PWRSAVE (bit 1), MCLK_SEL (8-9), IO_PAD_PWR_SWITCH (bit 16), SELECT_IN (18, 19-21) |
| HC+0x24 | PRESENT_STATE: CMD/DATA inhibit, livello di DAT0 |
| HC+0x29 | il bus power |

Se CAPS1 legge 0 o 0xffffffff → la finestra non è viva: il controller non è
inizializzato e **non si deve scriverci** (è la causa dei due hang).

---

## 4. La ricetta SD (dal tree CAF, non a memoria)

| passo | dove | cosa |
|---|---|---|
| CMD0 | `sd.c:821` | `mmc_go_idle(host)` |
| **CMD8** | `sd_ops.c:193` | arg = `((ocr & 0xFF8000) != 0) << 8 \| 0xAA` → **0x1AA**; la R7 deve avere 0xAA nel byte basso |
| HCS | `sd.c:829-831` | se CMD8 risponde: `ocr \|= SD_OCR_CCS` (bit 30) |
| 1.8 V | `sd.c:838-839` | se UHS-I: `ocr \|= SD_OCR_S18R` (bit 24) |
| **ACMD41** | `sd_ops.c:150` | CMD55 con arg 41, poi CMD41 con arg = ocr; **100 tentativi** con 10 ms fra uno e l'altro, finché la R3 non ha il bit 31 (power-up complete) |
| CMD2, CMD3, CMD7, CMD9 | `sd_ops.c` | CID, RCA, selezione, CSD |

L'OCR parte da `host->ocr` (`mmc.c:2209`), cioè dalle tensioni del DT
(`qcom,vdd-io-voltage-level = <0x1b7740 0x2d0370>` = 1.8-2.95 V). Per una SD
che non chiede lo switch a 1.8 V: `0x40FF8000` (HCS + 2.7-3.6 V + S4);
`0x40FF8080` aggiunge anche S18.

---

## 5. I test (offline, ripetibili)

Il decoder è stato validato con due dump a valori noti:

1. **un file binario** con `CTL=pin`, `MUX=pin+1` per i dieci pin, passato
   per `od -A x -t x4 -v` → il decoder restituisce esattamente quei valori;
2. **un dump completo simulato** (TLMM + GCC + i due HC) → il decoder calcola
   **400000 Hz** per il RCG con i valori della tabella CAF e riconosce le
   sezioni.

Il primo test ha trovato un bug vero: il parser accettava solo indirizzi di
otto cifre, mentre `od -A x` stampa "0000000" (sette). Corretto.

```sh
python3 tools/decode-mmc-registers.py <il-dump>
```

---

## 6. I file

| file | cosa fa |
|---|---|
| `tools/read-mmc-registers.sh` | legge le finestre dalla recovery (solo `dd if=/dev/mem`) |
| `tools/decode-mmc-registers.py` | trasforma il dump nelle tre tabelle |
| `research/lk-g4/T1B-SDCC2.md` | la statica completa di SDCC2 e la ricetta SD |
| `GUIDE-05-MMIO-SAFE.md` | perché il telefono si era bloccato e come è protetto adesso |
