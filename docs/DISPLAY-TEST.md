# Test display dello shim — guida corrente (aggiornata 2026-11-16)

**Questo documento descrive il codice che c'è davvero in `shim/display.c`.**
Per l'archivio dei sette test precedenti vedi `old/displayTest.md`; per la
mappa di tutta la documentazione vedi `docs/INDEX.md`.

> **IL DISPLAY È RISOLTO.** Il mode **11** scrive HELLO, numero di stage,
> etichetta e heartbeat sul pannello. La ricetta, i tentativi falliti e il
> metodo riutilizzabile su altri device sono in **`docs/DISPLAY-SOLVED.md`**:
> leggi quello prima. Questo documento resta come riferimento operativo dei
> singoli mode e dei comandi.

**Storia in quattro righe.** D0/D1 (15/11): il logo non cambia nemmeno
scrivendo 10,5 MiB in `cont_splash`. D4 (16/11): BOOT dump catturato, entrambi
i link DSI vivi, ma il pannello resta fermo. D6: `0x28` tiene il pannello
**nero** → i DCS raggiungono il pannello. D10: finestra a 720 colonne → **rosso
pieno**, scritto dall'alto verso il basso. D11: stessa geometria, byte
diversi → **HELLO**. La causa dello static era uno sfalsamento di riga: la
finestra era a 640 colonne su un pannello da 720.

Il prossimo run utile non è un test display: è la verifica del kernel XNU a
4 KiB sul device (`docs/DISPLAY-SOLVED.md §6`).

---

## 1. Obiettivo

Dare allo shim una superficie visibile: **HELLO**, un **numero di stage** e un
**heartbeat**. Lo shim in questa build **non salta a XNU**: dipinge, mantiene
il quadro per 20 s e si riavvia da solo in recovery, dove si legge il pstore.

Perché il numero di stage e non solo HELLO: un testo statico dice "sono
arrivato fino al disegno", ma non dove si è fermato dopo. Ogni stage
ridipinge il proprio numero **prima** di toccare l'hardware, quindi un hang
sul passo 3 lascia "3" sul pannello, uno sul passo 7 lascia "7". L'heartbeat
che avanza nel loop distingue "CPU appesa" (numero fermo) da "crash".

**Limite da tenere presente:** la console a schermo funziona solo quando lo
scan-out funziona. È il premio, non lo strumento con cui si trova il guasto:
la bisection si legge dal pstore. Inoltre copre i **log dello shim**; dopo il
salto a XNU questo codice non gira più, e i log del kernel restano sull'anello
di marker in RAM (`tools/read-pram.py --marks`).

---

## 2. Risultati D0 / D1 (15/11) — l'ipotesi chiusa

Eseguiti dall'utente con `fastboot boot`, due build attribuibili:

| | D0 baseline | D1 minimal touch |
|---|---|---|
| `build_id` | `0xa3df8faa` | `0x845d155c` |
| `image_sha256` | `a76202f7…b2db` | `8e4c5be7…ac37` |
| `DISP result=` | `00000040` (`DISP_OK_BASELINE`) | `00000010` (`DISP_OK_DRAWN`) |
| Scritture display | **zero** | `FB rows=00000A00 bytes=00A8C000` (10,5 MiB) |
| Heartbeat | nessuno (per costruzione) | `BEAT n=0…0x27` = **40**, tutti presenti |
| Fault / hang | nessuno | nessuno |
| Schermo | logo LG fermo | **logo LG fermo, zero pixel cambiati** |

Le tre foto allegate (`signal-2026-11-15-18-43-45-922.jpg`,
`19-29-53-887.jpg`, `22-06-25-307.jpg`) mostrano tutte lo stesso splash
LG / "POWERED BY android": nessun HELLO, nessun colore, nessun numero di
stage, nessuna riga di log. L'utente riferisce la stessa immagine anche
durante i due run delle 22:30 / 22:31.

> **NOTA DI RETTIFICA (16/11).** Le conclusioni qui sotto erano corrette
> *sulla misura* ("scrivere in `cont_splash` non cambia nulla") ma la
> spiegazione n. 1 è stata **superata dal BOOT dump di D4**: le pipe RGB0/RGB1
> puntano ancora a `0x03400000` con stride e formato esatti. Non è che MDP
> legga un altro indirizzo: è che **nessun frame parte più**. Vedi §2-bis.
> Il seguito di questo paragrafo va letto come cronologia della diagnosi.

**Cosa chiude, in modo definitivo:**

- D0 dice che la baseline è affidabile: se non tocchiamo nulla il logo resta.
  Quindi tutto ciò che segue è interpretabile.
- D1 dice che **il disegno è avvenuto davvero** (2560 righe, 10,5 MiB, cache
  clean, 40 heartbeat) e che la CPU è viva per tutti i 20 s, **eppure non si
  vede nulla**. Non è un crash, non è un formato pixel sbagliato (un formato
  sbagliato darebbe spazzatura, cioè comunque un cambiamento), non è un
  overrun: `disp_fill_bounded()` ha scritto esattamente il limite corretto.
- **Conclusione: all'handover `0x03400000` NON è la sorgente dello scan-out.**
  L'ipotesi "basta scrivere in `cont_splash`" è morta. Lo conferma anche il
  dato storico: `console.c` dipinge ogni riga di log in
  `con_fb = 0x03400000` dal 10/11 (`docs/shim-log-2026-11-10.txt:1-3`) senza
  che il logo sia mai cambiato.

**Le due spiegazioni rimaste:**

1. **(favorita)** Il JDI NT36850 è un pannello **DSI command-mode con GRAM
   interna**: LK ha già spinto il logo una volta e il pannello si
   auto-aggiorna da solo, con i clock MDSS/DSI spenti. Scrivere in DDR non
   può cambiarlo. `shim/mmio.h` del resto documenta che LK spegne il display
   prima di saltare.
2. MDP sta leggendo **un altro indirizzo** (o una **IOVA** attraverso lo
   SMMU, come il kernel funzionante con `SRC0_ADDR = 0x00E30000`).

All'epoca la mossa successiva scelta fu: non ritoccare `CTL_FLUSH` o i
registri MDP, ma **parlare direttamente al pannello** (mode 4) e ottenere il
BOOT dump per distinguere 1 da 2. Il BOOT dump è arrivato con D4 e ha
smentito la formulazione della n. 1: vedi §2-bis.

---

## 2-bis. Risultato D4 (16/11) — il BOOT dump e i due bit mancanti

`build_id=0x42d146b9`, `image_sha256=c1d73238…b22d`, `DISP result=000000B3`
(= CLOCKS + PLL + DRAWN + PANEL_ON + GRAM). Nessun timeout, nessun hang,
40 heartbeat. **Sul pannello: ancora e solo il logo LG.**

### Cosa ha misurato il BOOT dump (era la lacuna di tutti i sette test)

| Registro | Valore | Significato |
|---|---|---|
| `DSI.ctrl`, `DSI1.ctrl` | `000001F5` | **entrambi i link** abilitati, command mode, 4 lane, clock on — identico al kernel funzionante |
| `DSI.clkctl`, `DSI1.clkctl` | `0000023F` | clock del controller accesi |
| `DSI.mdpctl` | `00000008` | formato RGB888 |
| `DSI.dcsctl` | `00013C2C` | `wr_mem_start=0x2C`, `wr_mem_continue=0x3C`, insert DCS |
| `DSI.strm0c` / `strm0t` | `08710039` / `0A0002D0` | stream 720×2560 |
| **`DSI.trig`, `DSI1.trig`** | **`00000004`** | **`dma_trigger=SW`, `mdp_trigger=NONE`, `te_sel=0`** |
| `RGB0.addr`, `RGB1.addr` | `03400000` | **le pipe LK puntano ancora a `cont_splash`** |
| `RGB0.strd` | `000010E0` = 4320 | stride 1440×3: **esattamente quello usato da D1** |
| `RGB0.size` | `0A0002D0` | 720×2560 per link (dual-DSI split) |
| `RGB2.*` | tutti `0` | la pipe del kernel è spenta |
| `INTF1/2.tgen` | `0` | command mode, nessun timing engine |
| `CTL.top` / `CTL.flush` | `00021F20` / `40000000` | configurazione LK, bit pendente (normale) |

**Sorpresa:** la pipeline MDP **non è morta**. È configurata correttamente e
punta al buffer che D1 ha riempito con il formato giusto. Quello che manca è
un **frame**: LK ha spinto un'immagine nella GRAM e ha lasciato il percorso
frame congelato. I test 1–7 avevano *ricostruito* MDP (RGB2, `CTL_TOP`,
`CTL_LAYER`), rompendo proprio la configurazione buona di LK.

### Le due differenze misurate rispetto al kernel che funziona

1. **`DSI_TRIG_CTRL = 0x00000004` vs `0x80000004`**
   (`old/displayTest.md:262-263`). `BIT(31)` è `te_sel`: lo scrive
   `mdss_dsi_host.c:369-370` quando `pinfo->te_sel` è vero, e il DTS del
   pannello dichiara `qcom,mdss-dsi-te-pin-select = <1>`. Con `te_sel=0` e
   `mdp_trigger=none` il pannello non riceve nuovi frame: la GRAM resta
   sull'ultima immagine.
2. **`DSI_COMMAND_MODE_DMA_CTRL BIT(26)` = LP mode.**
   `dsi_host_v2.c:470-472` imposta `0x14000000` ("from frame buffer, low
   power mode"); `dsi_set_tx_power_mode()` (`:513-525`, e
   `mdss_dsi_host.c:417-437` all'offset reale `0x3c`) **azzera BIT(26) per
   l'HS mode**. Il DTS ha `on-command-state = "dsi_lp_mode"` ma
   `off-command-state = "dsi_hs_mode"`: LK lascia il motore DMA in LP, e i
   nostri pacchetti `0x2C` da 15 KiB non possono uscire in quel modo.

### Perché `done0=1 done1=1` non è una prova

`CMD_DMA_DONE` (`DSI_INT_CTRL` bit 0) è il completamento **lato host**: dice
che il DMA ha movimentato il pacchetto nel FIFO del controller DSI, **non**
che il pannello l'abbia ricevuto o eseguito. D4 ha loggato
`DCS done0=00000001 done1=00000001` due volte, `GRAM packets=00000019` e
40 heartbeat senza alcun errore: tutti completamenti host-side. Per questo
dal 16/11 il log riporta anche `wait_ms=` (un `0` su un pacchetto da 15 KiB
tradirebbe un bit stale) e il dump esteso legge `DSI.dmactl`, `DSI.status`,
`DSI.fifo`, `DSI.ackerr`, `DSI.intctl`, `DSI.rdbk0` su entrambi i link, più i
contatori di frame `INTF1/2.frmc` e `PP0.tear` / `PP0.autorf`.

### Altri due fatti dal DTS del pannello

- `qcom,mdss-dsi-panel-width = <720>`, `bpp = <24>`: la finestra a 640
  colonne e i pixel RGB888 di D4 erano corretti.
- Il pannello usa un **command page select** `0xFF`: l'`on-command` finisce
  con `FF 10` due volte prima di `0x29`, quindi resta su una pagina
  produttore. Nelle pagine produttore `0x2A`/`0x2B`/`0x3A` compaiono come
  *indirizzi di registro* (`15 ... 02 2A 03` ecc.): i nostri comandi DCS di
  finestra possono essere reinterpretati. Il kernel non manda mai `0x2A`/
  `0x2B`: lascia che l'host DSI inserisca `0x2C`/`0x3C` nello stream MDP.
  Per questo `gram_hello()` ora imposta **una sola** finestra, usa `0x2C`
  per il primo pacchetto e `0x3C` (write memory continue) per gli altri.

---

## 3. Le modalità

`DISPLAY_MODE` è l'unico selettore della bisection.

| mode | Nome | Cosa fa | MMIO display | Domanda a cui risponde |
|---|---|---|---|---|
| **0** | D0 baseline | niente. Nessuna lettura e nessuna scrittura | **zero** | il logo LG resta stabile se non tocchiamo nulla? → **sì** |
| **1** | D1 minimal touch | fill limitato di `cont_splash` + HELLO + stage + cache clean | **zero** | LK sta ancora scansionando `0x03400000`? → **no** |
| **2** | D1b | clock core + **BOOT dump** + come 1 + trigger DSI0/DSI1 | sì (clock + 2 scritture) | un kick fa uscire il frame? |
| **3** | D3 full | clock → PLL → DSI → MDP → frame → pannello → kick | sì, in ordine | la pipeline completa riparte? |
| **4** | **D4 GRAM** | clock core + BOOT dump + PLL/pixel clock (solo se i link sono spenti) + DCS `0x11`/`0x29` + finestra `0x2A`/`0x2B` + **HELLO via `0x2C` dritto nella GRAM** | solo DSI/clock, **NESSUN registro MDP/CTL/RGB/LM/INTF** | il pannello è raggiungibile senza MDP? → **eseguito: engine risponde, schermo fermo** |
| **5** | **D5 re-arm** | dipinge **sia `cont_splash` sia la GRAM**, forza **HS** (`BIT(26)` a 0), imposta **`DSI_TRIG_CTRL BIT(31)`** su entrambi i link, poi `MDP_SW_TRIGGER` ogni 500 ms | **zero** scritture MDP/CTL/RGB/LM/INTF; solo 2 registri DSI per link | riattivando TE e HS, una delle due strade mostra HELLO? |
| **6** | **D6 contatto** | `0x28` display off in HS, pannello tenuto spento per i 20 s (ripristinato all'ultimo beat) | 1 registro DSI + 1 comando DCS | il pannello **ubbidisce** a un qualsiasi DCS? Nero = sì → **eseguito: NERO, il canale DCS funziona** |
| **7** | D7 clean HELLO | HS + `0x3A` 24bpp + HELLO in alto, **zero** cambi di TE | solo DSI | il formato pixel era il problema? → **no** |
| **8** | D8 barre | 8 barre a colori in `cont_splash` + kick MDP, zero DCS | sì (MDP) | MDP legge il nostro buffer? → **no** |
| **9** | D9 DCS only | come D7 ma **zero** `MDP_SW_TRIGGER` | solo DSI | sono i frame MDP a sovrascrivere? → **no** |
| **10** | **D10 full GRAM** | finestra 720×2560, **rosso pieno**, 86 pacchetti `0x2C`/`0x3C` | solo DSI | la GRAM è scrivibile? → **sì, rosso pieno** |
| **11** | **D11 SOLUZIONE** | finestra 720×2560, **HELLO + stage + label + heartbeat**, 86 pacchetti | solo DSI | → **sì: HELLO visibile** |

I mode 0..10 sono la bisection che ha portato alla soluzione: **non
riprodurli se non serve**. Il mode 11 è la build da usare.

> **Nota sulla riga D4:** il run del 16/11 (`build_id=0x42d146b9`) usava una
> finestra per pacchetto e solo `0x2C`. Il codice attuale, ricostruito dopo
> la lettura del DTS, usa una finestra sola + `0x3C` continue (§6). Se si
> ricompila il mode 4 non è quindi lo stesso binario di quel run: il
> confronto va fatto riportando sempre `build_id` e `image_sha256`.

**Il mode 2 non è più "zero MMIO".** Fino al 15/11 scriveva
`DSI_CMD_MODE_MDP_SW_TRIGGER` con i clock MDSS spenti: una scrittura su una
finestra con clock spento non completa e la CPU si ferma dentro l'accesso
(due hang il 9/11). Ora chiama `clocks_display_core()` **prima** e ne
approfitta per il BOOT dump. Chi vuole il confronto "D1 + solo un trigger"
deve tenere conto di questa differenza.

`DISPLAY_STAGE` biseziona i mode 3, 4 **e** 5:

| stage | mode 3 | mode 4 | mode 5 |
|---|---|---|---|
| 1 | clock MMSS core | clock core + **BOOT dump** | clock core + **dump esteso** + contatori `FR0` |
| 2 | + PLL DSI e pixel/byte clock, poi BOOT dump | + PLL/clock solo se i link sono giù, poi **probe DCS** | + **HS mode** e **TE select** su entrambi i link |
| 3 | + PHY DSI e controller in command mode | + finestra e **HELLO via `0x2C`** nella GRAM | + dipinge DDR e GRAM, `FR1` → kick → `FR2` |
| 4 | + MDP (CTL, RGB2, INTF2) | — | — |
| 5 | + frame, commit, pannello, kick | — | — |

I mode 0, 1 e 6 ignorano `DISPLAY_STAGE`.

---

## 4. Build

Tutti i comandi partono dalla radice del progetto
(`/Users/io/Dev/ios/ios-sweet-lg-g4`): `build.sh` va lanciato da `shim/`,
`auto-display-test.sh` dalla radice.

```sh
cd /Users/io/Dev/ios/ios-sweet-lg-g4

# normale (salta a XNU, NON tocca il display)
(cd shim && ./build.sh)

# === LA SOLUZIONE: D11, HELLO sul pannello ===
(cd shim && SHIM_DISPLAY_TEST=1 DISPLAY_MODE=11 DISPLAY_STAGE=3 ./build.sh)
./tools/auto-display-test.sh

# i mode della bisection, se serve ripeterli (0..10 sono già eseguiti)
(cd shim && SHIM_DISPLAY_TEST=1 DISPLAY_MODE=0 ./build.sh)
(cd shim && SHIM_DISPLAY_TEST=1 DISPLAY_MODE=6 ./build.sh)   # probe di contatto
(cd shim && SHIM_DISPLAY_TEST=1 DISPLAY_MODE=10 ./build.sh)  # rosso pieno
```

| variabile | default | significato |
|---|---|---|
| `SHIM_DISPLAY_TEST` | **0** | 1 = test display, nessun salto a XNU |
| `DISPLAY_MODE` | 1 | 0..11 come sopra; **11 = la soluzione** |
| `DISPLAY_STAGE` | 5 | fino a dove arriva il mode 3, 4 o 5 |
| `DISP_FB_BPP` | 24 | 24 = RGB888 (pipe ereditate), 32 = ARGB8888 |
| `SKIP_EMMC` | = `SHIM_DISPLAY_TEST` | salta la fase eMMC nel test |
| `DISP_DSI_MAP_SHIFT` | 4 | offset della mappa DSI del telefono |

Ogni build scrive **`out/run-manifest.txt`** e stampa `BUILD id=...` come
prima riga del log pstore:

```
date=...  commit=...  build_id=0x81469482  image_sha256=5b8fcac5...
display_test=1 mode=11 stage=3 bpp=24 skip_emmc=1
```

`build_id` è l'hash di sorgenti e flag: **stabile** a parità di codice.
`image_sha256` cambia a ogni rebuild (il ramdisk contiene un timestamp).
L'immagine già pronta in `shim/out/shim-boot.img` è **D11 stage 3**.

**Copia sempre build_id e image_sha256 nel report.** Prima del 15/11 non
esistevano, ed è il motivo per cui i sette test non sono attribuibili a un
binario preciso. `tools/auto-display-test.sh` stampa il manifest e ora legge
la modalità da lì, così non dice più "HELLO dipinto" in un run D0.

---

## 5. L'ordine (regola d'oro)

**Il clock di un controller si accende prima di scrivere un suo registro.**
Una scrittura su una finestra con clock spento non completa: la transazione
resta sul bus AXI, la CPU si ferma dentro l'accesso (due hang il 2026-11-09).

Per questo i mode 0 e 1 **non fanno alcun MMIO**: scrivono solo DRAM. Il mode
3 segue `clocks_display_core()` → `dsi_pll_enable()` →
`clocks_display_pixel()` → `dsi_phy_init()` → `dsi_ctrl_init_cmd_mode()` →
`disp_mdp_init()` → frame → `dsi_panel_init()`.

I mode 4/5 accendono solo i clock core, **leggono** lo stato ereditato e poi
usano esclusivamente registri DSI. Non programmano MDP, CTL, RGB, LM o INTF:
è il motivo per cui sono più corti e per cui il problema SMMU/IOVA non li
riguarda. Se `DSI0_CTRL` e `DSI1_CTRL` risultano già
`DSI_EN | CMD_MODE_EN | CLK_EN`, **non toccano nemmeno PLL e clock pixel**:
riconfigurare un RCG vivo potrebbe spegnere il pannello e mescolerebbe due
variabili.

Il mode 5 aggiunge esattamente **due bit**, entrambi misurati e non inventati:
`DSI_COMMAND_MODE_DMA_CTRL &= ~BIT(26)` (LP → HS) e
`DSI_TRIG_CTRL |= BIT(31)` (`te_sel`, per portare `0x00000004` al
`0x80000004` del kernel funzionante). Nessun soft reset, nessun `CTL_FLUSH`,
nessuna riconfigurazione delle pipe: la pipeline buona di LK resta intatta.

Il **BOOT dump** viene letto a clock accesi ma **prima di qualunque
scrittura nei registri MDP/DSI**: è la fotografia dello stato ereditato dal
bootloader che mancava in tutti i test precedenti ed è stata finalmente
catturata con D4 (§2-bis). Dal 15/11 include `DSI1.ctrl`, `DSI1.clkctl`,
`DSI1.trig`; dal 16/11 il dump è passato da 37 a **53 registri**, con
`dmactl/status/fifo/ackerr/intctl/rdbk0` su entrambi i link, i contatori di
frame `INTF1/2.frmc` e `PP0.tear/autorf`. Su un pannello dual-DSI leggere
solo DSI0 è metà della misura.

---

## 6. Buffer e formato pixel

| Buffer | Indirizzo | Dimensione | Chi lo legge |
|---|---|---|---|
| `cont_splash_mem` | `0x03400000` | **12 MiB** (`0xC00000`) | le pipe RGB0+RGB1 del bootloader, 720×2560, **RGB888** — **ancora configurate su questo indirizzo, ma senza frame in uscita** (BOOT dump D4) |
| framebuffer shim | `0x20000000` | DRAM libera | mode 3: RGB2, 1440×2560, **ARGB8888**, stride 5760 |
| scratch HELLO dei mode 4/5 | `0x20000000` | 640×200×3 = 384.000 B | nessuno: è solo la sorgente del DMA DCS `0x2C`/`0x3C` |
| buffer comandi DCS | `0x50010000` | **16 KiB** | DMA del controller DSI (`DSI_DMA_CMD_OFFSET`) |

- `1440*2560*3 = 0xA8C000` (10,5 MiB) **entra** nei 12 MiB;
  `1440*2560*4 = 0xE10000` (14,1 MiB) **non entra**.
  Il vecchio `disp_draw()` scriveva quest'ultimo in `cont_splash`: 2,06 MiB
  oltre `0x04000000`. Ora `disp_fill_bounded()` limita le righe a
  `size / stride` (D1 ha confermato `rows=0xA00 bytes=0xA8C000`).
- Il testo usa `con_draw_text_bpp()` (`shim/console.c`): il formato è scelto a
  runtime, non più hardcoded a 32 bpp con stride 5760 su pipe RGB888.
- Il colore di ogni stage ha **tutti i byte uguali**: è uniforme sia che le
  pipe leggano 3 sia 4 byte per pixel.
- Il kernel funzionante usa `SRC0_ADDR = 0x00E30000`, che è una **IOVA** sul
  SMMU MDP, non un indirizzo fisico (`docs/DISPLAY-FINDINGS.md`). Se il
  context bank 0 è agganciato, gli indirizzi fisici non sono visibili al MDP.
  Il BOOT dump di D4 rende l'ipotesi meno probabile (le pipe LK leggono
  l'indirizzo fisico atteso); in ogni caso **i mode 4/5 aggirano la
  questione**: il percorso DCS non passa da MDP.

### I pacchetti DCS (mode 4/5/6)

Il formato è quello del kernel del telefono
(`mdss_dsi_cmd.c:96-135`, `mdss_dsi_host.c:1734-1795`), verificato con un
modello host prima di buildare:

- header a 4 byte: `WC = lunghezza_payload + 1` (il byte di comando conta),
  `DTYPE = 0x39` (DCS long write), `LONG_PKT`, `LAST`;
- `buf[4]` = comando, payload da `buf[5]`, padding `0xff` fino a multiplo di 4;
- `DSI_DMA_CMD_OFFSET = 0x48`, `DSI_DMA_CMD_LENGTH = 0x4C`,
  trigger `DSI_CMD_MODE_DMA_SW_TRIGGER = 0x90` (la mappa del telefono è
  quella dell'albero + 4);
- `CMD_DMA_DONE` è bit 0 di `DSI_INT_CTRL` (`0x110`) ed è **write-one-to-clear**.

Dual-DSI: il kernel manda lo **stesso** `dma_addr` a entrambi i controller
(`mdss_dsi_host.c:1769-1781`). Il comando viene quindi trasmesso su DSI0 e
DSI1; una finestra di 640 colonne appare **due volte**, una per metà pannello.
È voluto: è inconfondibile ed evita di dover indovinare come LK ha diviso le
colonne (il DTS conferma `panel-width = <720>` per link).

**Cambiato il 16/11:** il run D4 impostava una finestra `0x2A`/`0x2B` nuova
prima di ogni pacchetto da 8 righe e usava `0x2C` per tutti. Ora
`gram_hello()` imposta **una sola** finestra, manda il primo pacchetto con
`0x2C` e tutti i successivi con **`0x3C` (write memory continue)**: è lo
stesso meccanismo dell'host DSI del kernel (`DSI.dcsctl = 0x00013C2C`,
`wr_mem_start=0x2c`, `wr_mem_continue=0x3c`) e riduce il rischio che
`0x2A`/`0x2B` vengano reinterpretati sulla pagina comando produttore.

---

## 7. `CTL_FLUSH` non è un criterio di riuscita

Il kernel che funziona ha `CTL_FLUSH = 0x40000018` in idle con **27 frame
giocati e 0 underrun** (`docs/display-2026-11-10.txt`): bit pendenti sono
normali su questo SoC. Giudicare un test dal flush è sbagliato, e il poll del
test 6 (`v != kick_value`) usciva sempre alla prima iterazione.

`disp_mdp_commit()` ora fa il poll giusto — `(v & mask) == 0`, solo sui bit
che abbiamo scritto — ma lo **registra come informazione**, non come verdetto.
Il verdetto è: **si vede qualcosa sul pannello, sì o no** — oppure, dal
16/11, il verdetto numerico dei contatori `FR*`. I mode 4/5/6 non scrivono
affatto `CTL_FLUSH`.

---

## 8. Fine del run e lettura

```sh
cd /Users/io/Dev/ios/ios-sweet-lg-g4
./tools/auto-display-test.sh
```

Lo script stampa il manifest, avvia con **`fastboot boot`** (mai flash),
aspetta TWRP e legge `pmsg-ramoops-0` / `console-ramoops-0`. Lo shim arma
il watchdog prima di toccare il display, imposta `LK_RECOVERY_MODE` e dopo
20 s lascia cadere PSHOLD: un hang diventa un reset, non un mattone.

Nel log cerca, nell'ordine: `BUILD id=`, `BOOT ...` (53 registri dal 16/11),
`HS dmactrl0/1` e `after0/1`, `TE trig0/1` e `after0/1`, `FR0/FR1/FR2/FRB`,
`DCS wait_ms= links=`, `GRAM packets=`, `BEAT n=`, `DISP result=`.

| Osservazione | Significato | Azione |
|---|---|---|
| D0: logo instabile | la baseline non è affidabile | fermarsi, nessun test dopo è interpretabile |
| D1: schermo cambia | LK scansiona ancora `cont_splash` | HELLO è risolto (ma D1 ha detto di no) |
| D1: logo fermo | nessun frame in uscita all'handover | **confermato il 15/11**, spiegato dal BOOT dump D4 → **D5**, non ritoccare i registri MDP |
| `BOOT RGB*.addr` != `0x03400000` | MDP punta altrove | leggere `RGB*.fmt`/`strd`, valutare IOVA/SMMU |
| `BOOT DSI1.ctrl` spento | metà pannello non pilotata | il bring-up deve coprire entrambi i link |
| `DCS timeout cmd=11` / `done0=00000000` | link DSI spento o DMA bloccato | serve il bring-up completo (mode 3); verificare l'indirizzo DMA |
| `done0=00000001 done1=00000000` | DSI0 risponde, DSI1 è muto | metà schermo: indagare clock/soft reset di DSI1 |
| **HELLO appare (due volte) + quadrato rosso/verde lampeggia** | **GRAM scrivibile** | la console si costruisce qui; MDP non serve più |
| HELLO appare ma illeggibile / sfasato | formato pixel non RGB888 | provare DCS `0x3A` (0x77 = 24bpp, 0x55 = 16bpp) |
| tutti `done0=1` ma schermo fermo | **D4, confermato**: completamento solo lato host | D5, poi D6 |
| `DCS wait_ms=0` su un pacchetto da 15 KiB | `CMD_DMA_DONE` non si è azzerato: bit stale | il clear del bit è da rivedere, tutti i `done` precedenti sono falsi positivi |
| `HS dmactrl0/1` con `BIT(26)` settato | LK aveva lasciato il DMA in LP | D5 lo azzera: `after0/1` deve perdere `0x04000000` |
| `TE after0/1 = 80000004` | i link ora hanno il valore del kernel | confrontare `FR0`/`FR1`/`FR2` |
| **`FR*` che avanza** | **MDP sta giocando frame**: prova numerica, indipendente dall'occhio | se i frame avanzano e lo schermo è fermo, il guasto è a valle (DSI/pannello) |
| **`FR*` fermo a 0** | MDP non parte nemmeno con TE selezionato | serve `CTL_START`/commit (mode 3) o il percorso DCS |
| D6: **schermo NERO** per 20 s | il pannello **ubbidisce ai DCS** | il problema è finestra/pagina/formato: lavorare sullo stream `0x2C`/`0x3C` |
| D6: logo fermo | i pacchetti non lasciano l'host | rivedere LP/HS, indirizzo DMA, trigger |
| `BEAT n=` avanza | la CPU è viva, è solo lo scan-out | confermato in D1 e D4 (40 beat) |

---

## 9. Aperto

1. ~~Il **BOOT dump** completo non era stato catturato~~ → **fatto il
   16/11 con D4** (37 registri; dal 16/11 il dump ne legge 53).
   `RGB0/RGB1 → 0x03400000`, stride 4320, `DSI_CTRL=0x1F5` su entrambi i
   link: la pipeline LK è viva e congelata, non rotta.
2. Il **SMMU MDP** non è stato verificato: se è agganciato servono IOVA.
   D4/D5 non ne hanno bisogno, D3 sì. Il dump del 16/11 rende l'ipotesi
   meno probabile (le pipe leggono l'indirizzo fisico atteso).
3. Il mapping corretto del bit di flush INTF (`BIT(29)` vs bit 3/4/30) va
   letto nel sorgente msm-3.18, non indovinato.
4. Il **TE** del pannello arriva su un GPIO che lo shim non instrada.
5. La sequenza DCS completa del JDI NT36850 non viene inviata (`0x11`, `0x29`,
   `0x2A`, `0x2B`, `0x2C`, `0x3C`): si eredita l'init del bootloader.
6. Il pannello resta su una **pagina comando produttore** (`FF 10` subito
   prima di `0x29`): non è ancora chiaro se `0x2A`/`0x2B` siano interpretati
   come DCS o come registri di pagina. D5 riduce il dubbio usando una sola
   finestra + `0x3C` continue, come fa l'host DSI del kernel.
7. **`CMD_DMA_DONE` non è un ack del pannello**: serve un **DCS read con BTA**
   (o il probe `0x28` del mode 6) per provare il collegamento bidirezionale.
8. **Traccia XNU, sempre aperta e prioritaria:** il kernel a 4 KiB
   (`xnu/patches/0085-sweet-4k-granule.patch`) non risulta ancora provato sul
   device. Il display non è la causa del mancato boot.

---

## 10. Sicurezza

Solo `fastboot boot`. Niente flash, niente `userdata`, niente scritture su
controller con clock spento. `tools/check-mmio-writes.py` deve restare verde
(viene lanciato da `build.sh`). Dopo 10 fallimenti sullo stesso punto ci si
ferma. Vedi `docs/INDEX.md §6` e `docs/HANDOFF-PROMPT.md §8`.
