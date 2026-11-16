# Piano di diagnosi — 2026-11-15

**Domanda:** è realizzabile un "HELLO" visibile a schermo per rendere lo shim
chiaro, vedere log/errori e capire se il boot si è bloccato? È lo step giusto?

**Verdetto iniziale:** realizzabile in linea di principio, ma non è lo step
giusto per sbloccare il boot di XNU: su questa board il display è il
sottosistema più difficile (7 test già falliti), mentre i canali per vedere i
log esistono già. La causa del mancato avvio di XNU è stata **misurata** e non
è il display.

> **CORREZIONE (15/11, sera).** La formulazione qui sotto era troppo assoluta:
> diceva che con HELLO "non c'è modo di distinguere il passo 3 dal passo 7".
> Vale solo per un **HELLO statico**. Una **console a framebuffer aggiornata a
> ogni stage** distingue eccellentemente i due casi, e il renderer dei log a
> schermo esisteva già (`console.c`: `con_str` → `put_str`). L'utente ha ragione.
> Il codice è stato quindi modificato per implementarla: vedi §3-bis e
> `docs/DISPLAY-TEST.md`. Restano valide le due precisazioni: su un pannello
> DSI command-mode ogni aggiornamento richiede un kick, e la console copre i
> log dello **shim**, non quelli di XNU (per quelli resta il marker ring).
> **Decisione dell'utente:** procedere comunque sul display come traccia
> dichiarata. Le due tracce restano separate, non in competizione.

> **AGGIORNAMENTO (16/11, dopo D4).** D0, D1 e D4 sono stati eseguiti:
> tutti riusciti, **nessun pixel cambiato**. D4 ha però finalmente catturato
> il **BOOT dump**: la pipeline MDP di LK è **viva e congelata** (RGB0/RGB1
> ancora su `0x03400000`, entrambi i DSI a `0x1F5`). Non va ricostruita, va
> **riarmata**: è il **mode 5** (HS + `te_sel` + kick, zero scritture MDP).
> Il **mode 6** (`0x28`) è il probe binario di contatto. Vedi §3-quater, §5
> e `docs/DISPLAY-TEST.md §2-bis`.

---

## 1. La separazione da fare

| Obiettivo | Strumento giusto | Serve il pannello? |
|---|---|---|
| "Lo shim è partito?" | pstore / RAM `0x50000000` / trail IMEM | no |
| "Dove si è fermato?" | `PHASE()`/`stage`, marker RAM di XNU, `read-pram.py --marks` | no |
| "È crashato o è appeso?" | heartbeat + vettori di eccezione (ESR/ELR/FAR) | no |
| "Voglio una console leggibile a occhio" | framebuffer + font 8×16 | **sì** |

Un "HELLO" statico dimostra solo che lo shim è arrivato fino alla `disp_draw()`:
non localizza un blocco successivo. Ma una console **dinamica** sì: se ogni
stage ridipinge il proprio numero prima di toccare l'hardware, un hang sul
passo 3 lascia "3" e uno sul passo 7 lascia "7". È esattamente l'equivalente
a schermo del marker ring in RAM, con il vantaggio che non serve riavviare e
leggere la memoria: l'ultima riga resta visibile anche quando la CPU è appesa.
Il prezzo è che **serve prima uno scan-out funzionante**: per questo la
bisection iniziale si legge ancora dal pstore, non dallo schermo.

| | HELLO statico | console dinamica (implementata) |
|---|---|---|
| "lo shim è partito" | sì | sì |
| "passo 3 o passo 7" | no | **sì** |
| "hang o crash" | no | **sì** (heartbeat) |
| log di XNU | no | no (serve un driver FB di XNU) |
| funziona senza scan-out | no | no |

**La premessa che ha generato il filone display è superata.**
`docs/DISPLAY-TEST.md:5-8` e `shim/display.h:6-9` dicono: "XNU muore senza log,
ipotesi: LK lascia i clock MDSS/DSI spenti, quindi niente console".
`NOTES.md:4165-4214` e `docs/HANDOFF-PROMPT.md` (aggiornamento 12/11) hanno
misurato il contrario: XNU arriva a `common_start`, scrive `TCR_EL1`/`TTBR`/`MAIR`,
e **muore dentro `MSR SCTLR_EL1`** perché il kernel chiede una grana da 16 KiB
che il Cortex-A53 del G4 non implementa (il core risponde 4 KiB, le tabelle
bootstrap vengono percorse con shift sbagliati, bus hang, nessuna eccezione).
La cura è già scritta: `xnu/patches/0085-sweet-4k-granule.patch` + il limite
dello spazio utente iOS (`23f0de5`). **Non risulta alcun boot su device
documentato con il kernel a 4 KiB**: l'ultima misura registrata è ancora quella
del kernel 16 KiB. `STATUS.md:21` classifica il display come Fase 4, "bloccata
su working XNU boot".

---

## 2. Perché i test del 15/11 non sono interpretabili

Ricognizione sul working tree (nessun file modificato, nessun comando sul telefono).

### 2.1 Il binario corrente non disegna HELLO

- `shim/display.c:1168-1220` — `display_test()` è la **"VARIANTE B"**: riempie
  `0x03400000` di ciano, fa un dump read-only, manda un DSI SW trigger, e basta.
  **Nessun HELLO, nessun log a schermo, nessun init di clock/DSI/MDP/pannello.**
- `fb_text()` / `fb_fill()` / `fb_px()` (`display.c:1018-1051`) sono **codice morto**:
  definite ma mai chiamate. Il font 8×16 esiste (`console.c:119`).
- `disp_clocks_enable`, `dsi_pll_enable`, `disp_mdp_init` sono definite ma **non
  richiamate** da `display_test()`. `SHIM_DISPLAY_STAGE` non è referenziato da
  nessuna parte in `display.c`: il bisection `DISPLAY_STAGE=1..5` documentato in
  `DISPLAY-TEST.md:33-49` **non ha effetto** nel codice attuale.
- `display_test()` ritorna solo `DISP_OK_DRAWN` (`display.h:65` = `0x10`), mentre
  `old/displayTest.md:440` riporta `DISP stage=0000003F` (tutti gli stage). **I log in
  `old/displayTest.md` non provengono dal binario che oggi c'è nel tree.**
- `tools/auto-display-test.sh:6,41` annuncia "HELLO + on-screen logs": falso per
  il codice corrente. Stessa cosa per `SHIM-ARCHITECTURE.md:99-118`.

### 2.2 Due default contraddittori in `build.sh`

- `shim/build.sh:92-94` (commento) e `DISPLAY-TEST.md:23`: "flag OFF by default".
- `shim/build.sh:103`: `-DSHIM_DISPLAY_TEST=${SHIM_DISPLAY_TEST:-1}` → **ON** di default.
  Un `./build.sh` "normale" produce quindi uno shim che **non salta a XNU**.
- `shim/build.sh:109-115`: `SKIP_EMMC` usa `${SHIM_DISPLAY_TEST:-0}`, che vale `0`
  quando la variabile non è impostata → `SKIP_EMMC=0`. La fase eMMC (quella
  dell'hang del 15/11, `SHIM-ARCHITECTURE.md:58-81`) gira comunque, nonostante il
  commento dica il contrario.

Conseguenza: **ogni risultato va associato a hash dell'immagine + flag esatti +
versione del sorgente**, altrimenti non è attribuibile.

### 2.3 Il fill attuale esce dalla finestra cont_splash

- `disp_draw()` (`display.c:1057-1086`) scrive `1440*2560*4 = 0xE10000` byte a
  `0x03400000`, cioè fino a **`0x04210000`**.
- `cont_splash_mem` è `0x03400000 + 0xC00000` = 12 MiB, fine `0x04000000`
  (`shim/g4.h:85`, `shim/mmio.h:141`). Sono **~2,06 MiB scritti fuori** dalla
  finestra, e anche il loop di cache-clean (`display.c:1080`) esce fuori.
- `old/displayTest.md:222-223` lo dice esso stesso: 12 MiB non bastano per un frame
  RGBA8888. Il test "minimal touch" proposto in `old/displayTest.md:550-560` è invece
  correttamente limitato a `0x300000` word = 12 MiB: **quello** è il limite da usare.

### 2.4 Formato pixel incompatibile con la config ereditata

`old/displayTest.md:229-241` (e `DISPLAY-FINDINGS.md:52-58`): all'handover il
bootloader ha **RGB0 + RGB1**, `720×2560`, **RGB888** (`0x004037ff`), entrambe a
`0x03400000`. Il kernel working usa invece **RGB2**, `1440×2560`, **RGBA8888**,
stride `5760`, con indirizzo **IOVA** `0x00E30000`.
Il codice (`display.c:1050`) disegna sullo splash con stride `5760` e pixel a
32 bit: anche se lo scanout del bootloader leggesse quella memoria, il risultato
sarebbe spazzatura. Prima di "HELLO" va deciso **quale delle due configurazioni**
si sta usando.

### 2.5 Contraddizioni da chiudere con una misura, non con un'ipotesi

| Fonte A | Fonte B | Da risolvere |
|---|---|---|
| `g4.h:191-197`: scrivere `0x03400000` = data abort (XPU) | `old/displayTest.md:100`: nessun fault XPU | La protezione è attiva all'handover? |
| `old/displayTest.md:174-191`: `CTL_FLUSH` pending è normale anche col kernel che funziona (`0x40000018`, 27 frame, 0 underrun) | 7 test giudicati con la metrica "flush non consumato" | **La metrica usata non è valida** |
| `old/displayTest.md:128-129`: bug noto nel poll (`v != kick_value` sempre vero) | Test 6 usato come evidenza | Va riscritto come `(v & mask) == 0` |
| `old/displayTest.md:193-195`: `BIT(29)` per INTF2 forse errato, il kernel ha bit 3/4/30 | `display.c:798` usa `BIT(29)` | Il mapping dei flush bit va letto nel sorgente msm-3.18, non indovinato |

### 2.6 Sicurezza e conservazione del lavoro

- `old/displayTest.md:347-353` presenta `fastboot flash boot` come alternativa
  accettabile. È in conflitto con la regola non negoziabile
  (`HANDOFF-PROMPT.md:409-410`, `README.md:93`): **solo `fastboot boot`**.
- `git log` si ferma al **2026-11-12 22:49** (`4418684`); `git status` mostra
  16 file modificati e 12 non tracciati, tra cui `shim/display.c`,
  `shim/clocks.c`, `old/displayTest.md`, `tools/auto-display-test.sh`.
  **Tutto il lavoro del 15/11 è solo nel working tree**: va messo in sicurezza
  prima di ogni altro esperimento.

---

## 3. Il piano

Principio: **un solo cambiamento per ciclo, un solo criterio di riuscita,
un solo canale di lettura.** Ogni fase ha un go/no-go; se il no-go, non si
procede a tentativi ma si isola.

### Fase 0 — Riproducibilità (0 cicli device)

1. Commit (o almeno una copia etichettata) dello stato attuale, così i 7 test
   restano associati al sorgente che li ha prodotti.
2. Correggere `build.sh`: `SHIM_DISPLAY_TEST` default `0`, e `SKIP_EMMC`
   derivato dallo **stesso** valore (non da un secondo default diverso).
3. Introdurre un **run manifest**: sha256 di `out/shim-boot.img`, data, flag di
   build e commit, scritto come prima riga del log pstore. Ogni `old/displayTest.md`
   futuro parte da quella riga.
4. Aggiornare i tre documenti che descrivono un programma che non c'è più
   (`DISPLAY-TEST.md`, `SHIM-ARCHITECTURE.md`, `auto-display-test.sh`),
   rimuovendo il suggerimento `fastboot flash boot`.

**Go:** `./build.sh` senza variabili produce uno shim che salta a XNU, e due build
con flag diversi producono manifest diversi.

### Fase 1 — Chiudere il blocker reale (1 run CI + 1 ciclo device)

È l'unica fase sulla critical path per "iOS parte".

1. `tools/check-xnu-patch-chain.sh` → deve dare **tutte applicate, 0 fallite**
   con 0085 e il fix del limite utente inclusi.
2. Build CI, poi **verifica sul binario**, non sul verde della CI
   (`tools/check-xnu-binary.sh`, vedi `cea05a2`): `TCR_EL1` atteso
   `0x002a51a161a` con **TG0 = `0b00` (4 KiB)**, entry VA
   `0xfffffff0074a8488` / PA `0x814a8488`, marker presenti.
3. `tools/mkpayload.py` → `shim/build.sh` con `SHIM_DISPLAY_TEST=0` →
   `fastboot boot` (mai flash) → dopo il reset del watchdog leggere
   **subito** `tools/read-pram.py --marks`, con la guardia sulle window RAM.

| Osservazione | Significato | Azione |
|---|---|---|
| marker 12/13 presenti | MMU accesa, `bsd_init` raggiunto | passare a Fase 2 (eMMC/root) |
| marker 11 presente, 12 assente | ancora la `MSR SCTLR_EL1` | isolare con i marker ai due lati della MSR (0084) |
| record ESR/FAR/ELR valorizzato | eccezione presa | decodificare ESR, non riprovare a caso |
| tutto vuoto, `CurrentEL`/`MIDR` assenti | morte prima del blocco marker 10 | controllare caricamento segmenti/entry |

**Go:** marker 12 presente. **No-go:** non si tocca il display finché non è chiaro.

### Fase 2 — Rendere "dove si è fermato" una lettura sola (0 cicli device)

Il bisogno espresso ("vedere log ed errori, capire se si è bloccato") si chiude
qui, senza pannello:

1. **Un solo indicatore numerico** a indirizzo RAM fisso: ultimo `PHASE`/`stage`
   dello shim + ultimo marker XNU, scritto da ogni checkpoint. Dopo il reset si
   legge una word e si sa il punto esatto.
2. **Heartbeat**: contatore incrementato nel loop principale. Congelato = hang;
   assente con record di eccezione = crash; sono due diagnosi diverse.
3. `shim_checkpoint()` (`shim.c:393-398`) già fa `con_str` + flush + pet:
   estendere il flush a **ogni** fase rischiosa, non solo a inizio/fine del test
   display. Il loop dei 20 s (`shim.c:1535-1540`) oggi chiama `disp_mdp_kick()`
   e accumula log che vengono scritti in pstore solo alla fine: un hang a metà
   loop perde le ultime righe.
4. Tenere i tre canali ridondanti: pstore/ramoops, RAM `0x50000000`, trail IMEM.
   Il canale USB live (`xnu/patches/0077`) resta il prossimo obiettivo per il
   pre-console di XNU, ma mai provato sul device: va dopo il marker 12.

### Fase 3 — Display, come traccia separata (solo dopo un gate esplicito)

**Gate:** si entra qui solo se (a) XNU raggiunge `bsd_init`, oppure (b) l'obiettivo
dichiarato diventa "console visibile" invece di "iOS parte".

Bisecare per **prove di vita**, non per registri, nell'ordine:

| # | Esperimento (una sola cosa alla volta) | Criterio | Se fallisce |
|---|---|---|---|
| D0 | shim che **non tocca nulla**, solo watchdog 20 s | il logo LG resta identico | la baseline non è stabile: fermarsi |
| D1 | **minimal touch vero**: fill dei soli 12 MiB `0x03400000..0x04000000` in **RGB888 / 720×2 metà**, `dc cvac` + `dsb`, **zero scritture MDP/DSI** | cambia qualcosa a schermo | la pipeline LK è chiusa: **non** iterare sui registri |
| D2 | dalla recovery con `/dev/mem` + debugfs MDP, leggere lo stato **del kernel che funziona**: `intf2 play count`, `underrun`, TE, `TRIG_CTRL`, `CTL_TOP`, SMMU context bank 0 | si ha un riferimento "vivente" | nessuna misura = nessuna ipotesi |
| D3 | verificare se l'**SMMU MDP è agganciato** (`DISPLAY-FINDINGS.md:22-40`): se sì, `0x20000000`/`0x03400000` sono indirizzi fisici che il MDP non può vedere | spiega "scritture OK, pixel no" in un colpo solo | se è bypassato, cercare altrove |
| D4 | correggere la metrica: `play count` / `underrun` / TE, **non** `CTL_FLUSH`; fix del poll `(v & mask)==0`; mapping bit INTF dal sorgente msm-3.18 | il contatore di frame cresce | — |
| D5 | solo ora, bring-up completo `stage 1→5` (richiamando davvero le funzioni) con clock prima di ogni MMIO | pattern stabile | tornare a D2, non a D4 |
| D6 | HELLO **dinamico**: stage number + contatore, non testo statico | si vede dove si ferma | — |

Regole per tutta la Fase 3: `SHIM_SKIP_EMMC=1`, watchdog armato prima del primo
MMIO, allowlist verde (`tools/check-mmio-writes.py`), niente scritture su
`cont_splash` oltre `0xC00000`, niente `fastboot flash`, niente `userdata`.
Dopo **10 fallimenti sullo stesso punto**: fermarsi (regola `HANDOFF §8.8`).

---

## 3-bis. Lavoro eseguito il 15/11 (Fase 0 + Fase 3, D0/D1/D1b/D3)

L'utente ha scelto di procedere sul display. Il gate (b) è quindi aperto e
queste modifiche sono state applicate e **compilate localmente** per tutte le
modalità, con `tools/check-mmio-writes.py` verde. **Nessun comando sul
telefono.**

| File | Modifica |
|---|---|
| `shim/build.sh` | `SHIM_DISPLAY_TEST` default `0` (era `1` contro il commento); `SKIP_EMMC` deriva dalla stessa variabile; nuovi `DISPLAY_MODE` / `DISP_FB_BPP`; `G4_BUILD_ID` da hash di sorgenti+flag; `out/run-manifest.txt` con data, commit, build_id, sha256 dell'immagine |
| `shim/display.h` | modalità 0/1/2/3, `DISP_OK_BASELINE`, `disp_screen_beat()`, documentazione corretta |
| `shim/display.c` | `dsi_soft_trigger()`, `disp_mdp_commit()` con poll corretto `(v & mask)==0`, `disp_fill_bounded()`, `disp_screen_paint()` con HELLO + numero di stage, `disp_screen_beat()` con heartbeat, `display_test()` con stage realmente collegati e **BOOT dump** a clock accesi prima di ogni scrittura MDP |
| `shim/console.c/.h` | `con_draw_text_bpp()`: il testo usa il formato pixel delle pipe reali, non più 32 bpp hardcoded |
| `shim/display.c` (bug) | `disp_dump()` aveva 24 slot e 34 `REG()` → overflow dello stack: `DISP_DUMP_MAX 40` + bounds check |
| `shim/shim.c` | loop di hold chiama `disp_screen_beat()`, `DISP result=` invece di `stage=`, messaggio 20 s corretto |
| `tools/auto-display-test.sh` | header allineato, stampa il manifest |
| `docs/` | nuovo `INDEX.md`, `DISPLAY-TEST.md` riscritto, banner su `old/displayTest.md`, `SHIM-ARCHITECTURE.md`, `STATUS.md`, `README.md`, `HANDOFF-PROMPT.md`, `DISPLAY-FINDINGS.md` |

Risultato della verifica locale: 4 modalità + build normale, **exit 0**, zero
warning nei file toccati, audit MMIO `ok: 9 device windows checked`.
Immagine lasciata pronta: **mode 1 (D1)**, `build_id=0x845d155c`,
`image_sha256=0b0ed54b…5d285` (valori esatti in `shim/out/run-manifest.txt`;
cambiano a ogni rebuild perché l'hash include sorgenti e flag).

---

## 3-ter. Esiti D0/D1 e lavoro del 16/11 (l'esperimento decisivo)

I due run sono stati eseguiti **dall'utente** sul device (non dall'assistente).
Entrambi tecnicamente riusciti, nessun cambiamento visibile.

| | D0 | D1 |
|---|---|---|
| `build_id` | `0xa3df8faa` | `0x845d155c` |
| `image_sha256` | `a76202f7…b2db` | `8e4c5be7…ac37` |
| `DISP result=` | `00000040` | `00000010` |
| Attività | zero letture/scritture display | `FB rows=00000A00 bytes=00A8C000`, `SCR stage=1`, **40 heartbeat** |
| Schermo | logo LG fermo | logo LG fermo |

**Ipotesi chiusa:** all'handover `0x03400000` non è la sorgente dello
scan-out. Non è un crash (40 `BEAT n=`), non è un formato pixel sbagliato (che
darebbe comunque spazzatura visibile), non è un overrun. Il rinforzo storico:
`console.c` dipinge in `con_fb = 0x03400000` dal 10/11 senza effetto.
L'interpretazione favorita è un pannello DSI **command-mode con GRAM interna**
che si auto-aggiorna dopo che LK ha spinto il logo, con i clock MDSS spenti.

**Conseguenza sul piano:** il gate D1b/D3 (ricostruire MDP → CTL → LM → INTF →
DSI) non è più il passo più corto. Si apre il **mode 4 / D4**: un comando DCS
`0x2C` diretto al pannello, che non scrive alcun registro MDP e aggira il nodo
SMMU/IOVA. È un esperimento binario: se HELLO appare, esiste un canale video
e la console si costruisce lì; se `DCS timeout`, il link DSI è spento
all'handover e D3 diventa inevitabile.

Lavoro di codice eseguito il 16/11 (sempre **solo build locali**, nessun
comando sul telefono):

| File | Modifica |
|---|---|
| `shim/display.c` | tre bug DCS: ultimo byte del payload long-write, dispatch dual-DSI, `CMD_DMA_DONE` write-one-to-clear + poll separato `done0`/`done1` + ritorno `-1` su timeout; buffer DMA 16 KiB con bound; `dsi_r()` DSI0-only rimosso |
| `shim/display.c` | **mode 4**: clock core → BOOT dump (anche DSI1) → PLL/pixel clock solo se i link sono spenti → probe `0x11`/`0x29` → finestra `0x2A`/`0x2B` → HELLO via `0x2C` nella GRAM; beat con quadrato rosso/verde |
| `shim/display.c` | mode 2 reso sicuro: `clocks_display_core()` prima del trigger (prima scriveva DSI a clock spenti) |
| `shim/display.h`, `shim/build.sh` | mode 4, `DISP_OK_GRAM`, stage 1/2/3 per D4 |
| `shim/shim.c`, `tools/auto-display-test.sh` | messaggi dipendenti dalla modalità: D0 non dice più "HELLO on screen" |
| `docs/` | `DISPLAY-TEST.md` e `INDEX.md` aggiornati con esiti, pacchetti DCS e tabella di interpretazione |

Verifica locale: **5 modalità + stage 1/2 di D4 + build normale**, tutte
exit 0, zero warning in `display.c`/`console.c`, audit MMIO verde
(`ok: 9 device windows checked`). Il formato dei pacchetti DCS è stato
ricontrollato contro `mdss_dsi_cmd.c:96-135` e `mdss_dsi_host.c:1734-1795`
e validato con un modello host prima di buildare.

---

## 3-quater. D4 (16/11) — il BOOT dump cambia la diagnosi

Eseguito dall'utente: `build_id=0x42d146b9`, `image_sha256=c1d73238…b22d`,
`DISP result=000000B3`, nessun timeout, 40 heartbeat. **Schermo: ancora solo
il logo LG.** Ma questo run ha finalmente prodotto la misura che mancava in
tutti i sette test precedenti.

| Misura | Valore | Conseguenza |
|---|---|---|
| `DSI.ctrl` / `DSI1.ctrl` | `000001F5` | **entrambi i link vivi**, command mode, 4 lane |
| `DSI.clkctl` / `DSI1.clkctl` | `0000023F` | clock controller accesi |
| `DSI.mdpctl` / `DSI.dcsctl` | `00000008` / `00013C2C` | RGB888; `0x2C` start / `0x3C` continue |
| `DSI.trig` / `DSI1.trig` | **`00000004`** | kernel funzionante: **`0x80000004`** → manca `te_sel` (BIT(31)) |
| `RGB0.addr` / `RGB1.addr` | `03400000` | **la pipeline MDP di LK punta ancora a `cont_splash`** |
| `RGB0.strd` / `size` | `000010E0` / `0A0002D0` | 4320 = 1440×3, 720×2560: **identico a D1** |
| `RGB2.*` | tutti 0 | pipe del kernel spenta |
| `DCS done0/done1` | `1/1` due volte | motore DMA DCS risponde su entrambi i link |
| `GRAM packets` | `00000019` (25) | 384.000 byte inviati, nessun errore |

**Correzione alla diagnosi precedente:** la pipeline MDP non è rotta, è
**congelata**. LK ha spinto un frame nella GRAM del pannello e ha lasciato il
percorso frame senza sorgente di trigger. I test 1–7 avevano *ricostruito*
MDP (RGB2, `CTL_TOP`, `CTL_LAYER`), cioè avevano smontato l'unica
configurazione che funzionava. D1 aveva ragione a non vedere nulla, ma la
causa non è "il pannello non legge la DDR": è "nessuno sta più facendo
partire un frame".

**Due difetti concreti trovati leggendo i sorgenti del telefono:**
`DSI_TRIG_CTRL` senza `BIT(31)` (`te_sel`, `mdss_dsi_host.c:369-370`, DTS
`te-pin-select=<1>`), e `DSI_COMMAND_MODE_DMA_CTRL` in **LP mode**
(`BIT(26)`, `dsi_host_v2.c:470-472/513-525`) mentre i pacchetti pixel da
15 KiB richiedono HS. Inoltre `CMD_DMA_DONE` è un completamento **lato host**:
i 25 pacchetti e i 40 beat di D4 non provano che il pannello abbia ricevuto
qualcosa.

**Lavoro del 16/11 (solo build locali):** dump esteso a 53 registri
(`dmactl/status/fifo/ackerr/intctl/rdbk0` su entrambi i link, contatori
`INTF*.frmc`, `PP0.tear/autorf`); log DCS con `wait_ms=` e bitmask dei link;
`gram_hello()` con una sola finestra + `0x3C` continue; **mode 5** (HS +
`te_sel` + pittura DDR e GRAM + kick MDP ogni 500 ms, zero scritture MDP) e
**mode 6** (probe `0x28` display off). Verifica: 7 modalità + 3 stage di D5 +
build normale, tutte exit 0, zero warning, audit MMIO verde.

---

## 4. Cosa NON ritentare

- Giudicare un test dal valore di `CTL_FLUSH` (`old/displayTest.md:174-191`).
- Cambiare `CTL_LAYER` / `CTL_TOP` / bit di flush senza un indicatore di frame
  consegnato: è la strada già percorsa 7 volte.
- Scrivere un frame RGBA8888 da 14 MiB dentro i 12 MiB di `cont_splash`.
- **Riscrivere ancora `cont_splash` sperando che appaia**: D1 lo ha escluso.
- Citare `fastboot flash boot` come alternativa di test.
- Trattare "niente console a schermo" come causa del mancato avvio di XNU.

---

## 5. Prossima azione singola

**Traccia display (scelta dell'utente):** un solo run di **D5**,
`SHIM_DISPLAY_TEST=1 DISPLAY_MODE=5 DISPLAY_STAGE=3 ./build.sh` seguito da
`tools/auto-display-test.sh`. Riattiva la pipeline viva di LK (HS + `te_sel`),
dipinge entrambe le sorgenti e kikka frame: i contatori `FR*` dicono
numericamente se i frame partono. Se resta buio, **D6** (`0x28`) risponde
alla domanda binaria "il pannello ubbidisce ai DCS?". D3 resta l'ultimo
fallback.

**Critical path da non perdere — traccia XNU:** Fase 1 (verifica binario 4 KiB
→ `fastboot boot` → `--marks`). Resta un run CI e un ciclo device, e dà un
sì/no sul blocker misurato.

Le due tracce non si escludono: il display dà la console visibile allo shim,
il marker ring dà il progresso di XNU.

---

*Prima stesura: documento di sola analisi. Aggiornato la sera del 15/11 con il
lavoro di codice del §3-bis, il 16/11 con gli esiti D0/D1 (§3-ter) e poi con
il BOOT dump di D4 e i mode 5/6 (§3-quater).
Tutte le build e le verifiche sono **locali**: **nessun comando sul telefono**
(`fastboot`/`adb`) eseguito dall'assistente e nessun flash. I run device
D0/D1/D4 sono stati eseguiti dall'utente.*
