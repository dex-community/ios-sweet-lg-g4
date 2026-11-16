# Documento di Analisi — LG G4 (H815) Display Bring-up Shim

> **ARCHIVIO — non è la guida corrente.** Test 1-7 del 15/11 eseguiti con un
> binario **non identificato** (nessun hash, nessun build-id) e con la
> "VARIANTE B" di `display.c`, che non disegnava HELLO. Le misure grezze e i
> valori dei registri restano validi; la metrica `CTL_FLUSH` usata per
> giudicarli **non lo è** (lo dice il §3.2 di questo stesso documento). Anche
> il `fastboot flash boot` del §7 è superato: solo `fastboot boot`.
> Guida corrente: `docs/DISPLAY-TEST.md` · mappa: `docs/INDEX.md`.

**Data:** 2026-11-15
**Oggetto:** Diagnosi del mancato funzionamento dello scanout MDP nel boot shim
**Stato attuale:** Display bloccato sul logo LG del bootloader; MDP non completa la consegna del frame al pannello

---

## 1. Contesto e obiettivo

Lo shim (boot image custom caricata via `fastboot boot`) deve:
- Inizializzare i clock MMSS/DSI
- Configurare i controller Dual-DSI e il pannello JDI NT36850
- Configurare MDP5 (layer mixer, pipe, interfacce)
- Disegnare un pattern visibile a schermo (cyan/HELLO)
- Eseguire il kickoff di scansione
- Mantenere il pattern visibile per 20 s, poi riavviare in TWRP
- Riportare log diagnostici in `pstore/pmsg-ramoops-0`

**Sintomo riportato:** durante i 20 s il pannello resta fermo sul logo LG del bootloader. Poi riavvio automatico in recovery. Mai visto alcun cambiamento visibile.

---

## 2. Cronologia dei test e degli esiti

### Test 1 — Configurazione iniziale "dual split" (RGB0+RGB1 su INTF1+INTF2)

**Configurazione:** ripresa dal bootloader. `CTL_LAYER = 0x01001200`, `CTL_TOP = 0x00021F20`, split display abilitato, autoresfresh su PINGPONG0.

**Output pstore:**
```
MDP CTL.top00021F20
MDP CTL.flush40000000
MDP CTL.layer001001200
MDP RGB2.* tutti 0x00000000
MDP INTF2.tgen00000000
MDP INTF2.pfmt0000213F
...
MDP_RUN CTL.flush60000000
```

**Esito:** schermo invariato (logo LG). `CTL.flush` residuo `0x60000000` (INTF1+INTF2 pending).

---

### Test 2 — Match kernel: solo RGB2, `CTL_LAYER = 0x01008000`, `CTL_TOP = 0x00020020`

**Modifiche:**
- `CTL_LAYER(0)` da `0x01001200` a `0x01008000` (RGB2 stage 1 + border)
- `CTL_TOP` a `0x00020020` (INTF2 in command mode)
- Flush con `BIT(30)` (INTF1) — *poi rivelatosi bit errato*

**Output pstore:**
```
MDP CTL.layer001008000
MDP RGB2.size0A0005A0
MDP RGB2.fmt000236FF
MDP RGB2.unpk03010002
MDP RGB2.addr20000000
MDP RGB2.strd00001680
MDP_RUN CTL.flush40000000    ← ancora pending
```

**Esito:** schermo invariato. `CTL.flush = 0x40000000` (INTF1 pending).

---

### Test 3 — Ripristino `CTL_TOP = 0x00020020` con flush su INTF2 (`BIT(29)`)

**Output:**
```
MDP_RUN CTL.flush60000000
```

**Esito:** schermo invariato. Ora flush `0x60000000` = INTF1+INTF2 pending.

---

### Test 4 — Azzeramento `CTL_FLUSH` prima della scrittura

**Modifica:** `wr(CTL_FLUSH, 0)` seguito da `wr(CTL_FLUSH, valore)`.

**Output:** identico. `CTL.flush = 0x60000000` dopo il kick.

---

### Test 5 — Verifica scrivibilità del cont_splash: fill CYAN

**Configurazione:** fill di `0x03400000` con cyan, `0x20000000` con rosso. RGB2 puntato a `0x03400000`.

**Output:**
```
DISP FILLED SPLASH WITH CYAN
DISP FILLED 0x20000000 WITH RED
MDP RGB2.addr03400000
MDP_RUN CTL.flush60000000
MDP_RUN RGB2.addr03400000
```

**Esito:** schermo ancora logo LG. Conferma che le scritture a `0x03400000` **non producono fault XPU**, ma MDP non le sta scansionando.

---

### Test 6 — Poll del flush (misurazione del consumo dei bit)

**Modifica:** loop che rilegge `CTL_FLUSH` ogni 1 ms per 200 ms confrontando con `kick_value`.

**Codice:**
```c
uint32_t flushed = 0;
for (int i = 0; i < 200; i++) {
    uint32_t v = rd(CTL_BASE + CTL_FLUSH);
    if (v != kick_value) { flushed = v; break; }
    mdelay(1);
}
```

**Output:**
```
KICK wrote=20020060 after=60000000 poll_ms=00000000
KICK flushed=60000000
KICK wrote=20020060 after=60000000 poll_ms=00000000
KICK flushed=60000000
(ripetuto 10 volte identico)
```

**Interpretazione:**
- `poll_ms=0` = uscita alla prima iterazione perché `0x60000000 != 0x20020060`
- **BUG nel test:** la condizione `v != kick_value` è sempre vera (write-1-to-clear), quindi il loop non misura davvero il consumo. Serve una condizione `(v & intf_mask) == 0`.

---

### Test 7 — Configurazione finale "match kernel completo" (RGB2, cache clean, dual-DSI trigger, no autorefresh)

**Modifiche:**
- RGB2 configurato esattamente come il kernel (`0x0A0005A0`, `0x000236FF`, `0x03010002`, `0x00001680`)
- `CTL_LAYER = 0x01008000`
- Buffer `FB_ADDR = 0x20000000` (poi `0x03400000`)
- **Cache clean** eseguita (`dc cvac` + `dsb sy`)
- Trigger su DSI0 e DSI1
- Rimozione autorefresh (`0x71030 = 0`)

**Output:**
```
DSI CTL.layer000001200      ← stato bootloader (prima dell'init)
MDP CTL.layer001008000      ← stato dopo init (RGB2, corretto)
DISP FILLED 0x20000000 WITH CYAN
DISP CACHE CLEANED to DRAM
KICK wrote=20020060 after=60000000
KICK wrote=20020060 after=60000000
(ripetuto 10 volte)
```

**Esito:** schermo ancora logo LG. `CTL.flush` sempre `0x60000000`.

---

## 3. Scoperte analitiche chiave

### 3.1 Decodifica corretta del flush register

Analisi del valore `0x60000000` restituito dopo la scrittura di `0x20020060`:

| Bit | Significato | Scritto in `0x20020060` | Letto in `0x60000000` | Interpretazione |
|---|---|---|---|---|
| 5 | RGB2 | ✅ | ❌ assente | **consumato** |
| 6 | LM0 | ✅ | ❌ assente | **consumato** |
| 17 | CTL | ✅ | ❌ assente | **consumato** |
| 29 | INTF2 | ✅ | ✅ presente | **non consumato** |
| 30 | (ignoto) | ❌ (era già lì) | ✅ presente | pre-esistente dal bootloader |

**Conclusione:** la pipeline RGB2 → LM0 → CTL **si flushatta correttamente**. Solo il bit di INTF2 non risponde. Il flusso si ferma all'ultimo stadio, dove MDP deve consegnare alle DSI.

### 3.2 Il flush pending NON è di per sé un errore

Analizzando il dump del kernel funzionante (`display-2026-11-10.txt`):

```
Play Count=27  Underrun Count=0
intf2: play: 00000027  vsync: 00000156  underrun: 00000000
(misurato due volte a 2 s di distanza, identico)

--- CTL0
0x00002010: 00000000 00020020 40000018 00000000
                              ^^^^^^^^
                              CTL_FLUSH = 0x40000018 in idle stabile
```

**Il kernel funzionante ha `CTL_FLUSH = 0x40000018` (bits 3, 4, 30 pending) mentre il display funziona perfettamente, 0 underrun, 27 frame giocati.**

Questo significa che **"flush pending = display rotto" è FALSO** su questa piattaforma. Alcuni bit restano legittimamente pending in idle.

### 3.3 Il bit `FLUSH_INTF2 = BIT(29)` potrebbe essere errato

Nel dump kernel, `CTL_FLUSH = 0x40000018` non contiene `BIT(29)`. Contiene invece `BIT(3)`, `BIT(4)`, `BIT(30)`. Il mapping dei flush bit per INTF2 potrebbe essere uno tra `BIT(3)` e `BIT(4)`, non `BIT(29)` come definito nel codice shim.

### 3.4 Il kernel usa RGB2 → INTF2 → DSI1 (dual-DSI split)

Dal dump kernel:
```
Panel #0 720x2560p60
Intf Mixer #0 res=1440x2560
SSPP #5 type=RGB ndx=20 play_cnt=27 stage=1
```

- Layer mixer: 1440×2560 full-width
- Un solo pipe attivo: **RGB2 (pipe 5)**
- `CTL_LAYER(0) = 0x01008000` → solo RGB2
- `CTL_TOP = 0x00020020` → INTF2 (bits[7:4] = 0x2)
- `intf2: play: 00000027` → INTF2 ha consegnato 27 frame

### 3.5 Framebuffer kernel: `0x00e30000`, dimensione insufficiente del cont_splash

```
--- RGB2_SSPP
0x00019010: 00e30000  ← SRC0_ADDR
0x00019020: 00001680  ← STRIDE = 5760 = 1440×4
0x00019030: 000236ff  ← FORMAT = RGBA8888
```

Framebuffer RGB2 = 1440×2560 RGBA8888 = **14.745.600 byte ≈ 14 MB**.

Il cont_splash buffer è **12 MB** (`size=00C00000`), quindi **non abbastanza** per contenere un frame full 1440×2560 RGBA8888.

### 3.6 RGB0/RGB1 del bootloader

Dal dump kernel (stato a valle dell'avvio, ma presumibilmente lasciato dal bootloader):

```
--- RGB0_SSPP
0x00015010: 03400000  ← SRC0_ADDR
0x00015000: 0a0002d0  ← size = 720×2560 (metà)
0x00015030: 004037ff  ← format RGB888
0x00015050: 00000040 00000080 000000c0  ← costanti colore

--- RGB1_SSPP
0x00017010: 03400000  ← SRC0_ADDR (stesso buffer!)
0x00017000: 0a0002d0  ← size = 720×2560 (metà)
```

**RGB0 e RGB1 puntano entrambi a `0x03400000`** con dimensione 720×2560. Il bootloader configura due pipe che leggono due metà dallo stesso buffer cont_splash (offset calcolati dal mixer, non dall'address).

### 3.7 INTF1 vs INTF2 nel kernel

- `INTF1` (`0x6b000`): `PANEL_FORMAT = 0x00002100` (solo parte alta)
- `INTF2` (`0x6c000`): `PANEL_FORMAT = 0x0000213F` (RGB888 completo)
- `intf2: play: 27` → **INTF2 è quella che trasmette**
- `CTL_TOP` bits[7:4] = 0x2 → **INTF2 selezionata**

### 3.8 DSI0 e DSI1 nel kernel

Entrambi i controller DSI hanno configurazione identica:
```
dsi0_ctrl[0x84] = 0x80000004  ← TRIG_CTRL (trigger MDP)
dsi1_ctrl[0x84] = 0x80000004  ← TRIG_CTRL (trigger MDP)
```

**Il kernel trigga DSI0 E DSI1 contemporaneamente** per il dual-DSI split.

### 3.9 PINGPONG0 e tear check

Kernel:
```
0x00071000: 00000001  ← PINGPONG0 abilitato con tear check
0x00071030: 00000000  ← NESSUN autorefresh
```

Lo shim scriveva `0x71030 = BIT(31) | 1` (autorefresh). Il kernel **non usa autorefresh**, usa tear check via DSI TE.

---

## 4. Ipotesi attuali e stato della diagnosi

### 4.1 Ipotesi principale: SMMU / IOMMU

Su msm8992 (Snapdragon 808), la pipeline MDP passa attraverso **SMMU (IOMMU)**. Il kernel configura SMMU in modo che MDP veda il framebuffer a un certo indirizzo virtuale (`0x00e30000`). Il bootloader fa lo stesso per il cont_splash (`0x03400000`).

**Lo shim scrive cyan a `0x20000000` e `0x03400000`. La CPU lo vede, ma MDP potrebbe non vederlo se `0x20000000` non è mappato nel contesto SMMU di MDP.** Questo spiegherebbe:
- Logo LG visibile (dato in `0x03400000`, mappato dal bootloader)
- Cyan non visibile (scritto in buffer non mappati nel contesto MDP)
- Fallimento di ogni combinazione di registri

### 4.2 Ipotesi alternativa: doppio trigger DSI

Il pannello G4 è dual-DSI split. Se il trigger DCS va solo su DSI0 e non su DSI1, la metà destra del pannello non riceve il frame → INTF2 non completa → flush non consumato.

### 4.3 Ipotesi terza: il bootloader ha "lockato" MDP

Il bootloader potrebbe aver lasciato MDP in uno stato da cui non è possibile riconfigurarlo senza un reset completo dei power domain / GDSC. Lo shim eredita uno stato funzionante ma "congelato", che non accetta nuove configurazioni.

---

## 5. Cosa NON ha funzionato (elenco sintetico)

| Tentativo | Esito |
|---|---|
| Match `CTL_LAYER` kernel (`0x01008000`) | ❌ flush ancora pending |
| Match `CTL_TOP` kernel (`0x00020020`) | ❌ flush ancora pending |
| Uso di `BIT(29)` o `BIT(30)` per flush INTF | ❌ nessun bit consumato |
| Azzeramento `CTL_FLUSH` prima del kick | ❌ invariato |
| RGB2 configurato esattamente come kernel | ❌ invariato |
| Buffer cyan a `0x03400000` | ❌ nessun cambio visivo |
| Buffer cyan a `0x20000000` | ❌ nessun cambio visivo |
| Cache clean eseguita | ❌ invariato |
| Trigger DSI dual | ❌ invariato |
| Rimozione autorefresh | ❌ invariato |
| Config split (RGB0+RGB1) | ❌ invariato |
| Polling del flush | ⚠️ bug nel test, rileva solo "pending" |

---

## 6. Cosa NON è ancora stato provato

1. **Dump "BOOT" completo** — stato dei registri MDP/DSI/SSPP **prima** di qualsiasi `wr()`. Mai catturato. Senza questo, ogni test è "a ciegas" rispetto a cosa lo shim eredita dal bootloader.

2. **Dump di RGB0 e RGB1** post-init — per verificare se puntano effettivamente a `0x03400000` o altrove.

3. **Dump di INTF1** — il log attuale dumpa solo INTF2, ma il kernel ha config anche su INTF1.

4. **Test "minimal touch"** — non toccare MDP affatto, solo fill del cont_splash a `0x03400000` con cyan, cache clean, e riscrivere il valore di `CTL_FLUSH` **letto al boot** senza modificarlo.

5. **Verifica SMMU/IOMMU** — non è stato verificato se MDP ha accesso a `0x20000000` / `0x00e30000` / `0x03400000` nel suo contesto SMMU.

6. **Test con framebuffer a `0x00e30000`** (indirizzo kernel) — mai provato.

7. **Logica "no-op"** — boot dello shim che non tocca nulla e verifica che la schermata logo LG rimanga stabile (test di regressione).

---

## 7. Comandi e script utilizzati

### Build
```bash
cd /Users/io/Dev/ios/ios-sweet-lg-g4/shim && ./build.sh
```

### Test automatico
```bash
cd /Users/io/Dev/ios/ios-sweet-lg-g4 && ./tools/auto-display-test.sh
```

### Flash (metodo sicuro, non distruttivo)
```bash
fastboot boot out/shim-boot.img
```

### Flash (alternativo, permanente)
```bash
fastboot flash boot out/shim-boot.img
fastboot reboot
```

**Nota:** `fastboot boot` è preferito per i test (non distruttivo, riavvio automatico, stato hardware identico al post-bootloader). `fastboot flash boot` è più rischioso ma non cambia il risultato finale del test.

---

## 8. Output chiave dell'ultimo test (riferimento)

```
DISP step 1: core clocks start
DISP core clocks: on
DISP step 2: dsi pll start
PLL init rsm=000000F5
PLL already locked from bootloader
DISP pll: locked
DISP step 2b: pixel/byte clocks start
DISP pixel/byte clocks: on
DISP step 3: dsi controller init
DISP dsi: command mode
DSI DSI.ctrl000001F5
DSI DSI.mdpctl00000008
DSI DSI.dcsctl00013C2C
DSI DSI.strm0c08710039
DSI DSI.strm0t0A0002D0
DSI DSI.trig80000004
DSI DSI.clkctl0000023F
DSI DSI.timing00002E2C
DSI PHY.str000000077
DSI PHY.reg0100A0000
DSI PHY.lane000000002
DSI PLL.rsm00000000
DSI CTL.top00021F20
DSI CTL.flush40000000
DSI CTL.layer000001200
DSI RGB2.size00000000
DSI RGB2.fmt00000000
DSI RGB2.unpk00000000
DSI RGB2.addr00000000
DSI RGB2.strd00000000
DSI INTF2.tgen00000000
DSI INTF2.pfmt0000213F
DSI LM0.outsz0A0005A0
DISP step 4: mdp init
DISP mdp: up
MDP DSI.ctrl000001F5
MDP DSI.mdpctl00000008
MDP DSI.dcsctl00013C2C
MDP DSI.strm0c08710039
MDP DSI.strm0t0A0002D0
MDP DSI.trig80000004
MDP DSI.clkctl0000023F
MDP DSI.timing00002E2C
MDP PHY.str000000077
MDP PHY.reg000000000
MDP PHY.lane000000002
MDP PLL.rsm00000000
MDP CTL.top00020020
MDP CTL.flush40000000
MDP CTL.layer001008000
MDP RGB2.size0A0005A0
MDP RGB2.fmt000236FF
MDP RGB2.unpk03010002
MDP RGB2.addr20000000
MDP RGB2.strd00001680
MDP INTF2.tgen00000000
MDP INTF2.pfmt0000213F
MDP LM0.outsz0A0005A0
DISP step 5: framebuffer pattern draw
DISP CACHE CLEANED to DRAM
DISP drawn
DISP step 6: panel sleep-out & display-on
DISP panel: on
DISP step 7: mdp start scanout
KICK wrote=20020060 after=60000000 poll_ms=00000000
KICK flushed=60000000
MDP_RUN DSI.ctrl000001F5
...
MDP_RUN CTL.top00020020
MDP_RUN CTL.flush60000000
MDP_RUN CTL.layer001008000
MDP_RUN RGB2.size0A0005A0
MDP_RUN RGB2.fmt000236FF
MDP_RUN RGB2.unpk03010002
MDP_RUN RGB2.addr20000000
MDP_RUN RGB2.strd00001680
MDP_RUN INTF2.tgen00000000
MDP_RUN INTF2.pfmt0000213F
MDP_RUN LM0.outsz0A0005A0
DISP display test: COMPLETE
DISP stage=0000003F
```

---

## 9. Dati di riferimento kernel (`display-2026-11-10.txt`)

### High level state
```
--[ Control path #0 - MIPI_CMD]--
Panel #0 720x2560p60
	vbp=10 vfp=9 vpw=1 hbp=88 hfp=168 hpw=4
MDP Clk=265064645  Final BW=1213632000
Play Count=27  Underrun Count=0

Intf Mixer #0  res=1440x2560
SSPP #5 type=RGB ndx=20 flags=0x00000000 play_cnt=27
	stage=1 ... src w=1440 h=2560 format=13 (RGBA_8888)
Total pipes=1
```

### CTL0
```
0x00002000: 01008000 00000000 00000000 00000000
0x00002010: 00000000 00020020 40000018 00000000
```

### RGB2_SSPP
```
0x00019000: 0a0005a0 0a0005a0 00000000 0a0005a0
0x00019010: 00000000 00e30000 00000000 00000000
0x00019020: 00000000 00001680 00000000 00000000
0x00019030: 000236ff 03010002 00000000 00000000
```

### RGB0 / RGB1 (entrambi a 0x03400000, 720×2560)
```
--- RGB0_SSPP
0x00015000: 0a0002d0 0a0002d0 00000000 0a0002d0
0x00015010: 00000000 03400000 ...
0x00015030: 004037ff 03020100 ...

--- RGB1_SSPP
0x00017000: 0a0002d0 0a0002d0 00000000 0a0002d0
0x00017010: 00000000 03400000 ...
0x00017030: 004037ff 03020100 ...
```

### INTF1 / INTF2
```
--- INTF2  off=0x6c000
0x0006c090: 0000213f ...

--- INTF1  off=0x6b000
0x0006b090: 00002100 ...
```

### Split display
```
0x000012f0: 00000000 00000001 00000106 00000000
0x00001330: 00000000 00000020 00310000 00000000
0x000013f0: 00000106 00000000 00000001 00000000
```

### PINGPONG0
```
0x00071000: 00000001 0018007c 0000fff0 00000a05
0x00071030: 00000000 00000000 00000000 00000000
```

### DSI0 / DSI1
```
--- dsi0_ctrl
0x00000000: 10030000 000001f5 00000000 11111000
0x00000040: 00000008 00013c2c 01c40000 00000004
0x00000080: 001c1a02 80000004 ...

--- dsi1_ctrl
0x00000000: 10030000 000001f5 00000000 11111000
0x00000040: 00000008 00013c2c 01c40000 00000004
0x00000080: 001c1a02 80000004 ...
```

### Contatori frame (due misure a 2 s)
```
intf2: play: 00000027  vsync: 00000156  underrun: 00000000
RGB0 :   00000002	RGB1 :   00000002	RGB2 :   00000027
```

---

## 10. Conclusioni

### Fatto stabilito
1. Il path MDP RGB2 → LM0 → CTL si flushatta correttamente (bit 5, 6, 17 consumati).
2. Il bit di flush di INTF2 **non si consuma**, indicando che la consegna DSI non completa.
3. **Il flush pending non è di per sé prova di malfunzionamento** — anche il kernel funzionante ha `CTL_FLUSH = 0x40000018` in idle.
4. Il kernel usa: RGB2 → INTF2 → DSI0+DSI1 (dual split), framebuffer `0x00e30000`.
5. Le scritture di shim a `0x03400000` e `0x20000000` **non causano fault**, ma MDP non le scansiona.
6. Il cont_splash (12 MB) è **troppo piccolo** per un frame 1440×2560 RGBA8888 (14 MB).

### Problema ancora aperto
**Non è chiaro perché MDP non stia consegnando il frame configurato alle DSI.** Le ipotesi principali sono:
- **(A)** Mancato mapping SMMU del buffer framebuffer nel contesto MDP
- **(B)** Trigger DCS non inviato su entrambe le DSI
- **(C)** Stato MDP del bootloader "locked", non riconfigurabile

### Prossimo passo diagnostico raccomandato
**Singolo test "minimal touch":**
```c
void disp_test_minimal(void) {
    volatile uint32_t *fb = (uint32_t *)0x03400000;
    for (uint32_t i = 0; i < 0x300000; i++) fb[i] = 0xFF00FFFF;
    for (uintptr_t a = 0x03400000; a < 0x03400000 + 0xC00000; a += 64)
        asm volatile("dc cvac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy" ::: "memory");
    uint32_t flush = rd(CTL_BASE + CTL_FLUSH);
    trace("FLUSH_boot", flush);
    wr(CTL_BASE + CTL_FLUSH, flush);  /* riscrivi identico */
    for (volatile int i = 0; i < 100000000; i++);
}
```

**Senza toccare nient'altro.** Se il pannello diventa cyan → il bootloader scansiona da `0x03400000` e basta scrivere lì. Se resta logo LG → la pipeline del bootloader è già "chiusa" al passaggio di consegne e serve diagnostica SMMU/XPU approfondita.

---

*Fine documento.*