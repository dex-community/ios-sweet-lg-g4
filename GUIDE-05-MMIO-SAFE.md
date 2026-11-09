# GUIDE-05 — Come non bloccare più il telefono: la allowlist MMIO

Sessione 2026-11-09. Questa guida raccoglie ciò che è stato fatto dopo i due
hang del telefono e spiega **perché** il codice ora non può più bloccarsi, e
come si verifica che è vero.

---

## 1. Cosa è successo (FATTO)

Due volte, il 2026-11-09, il telefono si è bloccato con il logo LG e non è
più tornato su USB. Non era un bug del driver eMMC: era una **scrittura MMIO
a un controller mai inizializzato**.

La meccanica, verificata sul codice:

1. Il controller SDCC2 (lo slot microSD, `0xf98a4900`) **non è mai stato
   inizializzato**: il bootloader LK non lo tocca (FATTO: nessun riferimento
   a `sdc2` nel binario, `research/lk-g4/`), e il kernel Android non è ancora
   partito.
2. Il suo clock è quindi spento.
3. La scrittura di `HC+0x2f = 1` (software reset) su un blocco con il clock
   spento **non si completa**: la transazione resta appoggiata sul bus.
4. Il bus AXI è bloccato, e con lui la CPU: l'accesso MMIO non ritorna mai.
5. **Non è una fault**: nessuna eccezione, nessun handler. Il watchdog non
   era armato (FATTO: nei log dei run collaudati **non c'è nessuna riga
   `WDT`**, quindi `WDT0_EN` era 0 quando lo shim è partito). Quindi
   niente reset: il telefono resta lì per sempre.
6. L'unica salita è il pulsanti: volume giù + power, poi fastboot, poi
   riflashare `boot`.

La seconda versione della sonda ha ripetuto la cosa leggendo il TLMM
(`0xfd510000`), una finestra che lo shim non aveva mai toccato: stesso
esito. **Il principio è generale: qualunque accesso a una finestra non
inizializzata può fermare il telefono.**

---

## 2. La difesa: due livelli

### 2.1 Il livello runtime (shim/mmio.h)

Ogni accesso passa per accessori che **controllano l'indirizzo** e **si
rifiutano** a eseguire quello che non è in allowlist:

```c
static inline void mmio_wr32(uintptr_t a, uint32_t v)
{
	if (!mmio_write_ok(a, 4)) {
		mmio_denied(a, 4);   /* la scrittura NON avviene */
		return;
	}
	*(volatile uint32_t *)a = v;
}
```

La allowlist ha tre sezioni:

| sezione | finestre | lettura | scrittura |
|---|---|---|---|
| **device** | eMMC core `0xf9824000`, eMMC host `0xf9824900`, GCC `0xfc400000`, watchdog `0xf9017000`, IMEM `0xfe80f000`, GIC `0xf9000000`, PSHOLD `0xfc4ab000` | sì | sì |
| **memoria** | immagine `0x80000`, DTB `0x2000000`, ramoops `0xff00000`, scratch `0x50000000`, banco RAM 1 `0x10000000-0x5fffffff`, banco RAM 2 `0x80000000-0xdfffffff` | sì | sì |
| **sola lettura** | splash framebuffer `0x3400000` | sì | **no** (l'XPU rifiuta la scrittura) |

**Perché questi indirizzi**: non dalla memoria.

- il controller eMMC: il LK ci ha appena letto il boot image, e il bring-up lo
  guida a ogni run;
- il GCC: il LK ci abilita i clock di SDCC1;
- il watchdog: i registri sono in `drivers/soc/qcom/watchdog_v2.c:40-46` del
  tree CAF da cui è costruito il kernel del telefono;
- IMEM, GIC, PSHOLD: il shim li usa già da sempre, e funzionano;
- l'immagine, il DTB, il ramoops e i banchi RAM: dal log dei run
  collaudati (`IMAGE lo=00080000 hi=000D0000`, `DTB at=02000000`) e da
  `/proc/iomem` del telefono.

**Una cosa da non fare nella deny-path**: non usare la console. La console
scrive nel framebuffer, che sul telefono è nella regione protetta dall'XPU:
una scrittura lì è un data abort, e l'abort dentro la deny-path trasformerebbe
una scrittura rifiutata in una fault. La prima versione lo faceva e il
self-test è andato in panic con `FAR=0xfc4004c4`, esattamente l'indirizzo che
stava rifiutando. Ora la deny-path scrive solo nel trail (IMEM, RAM semplice)
e incrementa un contatore.

### 2.2 Il livello statico (tools/check-mmio-writes.py)

L'audit legge i sorgenti dello shim e **fallisce** se trova una scrittura
grezza (`*(volatile uint32_t *)addr = x`) su una finestra device: sarebbe un
buco nella protezione. La build lo esegue:

```
python3 ../tools/check-mmio-writes.py || exit 1
```

---

## 3. La rete di sicurezza: il watchdog guard (shim/wdt.c)

Il watchdog è **spento** quando lo shim parte (FATTO: nessuna riga `WDT` nei
log). Quindi un hang non aveva nessuna rete. Ora, nella build diagnostica,
lo shim **lo arma** prima di toccare i controller e lo **spara** dopo che il
log è al sicuro:

- `WDT0_BARK_TIME` (0x10), `WDT0_BITE_TIME` (0x14), `WDT0_EN` (0x08),
  `WDT0_RST` (0x04, il "pet"): i registri sono quelli del driver CAF
  `watchdog_v2.c`, non scelti a memoria;
- i tempi sono scritti come li scrive il driver:
  `timeout = bark_ms * WDT_HZ / 1000`, `bite = timeout + 3 * WDT_HZ`;
- i loop di attesa lo accarezzano (`g4_wdt_pet()` in `emmc_delay_us()`), così
  il watchdog scade solo se il processore è davvero fermo.

Con la guardia, un hang diventa un **reset**: il telefono torna da solo e il
boot successivo salta la fase che ha bloccato (il marker `trail_seen()`).

Limite onesto: il DT ha `qcom,ipi-ping`, quindi il firmware sicuro può
accarezzare il watchdog da solo e la guardia non scatta. È una rete, non una
garanzia: la garanzia è la allowlist.

---

## 4. I test (tutti offline, nessun run sul telefono)

### 4.1 Il self-test del codice (shim/mmio_selftest.c)

Con `SHIM_MMIO_SELFTEST=1` lo shim prova, all'avvio:

1. una **scrittura** su SDCC2 (`0xf98a4900`) → deve essere rifiutata;
2. una **lettura** da SDCC2 → deve essere rifiutata;
3. una **scrittura** su una finestra che lo shim usa davvero (il GCC) → deve
   **passare**.

### 4.2 Il test QEMU (tools/qemu-mmio-guard.py)

```sh
python3 tools/qemu-mmio-guard.py
```

Controlla tre cose sul dump di RAM:

- **nessun accesso alla finestra SDCC2 è arrivato al bus** (il modello
  registra ogni accesso che lo raggiunge: se non c'è, non è successo);
- **una scrittura alla finestra GCC consentita è passata** (la allowlist
  non è troppo stretta);
- **il self-test ha contato le refuse** (l'attesa è 2: la scrittura e la
  lettura negate).

Risultato (2026-11-09): **tutti i check passano**.

### 4.3 Il percorso eMMC (tools/qemu-emmc.py)

I tre modi del modello (`mute`, `ack`, `always`) devono continuare a
funzionare. In `always` la sequenza completa è quella giusta:

```
CMD5, CMD0, CMD1, CMD2, CMD3, CMD7, CMD16, CMD8, CMD24 x4, CMD17
```

con 768 accessi al buffer: il bring-up non è rotto dalla allowlist.

### 4.4 L'audit statico

```sh
python3 tools/check-mmio-writes.py
```

→ `ok: no raw store bypasses the MMIO allowlist (6 device windows checked)`

---

## 5. La regola per i prossimi run

1. **Prima la baseline**: una build collaudata (senza sonda), per essere
   sicuri che il telefono regge ancora lo shim.
2. **Una fase sola per volta**, mai concatenate.
3. **Il ripristino di `boot` stock è parte della sequenza**, non un passo
   dimenticato (`tools/run-t2.sh` lo fa da solo).
4. **Ogni accesso nuovo deve passare dalla allowlist**; se serve una
   finestra nuova, va prima provata in QEMU e poi dichiarata in
   `shim/mmio.h` **con la prova che la finestra è viva**.
5. **Se una misura richiede hardware non inizializzato**, si fa dalla
   recovery (`tools/read-mmc-registers.sh`), dove c'è un kernel vero: zero
   rischio per il telefono.

---

## 6. I file

| file | cosa fa |
|---|---|
| `shim/mmio.h` | la allowlist e gli accessori sorvegliati |
| `shim/mmio_selftest.c` | il self-test (build `SHIM_MMIO_SELFTEST=1`) |
| `shim/wdt.c`, `shim/wdt.h` | la guardia del watchdog |
| `shim/trail.h` | `trail_seen()`: rende un hang self-limiting |
| `tools/check-mmio-writes.py` | l'audit statico, dentro la build |
| `tools/qemu-mmio-guard.py` | il test QEMU della protezione |
| `tools/qemu-emmc.py` | il test QEMU del percorso eMMC |
| `tools/run-t2.sh` | la sequenza completa: flash → log → `boot` stock |
| `tools/read-mmc-registers.sh` | le misure dalla recovery (zero rischio) |

---

## 7. Comandi

```sh
cd shim

# la baseline (nessuna sonda, il percorso collaudato)
SHIM_SD_PROBE=0 RAW_TIMING=1 FINE_TIMING=1 DIAG_VARIANTS=1 DIAG_TIMING=1 ./build.sh

# la build con la sonda (solo letture)
SHIM_SD_PROBE=1 SD_PROBE_PHASE=3 RAW_TIMING=1 FINE_TIMING=1 DIAG_VARIANTS=0 DIAG_TIMING=0 ./build.sh

# i test, tutti offline
python3 ../tools/check-mmio-writes.py
python3 ../tools/qemu-mmio-guard.py
python3 ../tools/qemu-emmc.py --mode always --secs 60

# un run sul telefono, con il ripristino finale
cd .. && MODE=diag tools/run-t2.sh
```
