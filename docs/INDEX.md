# Indice della documentazione — stato di verità

**Aggiornato: 2026-11-16 (mode 13, hardware).** Questo file è il punto di ingresso. Prima di usare
un documento, controlla qui il suo stato: nel progetto convivono misure vere,
ipotesi superate e descrizioni di codice che non esiste più.

---

## 1. Stato corrente (colpo d'occhio)

| Tema | Stato | Fonte |
|---|---|---|
| Shim | gira sul telefono, log in pstore / RAM `0x50000000` / IMEM | `NOTES.md`, `shim/log.h` |
| eMMC | CMD2 risolto (`C2OK v=00000009`), rootfs HFS+ montato ro da Linux | `NOTES.md`, `docs/test-2026-11-10-hfs-mount.txt` |
| XNU | **arriva a `common_start` e muore dentro `MSR SCTLR_EL1`**: il Cortex-A53 non ha la grana 16 KiB | `NOTES.md:4165-4214` |
| Fix XNU | `xnu/patches/0085-sweet-4k-granule.patch` + limite utente iOS (`23f0de5`) | ultimo commit `4418684`, 2026-11-12 |
| Kernel 4 KiB | **VERIFICATO corretto** il 16/11: artefatto CI `43313974215`, `TCR_EL1 0x022a51a251a` (TG0=4 KiB), `__TEXT 0xfffffff007004000`, zero LSE. Il vecchio FAIL era un bug del checker. **Boot device ancora da eseguire** | `tools/check-xnu-binary.sh`, `docs/DISPLAY-SOLVED.md` |
| **Display** | **RISOLTO il 16/11, mode 13**: frame spinto da MDP/DSI DMA (hardware), istantaneo, log boot step-by-step a schermo, `GPU verdict=HW`. HELLO rimosso. Dati in `shim/display-data.h` | **`docs/DISPLAY-SOLVED.md`**, `shim/display.c` |
| Causa dello stallo | finestra DCS a 640 colonne su pannello da 720: ogni riga sfalsata di 240 byte → static deterministico. Non era MDP, non era SMMU, non era formato pixel | `docs/DISPLAY-SOLVED.md §3` |
| Causa del mancato boot | **non è il display** | `NOTES.md`, `HANDOFF-PROMPT.md` |
| Test display | mode 0..11; **11 è la soluzione**, 0..10 sono la bisection storica | `docs/DISPLAY-TEST.md` |
| Documenti superati | spostati in `old/` il 16/11 | mappa qui sotto |
| Sicurezza | solo `fastboot boot`, watchdog armato, niente `userdata` | `README.md`, `HANDOFF-PROMPT.md §8` |

**Stato delle tracce.** Display chiuso. Critical path = verifica del kernel a
4 KiB sul device. Le due tracce non sono mai state in competizione: il display
dava la console visibile allo shim, il marker ring dà il progresso di XNU.

---

## 2. Mappa dei documenti

| Documento | Stato | Usalo per | Non usarlo per |
|---|---|---|---|
| **`docs/INDEX.md`** | **autorevole** | orientarsi, sapere cosa è vero oggi | — |
| **`docs/DISPLAY-SOLVED.md`** | **autorevole (16/11)** | la soluzione: ricetta, tentativi falliti, ragionamento, metodo per altri device | — |
| **`docs/DISPLAY-TEST.md`** | autorevole come riferimento operativo | i 12 mode, comandi di build, interpretazione dei log | la diagnosi (è in DISPLAY-SOLVED.md) |
| `docs/DISPLAY-FINDINGS.md` | **misurato 10/11**, ancora valido | rgb2/intf2, IOVA, SMMU, CTL_FLUSH, formato | lo stato del display (cambia) |
| `docs/display-2026-11-10.txt` | **dato grezzo** | valori live del kernel funzionante | — |
| `docs/HANDOFF-PROMPT.md` | storico, con aggiornamenti fino al 12/11 | regole di sicurezza, indirizzi, strade morte | "prossimi passi" (superati) |
| `NOTES.md` | **diario primario**, append-only | ogni misura, crash, decisione | una sintesi |
| `STATUS.md` | aggiornato 16/11 (display chiuso) | cronologia delle fasi | lo stato del display |
| `README.md` | aggiornato 16/11 | architettura, safety rules, hardware | "current blocker" (11/11) |
| `docs/SHIM-ARCHITECTURE.md` | da leggere con il banner in testa | struttura moduli, i due hang | la sezione display (descrive un codice precedente) |
| `docs/BOOT-RECOVERY.md`, `docs/XNU-BOOT-TEST.md`, `docs/LSE-PAN-FIX-2026-11-11.md` | procedure vive | recovery, ciclo di boot XNU, fix LSE/PAN | — |
| `docs/g4-hardware/` | **fatti device tree** | indirizzi e costanti con file:line | — |
| `old/` | **ARCHIVIO** (16/11) | la cronologia: `PIANO-2026-11-15-diagnosi.md` (piano iniziale), `displayTest.md` (i 7 test), `PLAN-2026-11-10.md`, `DEVICE-TEST.md`, `QEMU_TESTING.md` | prendere decisioni da qui |
| `RECUPERO.md` | storico | ripristino LAF/KDZ | diagnosi corrente |

Regola: **una misura senza `build_id` e senza hash dell'immagine non è
attribuibile.** Dal 15/11 `shim/build.sh` scrive `shim/out/run-manifest.txt`
e stampa `BUILD id=...` come prima riga del log.

---

## 3. HELLO statico vs console a schermo (la correzione al piano)

Nel piano del 15/11 avevo scritto che con HELLO "non c'è modo di distinguere
'fermo al passo 3' da 'fermo al passo 7'". **Era troppo assoluto e va corretto.**

- Un **HELLO statico**, disegnato una volta, dice solo "lo shim è arrivato
  alla `disp_draw()`". Quello da solo non localizza un blocco successivo.
- Una **console a framebuffer**, aggiornata a ogni passo, invece lo fa
  benissimo: se ogni stage ridipinge il proprio numero **prima** di toccare
  l'hardware, un hang sul passo 3 lascia "3" sul pannello, uno sul passo 7
  lascia "7". Con un heartbeat che avanza nel loop di attesa si distingue
  anche "CPU appesa" da "crash".
- Il codice per i log a schermo **esisteva già**: `console.c` (`con_str` →
  `put_str`) dipinge ogni riga di log in `con_fb`, e i log del device dicono
  `FB cont_splash_mem` / `FB at=03400000 size=00C00000`
  (`docs/shim-log-2026-11-10.txt:1-3`): **lo shim scrive il log dentro
  `cont_splash` dal 10/11**. Eppure il logo LG non è mai cambiato. Questo è
  un indizio forte: il problema non è il contenuto del buffer ma la **catena
  di refresh** (o un formato pixel diverso da quello con cui il console
  dipinge). È il motivo per cui D0/D1 da soli probabilmente non basteranno e
  il BOOT dump + la verifica SMMU contano più di altri ritocchi ai registri.

Quindi l'intuizione è giusta, con due precisazioni tecniche:

1. Il pannello è **DSI command-mode**: scrivere in RAM non basta, ogni
   aggiornamento va seguito da un kick (`dsi_soft_trigger()` su DSI0+DSI1).
   Il loop di hold lo fa ogni 500 ms.
2. La console a schermo copre i **log dello shim**. I log di **XNU** restano
   fuori finché XNU non ha un suo driver di framebuffer: dopo il salto il
   codice dello shim non gira più. Per XNU il canale resta l'anello di marker
   in RAM (`tools/read-pram.py --marks`).

**Conseguenza operativa (aggiornata dopo D4):** la console a schermo è il
*premio* di uno scan-out funzionante, non lo strumento per ottenerlo. D0/D1
hanno chiuso la prima ipotesi (scrivere in `cont_splash` non basta). **D4 ha
cambiato il quadro**: il BOOT dump mostra che la pipeline MDP di LK è **viva
e correttamente configurata** su `0x03400000`, con entrambi i link DSI
accesi. Non è rotta: è **congelata** perché manca un frame. I test 1–7
l'avevano ricostruita (RGB2, `CTL_TOP`, `CTL_LAYER`), rompendo la config
buona. Due differenze misurate rispetto al kernel: `DSI_TRIG_CTRL` senza
`BIT(31)` (`te_sel`) e il motore DMA DCS in LP invece che HS. `CMD_DMA_DONE`
è un completamento **lato host**, non un ack del pannello: per questo D4 ha
potuto loggare 25 pacchetti e 40 heartbeat senza muovere un pixel.

Il **mode 5** non ricostruisce nulla: dipinge sia la DDR sia la GRAM, azzera
`BIT(26)` (HS), imposta `BIT(31)` (valore del kernel `0x80000004`) su
entrambi i link e kikka un frame MDP ogni 500 ms. I contatori `FR*` dicono
numericamente se i frame partono. Il **mode 6** è il fallback binario:
`0x28` display off → schermo nero = il pannello ubbidisce ai DCS.
Dettagli in `docs/DISPLAY-TEST.md §2-bis`.

---

## 4. Contraddizioni chiuse il 15/11

| Contraddizione | Risoluzione |
|---|---|
| `build.sh` diceva "display OFF by default" ma impostava `1`, e `SKIP_EMMC` usava un secondo default | un solo `DISP_TEST` (default `0`); `SKIP_EMMC` ne deriva |
| `DISPLAY_STAGE=1..5` documentato ma mai referenziato | ora `#if SHIM_DISPLAY_STAGE >= N` in `display_test()` |
| `display_test()` era "VARIANTE B" e non disegnava HELLO | modalità esplicite; HELLO + numero di stage + heartbeat |
| `disp_draw()` scriveva `0xE10000` byte in una finestra da `0xC00000` | `disp_fill_bounded()` limita rows a `size/stride` |
| dump registri: 24 slot, 34 `REG()` → overflow dello stack | `DISP_DUMP_MAX 40` + bounds check |
| testo disegnato a 32 bpp su pipe ereditate RGB888 | `con_draw_text_bpp()` sceglie il formato a runtime |
| `CTL_FLUSH` usato come metrica di riuscita | il poll è corretto (`(v & mask)==0`) ma è solo informativo: pending è normale su questo SoC |
| `g4.h` dice "scrivere `0x03400000` = data abort" | empiricamente falso all'handover: `console.c` ci scrive da sempre e il test 5 non ha fault |
| `old/displayTest.md` suggeriva `fastboot flash boot` | rimosso: solo `fastboot boot` |
| DCS long-write saltava l'ultimo byte del payload | `buf[4]` = comando, payload da `buf[5]`; WC conta anche il comando. Verificato contro `mdss_dsi_cmd.c:96-135` |
| comandi DCS inviati solo a DSI0 | `dsi_both_w()`: il kernel manda lo stesso `dma_addr` a entrambi i controller (`mdss_dsi_host.c:1769-1781`) |
| `CMD_DMA_DONE` mai azzerato e poll solo su DSI0 | clear write-one-to-clear prima di ogni comando, `done0`/`done1` separati, `-1` su timeout, buffer DMA portato a 16 KiB con bound |
| mode 2 scriveva il trigger DSI a clock spenti (hang potenziale) | `clocks_display_core()` prima di qualunque scrittura DSI + BOOT dump |
| D0 loggava "HELLO + stage on screen" pur non dipingendo nulla | messaggi dipendenti dal risultato in `shim.c` e dalla modalità letta dal manifest in `auto-display-test.sh` |
| BOOT dump leggeva solo DSI0 su un pannello dual-DSI | aggiunti `DSI1.ctrl` / `DSI1.clkctl` / `DSI1.trig`; dal 16/11 anche `dmactl/status/fifo/ackerr/intctl/rdbk0` su entrambi, contatori `INTF*.frmc`, `PP0.tear/autorf` (53 registri) |
| `CMD_DMA_DONE` trattato come "il pannello ha risposto" | è solo il completamento del DMA lato host: log con `wait_ms=` + link bitmask, dump degli errori ACK, mode 6 come probe di contatto |
| `0x2C` usato per ogni pacchetto con finestra ripetuta | una sola finestra, primo `0x2C` e poi `0x3C` continue, come l'host DSI del kernel (`dcsctl=0x00013C2C`) |
| DCS inviato in LP con pacchetti da 15 KiB | mode 5 forza HS (`DSI_COMMAND_MODE_DMA_CTRL &= ~BIT(26)`, `dsi_host_v2.c:513-525`) |
| `DSI_TRIG_CTRL` lasciato a `0x00000004` | mode 5 imposta `BIT(31)` = `te_sel`, portandolo al `0x80000004` misurato sul kernel (`mdss_dsi_host.c:369-370`) |

---

## 5. Ciclo di test (nessun flash)

```sh
cd /Users/io/Dev/ios/ios-sweet-lg-g4

# === LA SOLUZIONE: D11, HELLO sul pannello ===
(cd shim && SHIM_DISPLAY_TEST=1 DISPLAY_MODE=11 DISPLAY_STAGE=3 ./build.sh)
./tools/auto-display-test.sh
```

I mode 0..10 sono la bisection già eseguita il 15-16/11 (`build_id`
`0xa3df8faa`, `0x845d155c`, `0x42d146b9`, `0x954caa7d`, `0x4508258f`): non
serve ripeterli. L'immagine in `shim/out/` è già **D11 stage 3**
(`build_id=0x81469482`).

Ogni build produce `out/shim-boot.img`, `out/run-manifest.txt` e stampa
`BUILD id=...` nel pstore: **riporta sempre build_id + image_sha256 nel
report**, altrimenti i test non sono confrontabili.

---

## 6. Sicurezza (non negoziabile)

- Solo `fastboot boot`. Mai `fastboot flash boot`, mai `misc`/`aboot`/`tz`/
  `rpm`/`hyp`/`persist`/`modem`/`laf`, mai riscrivere `userdata`.
- Watchdog armato prima del primo MMIO del display; `LK_RECOVERY_MODE` per
  tornare in recovery da soli.
- Clock del controller acceso **prima** di qualunque accesso al suo blocco
  (tre hang already documentati).
- `tools/check-mmio-writes.py` deve restare verde (lo lancia `build.sh`).
- Dopo 10 fallimenti sullo stesso punto: fermarsi e chiedere.

---

## 7. Prossime azioni

**Traccia display: CHIUSA.** Il mode 11 scrive HELLO, stage, label e
heartbeat sul pannello. Nessun'altra azione display è necessaria; se in
futuro si tocca il codice DSI, leggere prima `docs/DISPLAY-SOLVED.md` (che
contiene anche i dieci bug di codice trovati e le trappole da evitare).

**Traccia XNU (critical path, ora unica):** verificare il binario a 4 KiB e
provarlo sul device.

1. L'artefatto CI `43313974215` (run `success` del 12/11 21:49) è stato
   recuperato in `xnu/out-43313974215/`. `tools/check-xnu-binary.sh` dà:
   `__TEXT vmaddr 0xfffffff007004000` **ok**, vecchio `TCR_EL1` 16 KiB
   **rimosso**, ma `TCR_EL1 0x002a51a161a` (4 KiB) **non presente come
   costante**. Da capire se la patch 0085 lo costruisce a runtime o se il
   binario è precedente alla patch.
2. Se il TCR è costruito a runtime: `fastboot boot` del payload kernel e
   `tools/read-pram.py --marks`. **Go = marker 12 presente.**
3. Se il TCR non è nel binario: triggerare il workflow `xnu.yml` e usare
   l'artefatto `xnu-kernel` del run nuovo.
