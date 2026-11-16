# Guida al Ripristino del Boot e Gestione KDZ / Download Mode su LG G4 (H815)

Questa guida documenta le procedure esatte per recuperare l'avvio dell'LG G4 (MSM8992 / H815) in caso di blocco, bootloop o freeze durante i test dello shim bare-metal o del kernel XNU.

---

## 1. Le Modalità Hardware del Telefono

### A. Download Mode ("Firmware Update")
* **Aspetto a schermo**: Schermata grigia/nera con icona USB e la scritta **"Firmware Update - Do not unplug the connection"**.
* **Come entrarci manualmente**:
  1. Scollega il cavo USB e togli/reinserisci la batteria (oppure spegni il telefono forzatamente).
  2. Tieni premuto **solo il tasto VOLUME SU**.
  3. Mantenendo premuto **Volume Su**, inserisci il cavo USB collegato al Mac/PC.
  4. Rilascia il tasto appena compare la schermata *Firmware Update*.
* **Protocollo**: Comunica tramite il protocollo proprietario LG **LAF** (non ADB, non Fastboot).

### B. Fastboot Mode
* **Aspetto a schermo**: Schermata nera con il testo bianco nell'angolo superiore sinistro (*"Welcome to Fastboot Mode! Quick Guidance..."*).
* **Come entrarci manualmente**:
  1. Spegni il telefono o stacca/reinserisci la batteria.
  2. Tieni premuti **VOLUME GIÙ + POWER** (oppure solo **VOLUME GIÙ** collegando il cavo USB).
* **Protocollo**: Standard Android `fastboot`.

---

## 2. Ripristino del Boot in Download Mode (LAF Mode)

Nel repository è presente lo strumento `tools/laf-tool.sh` configurato con la libreria Python `tools/lglaf/`.

### Ripristino Rapido (Consigliato)
Esegui dalla cartella radice `ios-sweet-lg-g4`:

```bash
# 1. Ripristina l'immagine di boot verificata (TWRP / pstore support)
./tools/laf-tool.sh restore-boot backup/boot-stock.img

# 2. Riavvia il telefono
./tools/laf-tool.sh reboot
```

### Ripristino con Boot Stock di Fabbrica (da KDZ)
Se desideri ripristinare il kernel stock di Android LG originale:

```bash
./tools/laf-tool.sh restore-boot kdz/extracted/boot.image
./tools/laf-tool.sh reboot
```

---

## 3. Gestione ed Estrazione dei File KDZ Ufficiali LG

I file firmware ufficiali LG (`.kdz`) sono contenitori proprietari a due livelli:
1. Il file `.kdz` contiene il file compresso `.dz`.
2. Il file `.dz` contiene le singole immagini delle partizioni raw (`boot.image`, `recovery.image`, `system.image`, `aboot.image`, ecc.).

Nel repository sono già inclusi gli strumenti `unkdz.py` e `undz.py` in `tools/kdztools/`.

### Procedura di estrazione di un nuovo firmware KDZ:

```bash
# 1. Estrarre il file .dz dal firmware KDZ
python3 tools/kdztools/unkdz.py -f "kdz/Europe H81520C_00_1218.kdz" -d kdz/

# 2. Estrarre tutte le immagini delle partizioni dal file .dz
python3 tools/kdztools/undz.py -f "kdz/H81520c_00.dz" -d kdz/extracted/
```

I file estratti compariranno in `kdz/extracted/` pronti per essere flashati tramite LAF o Fastboot.

---

## 4. Ripristino da Fastboot Mode

Se il dispositivo si trova in Fastboot Mode, il ripristino si effettua direttamente tramite i comandi fastboot o con lo script dedicato:

```bash
# Tramite script automatico:
./tools/recover-boot.sh

# Oppure manualmente:
fastboot flash boot backup/boot-stock.img
fastboot reboot
```

---

## 5. Cheat-Sheet dei Comandi Utili

| Azione | Stato Telefono | Comando |
|---|---|---|
| Lista partizioni GPT eMMC | Download Mode | `./tools/laf-tool.sh list` |
| Ripristino Boot | Download Mode | `./tools/laf-tool.sh restore-boot` |
| Riavvio | Download Mode | `./tools/laf-tool.sh reboot` |
| Ripristino Recovery | Download Mode | `./tools/laf-tool.sh restore-recovery` |
| Ripristino Boot | Fastboot Mode | `./tools/recover-boot.sh` |
| Estrazione log crash | TWRP / Android | `./tools/fetch-pstore.sh` |
