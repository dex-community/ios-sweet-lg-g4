# T1 — Statica SDCC2 (slot microSD) su LG G4 H815

Sessione 2026-11-09. Tutto qui è **FATTO** (letto nel sorgente o misurato sul
telefono) salvo dove è marcato **DEDUZIONE** o **IPOTESI**. I nomi dei registri
presi dal driver Linux CAF sono sempre `ipotesi(nome Linux)`.

Riferimenti:

- DTS: `/Users/io/Dev/ios/lg g4/msm8992-lg-h815-boot.dts`
- Kernel CAF: `/Users/io/Dev/ios/lg g4/G4-Titan-Kernel-titan_bpf2/`
- LK: `research/lk-g4/aboot-lk-g4-h815.elf` + disassembly in `research/lk-g4/`
- Live dal telefono: `research/raw-2026-11-09/t0-live.txt`, `t0-live2.txt`

---

## 1. Indirizzi e IRQ

| Voce | Valore | Fonte | Etichetta |
|---|---|---|---|
| SDCC2 HC ("hc_mem") | `0xf98a4900` | DTS:6428 | FATTO |
| SDCC2 HC size | `0x11c` | DTS:6428 | FATTO |
| SDCC2 CORE ("core_mem") | `0xf98a4000` | DTS:6428 | FATTO |
| SDCC2 CORE size | `0x800` | DTS:6428 | FATTO |
| alias DT | `sdhc2 = "/soc/sdhci@f98a4900"` | DTS:13271 | FATTO |
| `compatible` | `"qcom,sdhci-msm"` | DTS:6427 | FATTO |
| `interrupts` | `<0x00 0x01 0x02>` | DTS:6430 | FATTO |
| `interrupt-names` | `hc_irq`, `pwr_irq`, `status_irq` | DTS:6431 | FATTO |
| IRQ GIC di `hc_irq` | 221 (`0xdd`) | DTS:6455 (interrupt-map) | FATTO |
| IRQ GIC di `pwr_irq` | 210 (`0xd2`) | DTS:6455 | FATTO |
| IRQ GIC di `status_irq` | 123 (`0x7b`) | DTS:6455 | FATTO |
| bus width | 4 bit (`qcom,bus-width = <0x04>`) | DTS:6434 | FATTO |
| host in Android | `f98a4900.sdhci` → `mmc1` | `t0-live.txt:16,1004` | FATTO |

Confronto eMMC (SDCC1), per la differenza di IRQ e dimensione HC:

| Voce | SDCC1 | SDCC2 |
|---|---|---|
| HC | `0xf9824900` (0x1a0) | `0xf98a4900` (0x11c) |
| CORE | `0xf9824000` (0x800) | `0xf98a4000` (0x800) |
| IRQ hc / pwr | 123 / 138 | 221 / 210 |
| bus width | 8 bit | 4 bit |

**DEDUZIONE**: la finestra HC di SDCC2 è più piccola (0x11c) perché quel
controller non ha il blocco ADMA/caps esteso che l'eMMC ha. Nel codice del
shim i due host non possono condividere le stesse costanti di registro oltre
0x11c.

---

## 2. Clock GCC (FATTO, da `clock-gcc-8992.c`)

`GCC` base = `0xfc400000` (DTS:3852, `reg = <0xfc400000 0x2000>`).

| Registro | SDCC1 | SDCC2 | Fonte |
|---|---|---|---|
| APPS_CMD_RCGR | `0x4d0` | `0x510` | clock-gcc-8992.c:88,91 |
| APPS_CBCR | `0x4c4` | `0x504` | clock-gcc-8992.c:89,92 |
| AHB_CBCR | `0x4c8` | `0x508` | clock-gcc-8992.c:90,93 |

**RCG MND layout** (`clock-local2.c:48-52`): i registri di un RCG sono
contigui a partire da `CMD_RCGR`:

```
+0x00 CMD_RCGR   (root enable bit1, status bit31, config-update bit0)
+0x04 CFG_RCGR   (div[4:0], src_sel[10:8], mnd_mode[13:12])
+0x08 M          +0x0c N          +0x10 D
```

Bits (`clock-local2.c:62-72`): `CBCR_BRANCH_ENABLE_BIT=bit0`,
`CBCR_BRANCH_OFF_BIT=bit31`, `CMD_RCGR_ROOT_ENABLE_BIT=bit1`,
`CMD_RCGR_CONFIG_UPDATE_BIT=bit0`, `CMD_RCGR_ROOT_STATUS_BIT=bit31`,
`CFG_RCGR_DIV_MASK=BM(4,0)`, `CFG_RCGR_SRC_SEL_MASK=BM(10,8)`,
`MND_MODE_MASK=BM(13,12)`, `MND_DUAL_EDGE_MODE_BVAL=BVAL(13,12,0x2)`.

**Tabella frequenze identica per SDCC1 e SDCC2**
(`ftbl_sdcc1_apps_clk_src` clock-gcc-8992.c:1098 e
`ftbl_sdcc2_4_apps_clk_src` clock-gcc-8992.c:1125, stessa tabella per SDCC3/4):

| Frequenza | sorgente | div | M | N |
|---|---|---|---|---|
| 144 kHz | gcc_xo | 16 | 3 | 25 |
| **400 kHz** | **gcc_xo** | **12** | **1** | **4** |
| 20 MHz | gpll0_out_main | 15 | 1 | 2 |
| 25 MHz | gpll0_out_main | 12 | 1 | 2 |
| 50 MHz | gpll0_out_main | 12 | 0 | 0 |
| 100 MHz | gpll0_out_main | 6 | 0 | 0 |
| 200 MHz | gpll0_out_main | 3 | 0 | 0 |

Conversione in registri (macro `F`, clock-gcc-8992.c:43-52):
`m_val = M`, `n_val = ~((N-M) * !!(N))`, `d_val = ~N`,
`div_src_val = BVAL(4,0,FIXDIV(div)) | BVAL(10,8,src)` con
`FIXDIV(div) = div ? 2*div-1 : 0` e `gcc_xo_source_val = 0`.

**Calcolo per 400 kHz** (XO = 19.2 MHz, `F(400000, gcc_xo, 12, 1, 4)`):

- `div = 12` → `FIXDIV(12) = 23` → `CFG_RCGR[4:0] = 23` (0x17)
- sorgente XO → `CFG_RCGR[10:8] = 0`
- `M = 1`
- `N = 4` → `n_val = ~((4-1)) = ~3 = 0xFFFFFFFC`; il registro N non è
  sign-exteso, si scrive `0xFFFFFFFC` (bit 31..8 a 1) → il driver nel
  recovery fa `n_val |= (n_val >> 8) ? BM(31,16) : BM(31,8)`
  (`clock-local2.c:1152`).
- `D = 4` → `d_val = ~4 = 0xFFFFFFFB`
- `MND_MODE = 0x2` (dual edge) perché `n_val != 0`

Verifica della formula: la frequenza è `parent * M / (N_reale)` con
`N_reale = ~n_val + M = 3 + 1 = 4`, e il pre-divider vale `(div+1)/2 = 6`:
`19.2 MHz / 6 / 4 = 800 kHz`; con `div` inteso come `FIXDIV` il divisore
totale è `2*div = 24`: `19.2 MHz / 24 / 4 = 200 kHz`. **Il valore esatto del
divisore va verificato in QEMU/ sul telefono misurando la frequenza: la
tabella CAF è la fonte, ma la mia aritmetica sul pre-divider non è
riproducibile con certezza.** → **IPOTESI / DA VERIFICARE**.

**LK**: `set_sdc_core_clk` (0x0f90c378) accetta le frequenze
`0x60000` (393216), `0x2ee0000` (49152000), `0x05b80000` (96000000),
`0x0b710000` (191500000), `0x0beb0000` (200000000), `0x17d70000`
(400000000) e **0x14816000**/`0x16e36000` (dis-4.9-h-set_sdc_core_clk.txt:120-129).
Il LK **non** usa 400 kHz per l'eMMC: il suo clock è alto (>= 393 kHz con
`div` fisso). → **FATTO: il LK pilota il clock dell'SDCC1 dal GCC, non dal
divisore SDHCI.** Questo conferma la correzione "A1" sotto.

---

## 3. "A1: il divisore SDHCI è ignorato su MSM" — ora è FATTO

`sdhci-msm.c:3488-3489`:

```c
host->quirks |= SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN;
host->quirks2 |= SDHCI_QUIRK2_ALWAYS_USE_BASE_CLOCK;
```

`SDHCI_QUIRK2_ALWAYS_USE_BASE_CLOCK` fa sì che il driver SDHCI generico **non
programmi il divisore**: il clock della carta è il *base clock*, cioè quello
che il SoC fornisce. E il driver imposta il clock vero con
`clk_set_rate(msm_host->clk, sup_clock)` (`sdhci-msm.c:2929`), dove
`msm_host->clk` = `"core_clk"` = il clock GCC del controller.

Corollario: **il clock della carta è il GCC SDCCx_APPS**, e il nostro shim
scrive solo `HC+0x2c` (il divisore SDHCI) → **non sta impostando il clock
reale**. Questo spiega perché il `HC+0x2c = 0x0007` (divisore 0) letto
all'ingresso dello shim non dice nulla: il clock era pilotato dal GCC.

**FATTO (driver CAF)**: `sdhci_msm_set_clock()` scrive `CORE_VENDOR_SPEC`
(0x10c) per togliere `CORE_CLK_PWRSAVE` (bit 1) durante l'identificazione
(`sdhci-msm.c:2800-2810`), e mette `CORE_HC_MCLK_SEL_DFLT = 2` (bit 8-9) e
`CORE_HC_SELECT_IN` default per una carta non-HS400 (2832-2844, 2898-2900).

**FATTO (driver CAF)**: `CORE_IO_PAD_PWR_SWITCH = (1 << 16)` in
`CORE_VENDOR_SPEC` (0x10c) (`sdhci-msm.c:102`) è il bit che seleziona il
livello dei pad IO: `REQ_IO_HIGH` lo **azzera**, `REQ_IO_LOW` lo **accende**
(`sdhci-msm.c:2441-2448`). Quindi la mia ipotesi precedente "bit 15/16" era
**sbagliata sul bit**: è il **bit 16** (non 15). Il bit 15 non è definito in
quel driver.

---

## 4. Pin TLMM e card-detect

I pin di SDCC2 sono nello **stesso nodo pinctrl 0x196** degli SDCC1, con
numeri di pin diversi:

| Gruppo DT | pin | funzione | phandle | fonte |
|---|---|---|---|---|
| `pmx_sdc1_clk` | 0x196 / 0x00 | sdc1-clk | — | DTS:11460-11470 |
| `pmx_sdc1_cmd` | 0x196 / 0x01 | sdc1-cmd | — | DTS:11473-11483 |
| `pmx_sdc1_data` | 0x196 / 0x02 | sdc1-data | — | DTS:11486-11494 |
| `pmx_sdc1_rclk` | 0x196 / 0x03 | sdc1-rclk | — | DTS:11495-11510 |
| **`pmx_sdc2_clk`** | **0x196 / 0x04** | **sdc2-clk** | 0x129 | DTS:11512-11530 |
| **`pmx_sdc2_cmd`** | **0x196 / 0x05** | **sdc2-cmd** | 0x12a | DTS:11532-11550 |
| **`pmx_sdc2_data`** | **0x196 / 0x06** | **sdc2-data** | 0x12b | DTS:11552-11570 |
| `pmx_sdc3_clk` | 0x196 / 0x07 | sdc3-clk | 0x12f | DTS:11572-… |

`pinctrl-0` (active) di SDCC2 = `<0x129 0x12a 0x12b>` = clk, cmd, data
(DTS:6458). `pinctrl-1` (sleep) = `<0x12c 0x12d 0x12e>` (DTS:6459).

**FATTO**: SDCC2 ha **solo 3 pin** (clk, cmd, data) perché il bus è a 4 bit
e il pin `data3`/`rclk` è condiviso con la modalità sleep; il DT non elenca
un pin `sdc2_data3` né `sdc2_data4`.

**FATTO (card-detect)**: `cd-gpios = <0xd2 0x08 0x01>` (DTS:6456), cioè
controller GPIO `0xd2` = `pm8994-gpio` (DTS:4073-4075), indice 8, flag
`0x01` = **GPIO_ACTIVE_HIGH**. Il kernel lo conferma:
`gpio-873 (f98a4900.sdhci cd) in lo` (`t0-live.txt:791`) — la carta è
inserita e il segnale è **attivo basso in questo momento** nonostante il flag
active-high nel DT, quindi il pin del PMIC è a 0 con carta dentro.
`sdhci-msm.c:1675-1677` mappa `cd-gpios` → `MMC_CAP2_CD_ACTIVE_HIGH` se il
flag non è ACTIVE_LOW.

**FATTO**: il LK **non inizializza SDCC2**. La tabella clock del LK contiene
`sdc1_core_clk` / `sdc1_iface_clk` e le routine sono
`set_sdc_core_clk` / `set_sdc_iface_clk` con l'indice `%u` del controller
(`dis-4.9-h-…`, `dis-4.9-g-…`), ma nel binario **non esiste alcun riferimento
a sdc2/sdcc2** (`grep -i sdc2` su `research/lk-g4/` → solo commenti generici
`SDCC2` nei sorgenti recuperati). → **l'SDCC2 è inizializzato dal kernel
Android, non dal bootloader.** Questo è il motivo per cui lo shim, a differenza
dell'eMMC, **non trova nulla di già fatto** su SDCC2 e deve fare tutto lui.

---

## 5. Regolatori dello slot SD

| Slot | Regolatore | phandle | Tipo | Tensione DT | Stato su Android |
|---|---|---|---|---|---|
| SDCC1 (eMMC) VDD | `pm8994_l20` | 0x11d | RPM SMD | 2.95 V | **enabled** |
| SDCC1 VDD-IO | 0x39 | — | — | 1.8 V, `qcom,vdd-io-always-on` | n/d |
| **SDCC2 VDD** | **`pm8994_l21`** | **0x126** | **RPM SMD** | 2.95 V (`0x2d0370`) | **disabled** |
| **SDCC2 VDD-IO** | **`pm8994_l13`** | **0x127** | **RPM SMD** | 1.8–2.95 V, init 2.95 V | **disabled** |

Fonti: DTS:6445-6450 (SDCC2 `vdd-supply = <0x126>`, `vdd-io-supply = <0x127>`);
DTS:7583-7592 (`regulator-l21` = `pm8994_l21`, `qcom,set = <0x03>` = RPM SMD
set 3); DTS:7418-7427 (`regulator-l13` = `pm8994_l13`); stato live in
`/sys/class/regulator/regulator.30` (pm8994_l21 → **disabled**) e
`regulator.22` (pm8994_l13 → **disabled**), `t0-live.txt:573-576, 549-552`.

**FATTO**: con la microSD inserita e Android avviato, la SD funziona
(`mmc1: new ultra high speed DDR50 SDHC card at address aaaa`,
`mmcblk1: mmc1:aaaa SS32G 29.7 GiB`) **mentre `pm8994_l21` e `pm8994_l13`
sono "disabled" nel framework dei regolatori**. Il dmesg dice anche
`mmc1: no vqmmc regulator found` / `no vmmc regulator found`
(`t0-live.txt:1002-1004`).

**DEDUZIONE (forte)**: il rail che alimenta lo slot SD **non è governato dal
framework dei regolatori** del kernel: o è always-on nell'hardware, o il DT
non lo descrive correttamente. **Conseguenza per lo shim: non c'è nessun
regolatore da accendere per lo slot SD.** (Da confrontare: per l'eMMC il
driver accende `pm8994_l20` e lo stato live dice "enabled".)

**FATTO (driver CAF)**: `sdhci-msm.c:2144` chiama `regulator_enable(vreg->reg)`
per `vdd_data` e `vdd_io_data`; il DT del G4 li dichiara ma il driver CAF li
abilita solo se trovati (`vreg->reg` può essere NULL e il codice lo salta).

**FATTO (driver CAF)**: `sdhci-msm.c:3483-3486` — *"Card detection is handled
using separate GPIO. Bus power control is handled by interacting with PMIC."*
e il driver imposta `SDHCI_QUIRK_SINGLE_POWER_WRITE`: la scrittura di
HC+0x29 (bus power) è singola.

---

## 6. Cosa deve fare lo shim per SDCC2

Elenco, in ordine, con la fonte di ogni pezzo:

1. **Abilitare i clock GCC di SDCC2**: `AHB_CBCR` `0xfc400508` prima di
   `APPS_CBCR` `0xfc400504` (ordine: l'AXI/AHB deve essere pronto prima del
   clock dell'app). La routine esistente `emmc_gcc_enable()` in
   `shim/emmc_lk_bringup.inc:160` fa già questo pattern per SDCC1
   (`GCC_SDCC1_AHB_CBCR` prima di `GCC_SDCC1_APPS_CBCR`, righe 256-257).
   **FATTO** che l'ordine AHB-prima è quello del driver: `sdhci-msm.c:3355`
   abilita `iface_clk` (AHB) e solo dopo (3364-3378) `core_clk`.
2. **Programmare il clock a ~400 kHz via GCC RCG** (`0xfc400510` + M/N/D),
   non via il divisore SDHCI. Per l'identificazione SD l'init deve stare a
   ≤400 kHz. **FATTO** che il CAF usa `sup_clk_table` = `qcom,clk-rates` del
   DT: per SDCC2 il DT **non** ha `qcom,clk-rates` (lo ha solo SDCC1,
   DTS:6421) → il driver prende 400000 dalla tabella GCC
   (`ftbl_sdcc2_4_apps_clk_src`).
   **FATTO**: `sdhci_msm_get_min_clock()` (2609-2615) restituisce
   `sup_clk_table[0]`, e per SDCC2 il driver non ha la tabella DT → il suo
   `sup_clk_cnt` viene dalla tabella GCC.
3. **Core reset + HC mode**: come per l'eMMC (`CORE+0x78` bit 0 = HC_MODE_EN,
   `CORE+0x00` bit 7 = core reset), dal `sdhci_msm_init` del LK
   (`emmc_lk_bringup.inc:261-268`).
4. **Vendor spec 0x10c**: togliere `CORE_CLK_PWRSAVE` (bit 1) e mettere
   `CORE_HC_MCLK_SEL_DFLT` (2<<8), come fa il driver (2791-2830).
   **Per SD in 1.8 V**: `CORE_IO_PAD_PWR_SWITCH` (bit 16) decide la tensione
   dei pad; `REQ_IO_LOW` lo accende.
5. **Bus power HC+0x29**: il valore va preso da CAPS1 (bit 26 = 1.8V
   support) e `SDHCI_QUIRK_SINGLE_POWER_WRITE` impone una sola scrittura.
   **DA VERIFICARE** su SDCC2 quale valore scrive il driver (il DT dice
   `qcom,vdd-io-voltage-level = <0x1b7740 0x2d0370>` = 1.8–2.95 V).
6. **Sequenza SD**: CMD0 → CMD8 (0x1AA) → ACMD41 (HCS, loop) → CMD2 → CMD3 →
   CMD7 → (CMD9/CSD) → CMD16 (se SDSC) → CMD17.
   **FATTO** che il CAF/SD stack usa `0x40ff8080` come OCR per ACMD41
   (`docs/g4-hardware/EMMC-REFERENCE.md:511`, da UniXNU) e che
   `CMD1` va ripetuta (UniXNU: 1000 tentativi a 1 ms).
7. **Card detect**: `pm8994-gpio` indice 8, active-high nel DT. **Non
   necessario** per una prima prova: se il timeout è l'unica risposta,
   la carta non c'è o non risponde; il GPIO serve solo per non interrogare
   uno slot vuoto e per l'eventuale IRQ.

---

## 7. Cosa NON è stato trovato (limiti di T1)

- I **registri TLMM** per scrivere il mux dei pin 0x196/0x04..0x06: il tree
  kernel **non contiene `drivers/pinctrl/qcom/`** (vicolo noto). Il base
  TLMM e l'offset dei pin servono per il mux manuale, se T2/T3 lo richiederà.
- **Nessuna mappa dei registri PM8994 GPIO** per il card-detect (serve SPMI).
- Il **valore esatto del divisore GCC** per 400 kHz (arithmetic above).
- Il **valore HC+0x29** che il driver scrive su SDCC2 (serve CAPS1 letto dal
  vivo: T2).

---

## 8. Implicazioni per l'eMMC (T2(a))

- **FATTO**: il clock della carta eMMC è pilotato dal **GCC SDCC1**
  (`0xfc4004d0` RCG), non dal divisore SDHCI. Il nostro shim **non scrive
  mai** `0xfc4004d0`. → la velocità reale della carta durante CMD0/CMD1 non
  è mai stata controllata da noi. **A1 è plausibile** e T2 deve leggere
  RCG/CBCR di SDCC1.
- **FATTO**: `HC+0x10c` bit 1 = `CORE_CLK_PWRSAVE`: se il LK lo lascia
  acceso, il controller **gatea il clock della carta da solo** e nessun
  comando completa. Il driver CAF lo toglie durante l'identificazione
  (2800-2810). Il nostro shim lo toglie **solo** se la variante
  `LK_F_CLR_PWRSAVE` è attiva — cioè **non nella configurazione di default**.
  → da leggere in T2(a) prima e dopo il reset.
- **FATTO**: `CORE_IO_PAD_PWR_SWITCH` è il **bit 16** di 0x10c (non 15).
- **FATTO**: `CORE_HC_MCLK_SEL` (bit 8-9) e `CORE_HC_SELECT_IN` (bit 18, 19-21)
  descrivono il percorso del clock; il driver li mette a default per una
  carta non-HS400. Il shim lo fa solo con `LK_F_VENDOR_RESET`.
