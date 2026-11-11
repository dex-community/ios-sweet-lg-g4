# Test Report: XNU Boot - 2026-11-11

## Obiettivo
Verificare il ciclo di boot di XNU e la capacità di leggere i log dopo il crash.

## Setup
- **Data**: 2026-11-11 04:03 CET
- **Immagine**: `shim/out/shim-boot-g4.img` (17MB, built 2026-11-11 03:18)
- **Kernel XNU**: `xnu/out-42942530869/kernel.development.sweet` (CI run 42942530869)
- **Payload**: `shim/out/g4.bin` (16.6MB)
- **Recovery flashata**: device-test-boot.img (con CONFIG_DEVMEM=y)

## Procedura
1. Telefono in fastboot mode
2. `fastboot boot shim/out/shim-boot-g4.img`
3. Aspettato reboot (~5 secondi)
4. Verificato stato (recovery ADB)
5. Letto log con `read-shim-log.py` e `read-xnu-log.py`

## Risultati

### Boot Sequence ✅
- Fastboot accetta l'immagine: **OK**
- Boot inizia: **OK**
- Tempo al crash: **~5 secondi** (molto veloce)
- LK boot in recovery: **OK** (comportamento corretto)

### Recovery Environment ✅
- Recovery parte automaticamente: **OK**
- ADB disponibile: **OK** (device: LGH815b448b72e, stato: recovery)
- `/dev/mem` presente: **OK** (`crw------- 1 root root 1, 1`)
- `/dev/mem` leggibile: **OK**

### Log Analysis ❌
```
Log shim (0x50000000):
  Status: VUOTO
  Motivo: Shim non scrive log prima di handoff a XNU

Log XNU console BSD:
  Status: VUOTO  
  Motivo: Console BSD non inizializzato prima del crash

Log eMMC driver:
  Status: VUOTO
  Motivo: Driver mai eseguito
```

### Parametri XNU Verificati
```
virt_base: 0xfffffe0006000000
phys_base: 0x80000000
Console ring: calcolato ma vuoto
eMMC driver log: calcolato ma vuoto
```

## Diagnosi

### Crash Timing
XNU crasha **immediatamente** all'avvio, prima di:
- Inizializzare il console BSD
- Caricare/eseguire driver (incluso eMMC)
- Scrivere qualsiasi output diagnostico

### Possibili Cause (in ordine di probabilità)
1. **Early MMU fault**: problema durante setup MMU/page tables
2. **Exception vector**: exception prima che gli handler siano pronti
3. **Bad pointer/address**: accesso a memoria non valida nel codice very early
4. **Memory layout**: conflitto tra virt_base/phys_base e layout reale
5. **ADT parsing**: problema nel device tree o boot_args

### Cosa Sappiamo
- ✅ Shim viene caricato correttamente dal bootloader (LK)
- ✅ LK passa il controllo allo shim/payload
- ❌ XNU non arriva mai al punto di inizializzare console
- ❌ XNU non esegue nessun driver
- ✅ Recovery e /dev/mem funzionano perfettamente

## Conclusioni

### Il Ciclo di Test Funziona ✅
Il flow boot → crash → recovery → lettura log è **corretto e funzionante**:
- La recovery flashata (device-test-boot.img) ha /dev/mem
- LK boot automaticamente in recovery dopo crash XNU
- Gli script di lettura log funzionano

### Il Problema è XNU ❌
XNU crasha troppo presto per lasciare tracce leggibili. Il problema non è:
- ❌ La recovery (funziona)
- ❌ Il ciclo di boot (corretto)
- ❌ Gli script di lettura (funzionano)
- ✅ **È XNU che crasha prima di scrivere qualcosa**

## Next Steps

### Immediate (diagnostiche)
1. Verificare se lo shim può scrivere un log PRIMA di saltare a XNU
2. Modificare lo shim per salvare:
   - Conferma che ha completato il setup
   - Indirizzo di salto
   - Parametri passati a XNU
3. Verificare parametri boot_args/ADT generati

### Short-term (fix XNU)
1. Confrontare virt_base/phys_base con altri port (sweet, v60)
2. Verificare che mem_size non ecceda la RAM disponibile
3. Controllare ADT per valori non validi
4. Confrontare exception vector con XNU funzionante

### Tools Needed
- Shim con logging pre-XNU handoff
- Dump completo di boot_args e ADT prima del salto
- Confronto binario kernel G4 vs sweet/v60

## Files
- Report: `test-reports/xnu-boot-test-2026-11-11.md`
- Boot image: `shim/out/shim-boot-g4.img`
- Kernel: `xnu/out-42942530869/kernel.development.sweet`
- Payload: `shim/out/g4.bin`

## Signature
Test eseguito da telefono reale LG H815 (b448b72e) in modalità fastboot/recovery.
Nessuna partizione modificata permanentemente (solo `fastboot boot` temporaneo).
