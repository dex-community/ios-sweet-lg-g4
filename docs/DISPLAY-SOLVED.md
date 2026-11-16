# Display dello shim: soluzione, postmortem e metodo (2026-11-16)

**Stato: RISOLTO.** HELLO, numero di stage, etichetta e heartbeat sono visibili
sul pannello del LG G4 H815, scritti dallo shim bare-metal senza MDP, senza
SMMU e senza framebuffer in DDR. Conferma visiva dell'utente nei video
`signal-2026-11-16-01-15-17-866.mp4` e `..._002.mp4` (due copie di HELLO, una
per link DSI, `1`, `SHIM CONSOLE`, `BEAT 00000000`, barra colorata in cima).

Questo documento è la fonte autorevole. Raccoglie la ricetta che funziona,
tutti i tentativi falliti, la catena di ragionamento che ha sbloccato il
problema e un metodo riutilizzabile su altri device.

---

## 1. La ricetta che funziona

```sh
cd /Users/io/Dev/ios/ios-sweet-lg-g4
(cd shim && SHIM_DISPLAY_TEST=1 DISPLAY_MODE=11 DISPLAY_STAGE=3 ./build.sh)
./tools/auto-display-test.sh
```

`tools/auto-display-test.sh` usa solo `fastboot boot` (mai `flash`), attende
TWRP e legge `pmsg-ramoops-0` / `console-ramoops-0`. Lo shim arma il watchdog,
tiene il quadro 20 s e torna in recovery da solo.

Sequenza eseguita da `shim/display.c`, mode 11:

| Passo | Azione | Dettaglio misurato |
|---|---|---|
| 1 | `clocks_display_core()` | MMSS AHB/AXI/MDP/ESC0/ESC1/VSYNC. Obbligatorio prima di ogni MMIO display. |
| 2 | `dsi_force_hs_mode()` | `DSI_COMMAND_MODE_DMA_CTRL &= ~BIT(26)` su DSI0 e DSI1: `0x14000000` → `0x10000000`. LK lascia il motore DMA in LP; i pacchetti pixel richiedono HS. |
| 3 | DCS `0x11` sleep out, `0x29` display on | Inviati a **entrambi** i link, short write. |
| 4 | DCS `0x3A = 0x77` | `set_pixel_format` 24 bpp, coerente con `qcom,mdss-dsi-bpp = <24>`. |
| 5 | Finestra `0x2A 0..719`, `0x2B 0..2559` | **720 colonne**, una per metà pannello. È il punto che sblocca tutto. |
| 6 | Primo pacchetto `0x2C`, poi `0x3C` continue | 86 pacchetti da 30 righe intere (64800 B = 30 × 720 × 3). `DSI_CMD_BUF` 64 KiB a `0x50010000`. |
| 7 | Heartbeat | `gram_console_beat()` aggiorna solo 64 righe (3 pacchetti), non tutto il frame. |

**Nessun** registro MDP, CTL, RGB, LM, INTF, nessun `CTL_FLUSH`, nessun
`CTL_START`, nessun `MDP_SW_TRIGGER`, nessun indirizzo IOVA. Il percorso è:

```
CPU -> DRAM 0x20000000 -> DMA del controller DSI -> DCS 0x2C/0x3C -> GRAM del pannello -> vetro
```

Il buffer è `720×2560×3 = 5.529.600` byte (`0x546000`), dentro i 12 MiB di
`0x20000000`. Log atteso:

```
HS dmactrl0=14000000 dmactrl1=14000000
HS after0=10000000 after1=10000000
FULL packets=00000056 bytes=00546000
ACK dsi0=00000000 dsi1=00000000
DISP D11: HELLO streamed to panel GRAM
DISP result=000000B1
```

Build finale: `build_id=0x81469482`, mode 11, stage 3, bpp 24.
`image_sha256` cambia a ogni rebuild (il ramdisk contiene un timestamp);
`build_id` è stabile perché è l'hash di sorgenti e flag.

---

## 2. Cosa NON ha funzionato

| Mode / test | Ipotesi | Esito | Lezione |
|---|---|---|---|
| D0 baseline | il logo è stabile se non tocchiamo nulla | logo fermo | baseline affidabile: tutto il resto è interpretabile. |
| D1 minimal touch | LK scansiona ancora `cont_splash` (`0x03400000`) | 10,5 MiB scritti, 40 heartbeat, **zero pixel cambiati** | scrivere nel framebuffer non basta. Ma attenzione: la conclusione "quell'indirizzo non è sorgente dello scan-out" era **sbagliata** (vedi §3). |
| D4 DCS in GRAM | basta un `0x2C` diretto al pannello | 25 pacchetti, `done0/done1=1`, nessun cambiamento | `CMD_DMA_DONE` è completamento lato host, non ack del pannello. Inoltre finestra a 640 colonne e LP mode. |
| D5 riarmo MDP | HS + `te_sel` + kick MDP riattivano la pipeline | **prima immagine diversa**: static deterministico | il pannello risponde, ma i frame MDP sovrascrivono la GRAM. |
| D6 `0x28` display off | verifica binaria del collegamento DCS | **schermo nero per 20 s** | prova decisiva: i DCS raggiungono il pannello. |
| D7 `0x3A` 24bpp + HELLO | il static è un formato pixel sbagliato | static identico | ipotesi parziale: il formato contava, ma la causa principale era la finestra. |
| D8 barre colorate in `cont_splash` | MDP legge il nostro buffer | static / nessuna barra | la via MDP è un vicolo cieco per questo obiettivo. |
| D9 DCS senza kick MDP | i frame MDP sovrascrivono HELLO | static identico | i kick non erano la causa: la finestra sì. |
| D10 full frame rosso | la GRAM è scrivibile su tutta la superficie | **rosso pieno**, dall'alto a sinistra verso il basso a destra | svolta. Finestra 720 colonne + `0x3A` + HS + `0x3C`. |
| D11 console | HELLO con la stessa geometria di D10 | **HELLO visibile** | risolto. |

Anche i sette test storici del 15/11 (archiviati in `old/displayTest.md`)
avevano tentato `CTL_LAYER`, `CTL_TOP`, `CTL_FLUSH`, RGB2, cache clean, doppio
trigger DSI e autorefresh senza mai cambiare un pixel. Erano tutti dentro
l'ipotesi sbagliata "il guasto è nella pipeline MDP".

### Bug di codice trovati e corretti lungo la strada

1. `disp_draw()` scriveva `0xE10000` byte in una finestra da `0xC00000`
   (2,06 MiB fuori). Corretto con `disp_fill_bounded()`.
2. `disp_dump()` aveva 24 slot e 34 `REG()` → overflow dello stack.
   Ora `DISP_DUMP_MAX 64` con bounds check; 53 registri letti.
3. DCS long-write saltava l'ultimo byte del payload: `buf[4]` è il comando,
   il payload parte da `buf[5]`, `WC = len + 1`.
4. Comandi DCS inviati solo a DSI0. Il kernel manda lo stesso `dma_addr` a
   entrambi i controller (`mdss_dsi_host.c:1769-1781`).
5. `CMD_DMA_DONE` (`DSI_INT_CTRL` bit 0) è write-one-to-clear e non veniva
   azzerato: i "completamenti" potevano essere bit stale. Ora clear prima di
   ogni comando, `wait_ms=` e bitmask dei link nel log.
6. `DSI_CMD_BUF` era 256 B senza controllo di dimensione. Portato a 64 KiB.
7. Finestre DCS strette (640 o 32 colonne) su pannello da 720: ogni riga
   traslata di `(720-640)×3 = 240` byte → il static deterministico.
8. Fill a 32 bit con byte diversi (`0x10182020`) su framebuffer a 3 B/pixel
   → colonne colorate. Ora word con byte uguali o pattern di 3 word su 12 byte.
9. `GRAM_BEAT_FB` cadeva dentro il framebuffer full e ne sovrascriveva la
   riga ~178. Spostato dopo `GRAM_SCRATCH`.
10. Mode 2 scriveva `MDP_SW_TRIGGER` a clock MDSS spenti (hang potenziale).
    Ora `clocks_display_core()` prima.

---

## 3. La catena logica che ha sbloccato il problema

Il punto non è stato "provare più registri": è stato **cambiare la domanda**
quando una misura contraddiceva il modello.

1. **D0/D1 hanno stabilito un fatto negativo solido**: il codice gira, la CPU
   è viva (40 heartbeat), il disegno avviene, eppure il vetro non cambia.
   Quindi il problema non è il contenuto del buffer né un crash.
2. **D4 ha fornito il BOOT dump** letto a clock accesi, la misura mancante in
   tutti i test precedenti: `RGB0/RGB1 → 0x03400000`, stride `0x10E0` = 4320,
   `DSI.ctrl = 0x1F5` su **entrambi** i link, `clkctl = 0x23F`, `RGB2 = 0`.
   La pipeline MDP di LK è viva e configurata correttamente: è **ferma**, non
   rotta. Questo ha corretto la conclusione affrettata di D1.
3. **D6 ha isolato il canale**: un solo comando `0x28` ha spento il pannello.
   Risposta binaria, non interpretabile: il collegamento DCS esiste.
   Tutte le ipotesi "i pacchetti non partono dall'host" sono morte qui.
4. **D10 ha isolato la geometria**: stesso canale, ma finestra a 720 colonne
   e frame intero → rosso pieno, scritto progressivamente riga per riga.
   Confronto con D4/D7 (finestra 640) → il static deterministico era uno
   **sfalsamento di riga**, non rumore e non un problema di formato pixel.
   La costanza dell'immagine a ogni avvio era il segnale chiave: il rumore
   vero non è riproducibile, uno shift di stride sì.
5. **D11 ha riutilizzato la geometria vincente** senza cambiarla: stesso
   codice di D10, solo pixel diversi. HELLO è apparso al primo colpo.

Formula generale: **trova il canale che risponde, poi riduci le variabili
finché una sola differenza separa il caso che funziona da quello che non
funziona.** D10 e D4 differivano per finestra, formato, HS e `0x3C`; D10 e D11
differivano solo per i byte inviati.

### Errori di ragionamento da non ripetere

- Trattare `CMD_DMA_DONE` come prova che il pannello abbia eseguito il comando.
  È il DMA dell'host che ha finito di riempire il FIFO. Serve un effetto
  osservabile (`0x28` → nero) o un read con BTA.
- Dedurre "MDP non legge questo indirizzo" da "scrivere qui non cambia nulla".
  Il BOOT dump ha mostrato che l'indirizzo era giusto: mancava il frame.
- Inseguire `CTL_FLUSH` come metrica di successo. Sul kernel funzionante è
  `0x40000018` con 27 frame e 0 underrun: bit pendenti sono normali.
- Riconfigurare MDP (RGB2, `CTL_TOP`, `CTL_LAYER`) sperando di ripararlo,
  quando era l'unica parte già corretta.
- Cambiare più variabili per volta. D5 combinava HS, `te_sel`, pittura DDR,
  pittura GRAM e kick: ha prodotto un'immagine nuova ma non attribuibile.

---

## 4. Playbook per un altro device

Valido per qualsiasi SoC con pannello DSI command-mode e bootloader che
lascia uno splash. Ogni passo produce un sì/no; non si avanza senza il sì.

**Fase 0 — Rendere attribuibile ogni run.**
Compilare un `build_id` da hash di sorgenti e flag dentro il binario, scrivere
un manifest con `image_sha256`, stamparlo come prima riga del log. Senza questo
nessuna misura è confrontabile.

**Fase 1 — Scegliere un canale di log indipendente dal vetro.**
pstore/ramoops, scratch RAM, IMEM, marker ring. Il display è l'ultima cosa da
usare per diagnosticare il display.

**Fase 2 — Baseline.** Un run che non tocca nulla. Se il quadro non è stabile,
fermarsi: tutto il resto è ininterpretabile.

**Fase 3 — Fotografare lo stato ereditato, non indovinarlo.**
Leggere, a clock accesi e prima di ogni scrittura: `DSI_CTRL`, `CLK_CTRL`,
`TRIG_CTRL`, `DMA_CTRL`, `INT_CTRL`, `ACK_ERR_STATUS` su **ogni** link;
`SSPP_SRC0_ADDR / FORMAT / YSTRIDE / SIZE` di ogni pipe; `CTL_TOP`, `CTL_FLUSH`,
`INTF_*`, pingpong/TE. Confrontare con un dump del kernel funzionante dello
stesso device. Le differenze sono la lista delle ipotesi.

**Fase 4 — Provare il pannello con un comando binario.**
Prima di disegnare, inviare `0x28` (display off) o un DCS read con BTA.
Schermo nero / byte letto = il link comanda il pannello. Se non succede, il
problema è a monte: clock, LP/HS, indirizzo DMA, trigger, page select.

**Fase 5 — Riempimento pieno con la geometria esatta.**
Leggere `panel-width` e `bpp` dal device tree del **pannello**, non dal
framebuffer del kernel. Impostare `0x3A`, finestra `0..larghezza-1` ×
`0..altezza-1`, primo pacchetto `0x2C`, successivi `0x3C`, sempre righe
intere, colore solido. Se appare un colore pieno, il canale è acquisito.

**Fase 6 — Solo ora il contenuto.** Font, stage, heartbeat. Aggiornare per
regioni, non riscrivere tutto il frame.

**Fase 7 — Ottimizzare.** Pacchetti più grandi (fino al limite WC a 16 bit,
qui 30 righe = 64800 B), fill a 32 bit con pattern coerente col bpp, cache
clean limitato alla regione, DMA buffer 4 KiB-allineato.

### Segnali diagnostici universali

| Osservazione | Causa probabile |
|---|---|
| Schermo immutato ma CPU viva e log completi | nessun frame in uscita, non contenuto del buffer |
| Immagine "rumorosa" **identica a ogni avvio** | disallineamento stride/bpp/window, non rumore casuale |
| Comandi "completati" senza effetto | bit di done lato host, non ack del pannello; oppure LP invece di HS |
| Il quadro cambia e poi torna come prima | un frame MDP/TE sovrascrive la GRAM |
| Metà schermo funziona | dual-DSI: un link non riceve il comando |
| Colori a strisce verticali | word a 32 bit con byte diversi su framebuffer a 3 B/pixel |
| Il pannello si colora progressivamente | normale: la GRAM si aggiorna mentre arrivano i pacchetti |
| Un solo colore appare, il testo no | geometria corretta, font/stride del testo da rivedere |

### Regole di sicurezza (non negoziabili)

- Solo `fastboot boot`. Mai `flash boot`, mai `misc`/`aboot`/`tz`/`rpm`/`hyp`/
  `persist`/`modem`/`laf`, mai `userdata`.
- Watchdog armato prima del primo MMIO display; `LK_RECOVERY_MODE` per tornare
  in recovery da soli.
- Clock del controller acceso prima di qualunque accesso al suo blocco:
  una scrittura a clock spento non completa e la CPU si ferma sul bus AXI
  (due hang il 2026-11-09).
- Allowlist MMIO per ogni scrittura, con audit statico in `build.sh`.
- Dopo 10 fallimenti sullo stesso punto, fermarsi e rileggere le misure.

---

## 5. Confronto con l'altro kernel G4

`https://github.com/garwedgess/android_kernel_lge_g4`, branch `M`, confrontato
con il kernel locale `lg g4/G4-Titan-Kernel-titan_bpf2` (clone sparse in
`/tmp/g4-garwedgess`).

**Identici** (zero differenze funzionali): `mdss_dsi_host.c` (2686 righe),
`mdss_dsi_cmd.c` (709), `mdss_dsi.h` (658), `dsi_host_v2.c` (1851); nel solo
`mdss_dsi_host.c` cambia `%pK` → `%p` in un `pr_debug`. Il DTS del pannello
`dsi-panel-jdi-nt36850-dualmipi0-qhd-cmd.dtsi` è identico tranne la curva
`qcom,blmap` (256 valori di luminosità).

**Diverse**: `mdss_fb.c` (1086 righe di diff), `mdss_mdp_overlay.c` (729),
`mdss_mdp_pp.c` (603), `mdss_panel.c` (582), `mdss_dsi_panel.c` (417: il
kernel locale aggiunge LGE reader mode, extended panel, livedisplay),
`mdss_mdp_intf_cmd.c` (62: hack `lge_panel_send_off_cmd` sul kernel GitHub).

**Conclusione**: nessuna differenza sul bring-up DSI, sul formato pixel o sui
registri che abbiamo usato. Il kernel alternativo non cambia la diagnosi né la
soluzione. Le differenze sono feature di panel tuning post-boot.

---

## 6. Stato e prossimo passo

Display: **chiuso**. Traccia XNU: **aperta e prioritaria**. Il mancato boot di
XNU non è mai dipeso dal display: il kernel muore dentro `MSR SCTLR_EL1`
perché il Cortex-A53 del G4 non ha la grana 16 KiB
(`NOTES.md:4165-4214`, `xnu/patches/0085-sweet-4k-granule.patch`).

Il 16/11 è stato recuperato l'artefatto CI `43313974215` (run `success` del
12/11 21:49, mai scaricato prima) in `xnu/out-43313974215/`.
`tools/check-xnu-binary.sh` su quel binario dà:

```
FAIL  TCR_EL1 0x002a51a161a (4 KiB granule) NOT in the binary
ok    the 16 KiB TCR_EL1 0x0226511a511 is gone
ok    __TEXT vmaddr 0xfffffff007004000 (37 bit kernel address space)
ok    sweet_mark is in the symbol table
ok    30 boot marker immediates (0x5eed....)
ok    no LSE atomics
```

Quindi il link address è corretto e il vecchio TCR 16 KiB è sparito, ma il
valore atteso del TCR 4 KiB non compare come costante: va verificato se la
patch 0085 lo costruisce a runtime. Poi un `fastboot boot` e
`tools/read-pram.py --marks`: go = marker 12 presente.
