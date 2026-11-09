# T1 — Verifica statica della sequenza di bring-up (2026-11-08)

Nessun run sul telefono. Tutto qui è stato letto nel binario LK
(`aboot-lk-g4-h815.elf`), nelle sorgenti CAF recuperate e nel driver Linux
funzionante.

Nuovo tooling: `research/lk-g4/an4.py` (legge l'ELF direttamente, non più
da `/tmp/lkg/an.py` che non esiste più; mappa delle struct
`dev->HC` / `dev->plat->CORE`).

Listing nuovo e completo: `research/lk-g4/dis-4.9-p-sdhci_setup_host-full.txt`
(la versione `dis-4.9-m-0f913954.txt` era troncata a 0x0f913a90).

---

## D1 — L'LK scrive 0x0a/0x0b (1,8 V), NON 0x0e/0x0f (3,3 V)

**FATTO.** La deduzione è **confermata**. Il ramo 1,8 V esisteva ed era
proprio quello preso su questo hardware.

Il codice letto in `sdhci_setup_host` (0x0f913954):

```
0x0f9139b0  tst  r3, #0x1000000     ; CAPS1 bit24 = 3.3 V supportata
0x0f9139b4  beq  #0xf913a6c
0x0f9139b8  mov  ip, #0xf            ; -> HC+0x29 = 0x0f (power ON, 3.3 V)
0x0f9139bc  mov  r0, #0xe            ; -> HC+0x29 = 0x0e (power OFF, 3.3 V)
0x0f9139c0  mov  r3, #7              ; dev->voltage = 7
0x0f913a6c  tst  r3, #0x2000000     ; CAPS1 bit25 = 3.0 V supportata
0x0f913a70  beq  #0xf913a88
0x0f913a74  mov  r3, #6
0x0f913a78  mov  ip, #0xd            ; -> HC+0x29 = 0x0d (power ON, 3.0 V)
0x0f913a80  mov  r0, #0xc            ; -> HC+0x29 = 0x0c (power OFF, 3.0 V)
0x0f913a88  tst  r3, #0x4000000     ; CAPS1 bit26 = 1.8 V supportata
0x0f913a8c  ldrbeq r0, [r4,#0x26]    ; se non supportata: tensione predefinita
0x0f913a90  movne r3, #5             ; dev->voltage = 5
0x0f913a94  movne ip, #0xb          ; -> HC+0x29 = 0x0b (power ON, 1.8 V)
0x0f913a98  movne r0, #0xa          ; -> HC+0x29 = 0x0a (power OFF, 1.8 V)
0x0f913a9c  strbne r3, [r4,#0x26]
0x0f913aa0  lsleq r0, r0, #1        ; ramo "non supportata": vol<<1
0x0f913aa4  uxtbeq r0, r0
0x0f913aa8  orreq ip, r0, #1
```

Con `CAPS1 = 0x342dc8b2` (misurato sul telefono, run 5):

| bit | maschera | valore | significato |
|---|---|---|---|
| 24 | 0x01000000 | **0** | 3,3 V non supportata |
| 25 | 0x02000000 | **0** | 3,0 V non supportata |
| 26 | 0x04000000 | **1** | 1,8 V supportata |

=> ramo 1,8 V => `HC+0x29 = 0x0a` (power OFF) poi `HC+0x29 = 0x0b` (power ON).

**Corroborazione indipendente** dalla sorgente CAF (`sdhci.c`):

```c
if (caps[0] & SDHCI_3_3_VOL_MASK)        /* 0x01000000 */
        host->caps.voltage = SDHCI_VOL_3_3;
else if (caps[0] & SDHCI_3_0_VOL_MASK)   /* 0x02000000 */
        host->caps.voltage = SDHCI_VOL_3_0;
else if (caps[0] & SDHCI_1_8_VOL_MASK)   /* 0x04000000 */
        host->caps.voltage = SDHCI_VOL_1_8;
...
sdhci_set_bus_power_on(host);   /* voltage<<1, poi |BIT(0) */
```

`SDHCI_VOL_1_8 = 5` -> `5<<1 = 0x0a`, `|BIT(0) = 0x0b`. Identico.

**Conseguenza**: il "fix 3,3 V" (0x0f) con cui e' stato costruito il run 6
**non e' quello che fa l'LK su questa scheda**. Il run 5 ha risposto con
0x0f per combinazione/circostanza, non perche' 0x0f sia il valore giusto.
La variante 3 del run 5 (1,8 V forzato, 0x0b) **non e' mai stata provata**,
perche' la variante 2 aveva gia' vinto.

---

## D2 — 0x0f912ff4 scrive HC+0x3e, non "SDMA_ADDR_LOW"

**FATTO, deduzione confermata.** Nel listing:

```
0x0f912fb4  add  ip, r3, #0x3e        ; ip = &HC + 0x3e = 0xf982493e
0x0f912fc4  ldrh r3, [r3, #0x3e]      ; legge lo stesso indirizzo
0x0f912fc8  and  r4, r3, #0xfff8
0x0f912ff4  strh r4, [ip]             ; scrive 0xf982493e
```

`0x3e` e' `SDHCI_HOST_CONTROL2` (0x3c = HOST_CONTROL, 0x3e = HOST_CONTROL2).
L'etichetta "SDHCI_SDMA_ADDR_LOW" era sbagliata: SDMA_ADDR_LOW e' a
`HC+0x00`.

**Correzione applicata** a `research/lk-g4/dis-4.9-m-0f912f88.txt` (inline
+ MMIO SUMMARY + titolo della funzione). Il tool che generava le etichette
era in `/tmp/lkg/an.py` e non esiste piu'; il tool nuovo `an4.py` non ha
etichette hard-coded, quindi non riproduce l'errore.

---

## D3 — Handshake PWRCTL: confermato, ma con una correzione importante

**FATTO 1 — l'handler PWR esiste e cosa scrive.** Handler a 0x0f913ac0,
registrato in `sdhci_msm_init` (0x0f913fd4 `bl 0xf917f4c`) e abilitato
(0x0f913fdc `bl 0xf917f14`):

```
0x0f913ae4  ldr  r2, [r3, #0xe0]     ; CORE+0xe0 = PWRCTL MASK
0x0f913ae8  tst  r2, #0xc
0x0f913aec  movne r2, #5
0x0f913af0  moveq r2, #1
0x0f913af4  str  r2, [r3, #0xe8]     ; CORE+0xe8 = 5  (con MASK=0xf)
```

Con MASK = 0xf (scritto a 0x0f913fec) l'handler scrive **sempre 5**.
Corrispondenza con la sorgente CAF: `SDCC_HC_BUS_ON_OFF_SUCC = BIT(0)`,
`SDCC_HC_IO_SIG_SUCC = BIT(2)` => 1|4 = 5. **Il binario e il sorgente
corrispondono.**

**FATTO 2 — l'LK BLOCCA dopo la scrittura di HC+0x29.** In
`sdhci_setup_host`:

```
0x0f913a0c  strb r0, [r2, #0x29]     ; HC+0x29 = tensione<<1   (power OFF)
0x0f913a14  strb ip, [r3, #0x29]     ; HC+0x29 = tensione<<1|1 (power ON)
0x0f913a18  ldr  r0, [r4, #0x18]      ; &dev->sdhc_event
0x0f913a1c  bl   #0xf92e360           ; event_wait(&event)   <== BLOCCA
0x0f913a20  mov  r1, #0
0x0f913a24  mov  r0, r4
0x0f913a28  bl   #0x0f9130a0         ; clock setup: DOPO l'attesa
```

Sorgente CAF (`sdhci.c:805`):
```c
	/* Set bus power on */
	sdhci_set_bus_power_on(host);
	/* Wait for power interrupt to be handled */
	event_wait(host->sdhc_event);
```

**FATTO 3 — il driver Linux funzionante fa la stessa cosa.** In
`sdhci.c` (`sdhci_set_uhs_signaling`/`sdhci_set_bus_power_on`):

```c
	sdhci_writeb(host, pwr, SDHCI_POWER_CONTROL);       /* HC+0x29 */
	if (host->ops->check_power_status)
		host->ops->check_power_status(host, REQ_BUS_ON);
```
e in `sdhci-msm.c` (`sdhci_msm_check_power_status`):
```c
	if (done)
		init_completion(&msm_host->pwr_irq_completion);
	else if (!wait_for_completion_timeout(&msm_host->pwr_irq_completion,
				msecs_to_jiffies(MSM_PWR_IRQ_TIMEOUT_MS)))   /* 5000 ms */
		__WARN_printf("%s: request(%d) timed out waiting for pwr_irq\n", ...);
```
L'handler IRQ `sdhci_msm_pwr_irq` scrive l'ack a `CORE_PWRCTL_CTL` e fa
`complete(&msm_host->pwr_irq_completion)`.

**CORREZIONE alla deduzione D3 com'era formulata.** La deduzione diceva
"dopo una scrittura su HC+0x29 il controller attende un ack su 0xe4/0xe8".
Il fatto e' piu' preciso:

1. l'ack all'init (`sdhci_clear_power_ctrl_irq`) avviene **prima** della
   scrittura di HC+0x29, e lo calcola dal registro **STATUS** (0xdc);
2. l'ack dopo la scrittura di HC+0x29 lo fa **l'handler dell'interrupt**,
   e lo calcola dal registro **MASK** (0xe0) => scrive **5**;
3. il punto che manca al nostro shim non e' solo "scrivere 5": e' che
   **l'LK aspetta che l'handler sia stato eseguito** (event_wait) prima di
   proseguire con clock e comandi.

**FATTO 4 — il nostro shim non ha l'handler e non aspetta.**
`emmc_pwrctl_ack()` in `shim/emmc_lk_bringup.inc` scrive 0xe4 = status e
0xe8 = ctl **derivato dallo STATUS**; dopo la scrittura di HC+0x29 fa un
poll di 0xdc per 10 ms e poi richiama la stessa ack. Quindi:
- non scrive mai `CORE+0xe8 = 5` se il STATUS non ha i bit giusti;
- non aspetta alcun evento.

**IPOTESI (H5, nuova, forte).** Se il controller tiene il bus in uno stato
"power in corso" finche' non riceve l'ack `CORE+0xe8 = 5`, allora il nostro
shim emette CMD0/CMD1 mentre il bus non e' ancora utilizzabile, e la carta
risponde "muta". Questo spiegherebbe:
- perche' il run 5 ha funzionato solo con la variante 2 (0x0f) **preceduta
  da due tentativi falliti**: quei tentativi possono aver lasciato il
  controller in uno stato diverso (o fatto scattare e poi consumar l'evento);
- perche' H1 (tempo) e H4 (PMIC) sembrano plausibili ma non sono
  dimostrabili: il tempo e' un modo per aggirare un'attesa che non c'e'.

**Numeri concreti per l'IRQ (FATTO, dal DTS `msm8992-lg-h815-boot.dts`):**
```
sdhci@f9824900 {
    interrupts = <0x00 0x7b 0x00 0x00 0x8a 0x00>;
    interrupt-names = "hc_irq", "pwr_irq";
};
```
=> `hc_irq` = SPI 0x7b = **123**, `pwr_irq` = SPI 0x8a = **138**.

Nota: il codice GIC del nostro shim (`shim/shim.c`) e' **GICv2**
(0xf9000000 / 0xf9002000, dal DTS `qcom,msm-qgic2`), mentre il G4 ha
**GICv3** (GICD 0x17a00000, GICR 0x17a60000). Implementare l'handler IRQ
reale richiede quindi prima il GICv3. **Alternativa senza IRQ**:
implementare a mano l'attesa, cioe' scrivere `CORE+0xe8 = 5` e/o attendere
che `CORE+0xdc` mostri il bit di BUS_ON prima di procedere.

---

## Confronto: sequenza reale dell'LK vs `shim/emmc_lk_bringup.inc`

| # | LK (FATTO, letto nel binario) | shim oggi | stato |
|---|---|---|---|
| 1 | clock GCC (0x4c8/0x4c4) | idem | = |
| 2 | `CORE+0x78 &= ~1`; `CORE+0x00 |= 0x80`; delay(1); `CORE+0x78 |= 1`; `\|= 0x2000` | idem (delay 10 ms) | ~ |
| 3 | `HC+0x2f = 1`, poll | idem | = |
| 4 | `HC+0x10c \|= 0x18000` (condizionato a `dev->0xc != 0`) | **non lo fa** | **DIFF** |
| 5 | PWRCTL init: `0xdc`->`0xe4`; `0xe8` = ctl \| 1 (se status&3) \| 4 (se status&0xc) | idem | = |
| 6 | **registra handler IRQ PWR + unmask** | **assente** | **DIFF (H5)** |
| 7 | `CORE+0xe0 = 0xf` | idem | = |
| 8 | caps -> base clock | idem | = |
| 9 | **tensione da CAPS1: bit24/25/26 -> 3.3/3.0/1.8 V** | **0x0f fisso (3.3 V)** | **DIFF (D1)** |
| 10 | **`event_wait` dopo HC+0x29 (aspetta l'ack PWR IRQ)** | poll 0xdc 10 ms + ack derivata dallo status | **DIFF (H5)** |
| 11 | clock setup (`0x0f9130a0`: HC+0x28, HC+0x2e=0xf) DOPO l'attesa | HC+0x2e scritto PRIMA; clock dopo il power | ~ ordine |
| 12 | `HC+0x28 = 0x10` dopo il clock | `HC+0x28` &= ~0x26, scritto prima del power | ~ |
| 13 | `HC+0x34=0x000b`, `HC+0x36=0xffff`, `HC+0x38=0x000b`, `HC+0x3a=0xffff` | `HC+0x34=0x03ff00ff`, `HC+0x38=0` | **DIFF** |
| 14 | clock 393216 Hz (`0x60000`) | 400000 Hz | **DIFF (gia' noto)** |
| 15 | **CMD0 — nessun CMD5 di sveglia** | CMD5 (sleep/awake) prima di CMD0 | **DIFF** |
| 16 | CMD1 con OCR **0x40ff0000** al primo tentativo | CMD1 con OCR **0x40ff8000** | **DIFF** |
| 17 | CMD1 x1000, delay(1) | idem | = |
| 18 | uscita dal loop CMD1 quando la risposta ha bit31 (confronto *signed*) | idem (stessa logica) | = |
| 19 | CMD2 (CID), CMD3 (RCA 2), CMD7 | idem | = |
| 20 | a fine init il controller resta con il clock a 393216 Hz | shim continua sul suo percorso | - |

### Lettura del confronto

**FATTO.** L'unica differenza che spiega "il controller è sano ma la carta
è muta" e che riguarda **il bus, non i comandi** e' la #9 (tensione) e la
#10 (attesa dell'ack PWR). Tutte le altre differenze (#4, #12, #13, #14,
#15, #16) riguardano parametri che il run 4 e il run 5 hanno gia' variato
senza successo (core reset, clock, ordine CMD5/CMD0, primo OCR, vendor
spec) — quindi sono **candidate secondarie**.

**DEDUZIONE.** La carta e' muta perche' il bus non e' mai stato portato
nello stato "utilizzabile" come lo porta l'LK: tensione sbagliata (3,3 V
invece di 1,8 V) e, soprattutto, **ack PWR mancante** (H5). Il CMD0
dell'LK viene emesso solo dopo che l'handler PWR ha scritto 5.

**IPOTESI (H6).** Il valore 3,3 V in HC+0x29 non e' "sbagliato" in se' ma
**incompatibile con il resto**: con 3,3 V il controller/PMIC avvia una
transizione di tensione del bus diverse da quella che la carta e il
controller si aspettano (CAPS1 dice 1,8 V), e l'ack `CORE+0xe8 = 5` che
chiude la transizione non viene dato nella forma giusta. Le due cose
potrebbero essere un unico problema.

---

## Implicazioni dirette per T4 (preregistrazione)

Il piano di varianti va **ri impostato** sulla base di questi fatti:

- la base non deve piu' essere `{power 0x0f}` ma **`{power 0x0b}`** (il
  valore dell'LK su questa scheda);
- le varianti devono includere **l'ack PWR** (`CORE+0xe8 = 5`) e
  l'**attesa** (poll di `CORE+0xdc` / delay), non solo il valore di HC+0x29;
- V3 "senza scrivere HC+0x29" va mantenuta per coprire H4, ma va
  affiancata da una variante "senza l'ack PWR" per separare D1 da H5.

Nessuna modifica speculativa va applicata al percorso normale prima di
aver misurato: le varianti vanno testate tutte nello stesso boot.
