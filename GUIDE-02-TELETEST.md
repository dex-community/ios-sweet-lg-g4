# GUIDE-02 — Bring-up sul LG G4 reale

Come si porta avanti il porting sul telefono vero: cosa funziona, cosa non
funziona, e come si fa a leggere i log senza schermo.

## 1. Cosa dice il telefono su se stesso

Tutto questo è stato letto dal telefono, non da suppose.

| Domanda | Come | Risposta |
| --- | --- | --- |
| Il bootloader è LK o ABL? | `fastboot getvar all` | **`kernel:lk`**, versione `0.5` |
| `fastboot boot` esiste? | `fastboot boot img` | **no**: `FAILED (remote: 'unknown command')` |
| `fastboot fetch` esiste? | `fastboot fetch boot f` | **no**: file da 0 byte |
| `fastboot reboot recovery`? | | accettato, ma il telefono torna in fastboot |
| Il bootloader è sbloccato? | `fastboot getvar unlocked` | `yes` |
| Il pannello? | `/proc/cmdline` | `mdss_mdp.panel=1:dsi:0:qcom,mdss_dsi_jdi_nt36850_1440p_mipi0_cmd:1:..._mipi1_cmd` → JDI NT36850 1440p, **dual MIPI DSI in command mode** |
| Il framebuffer? | `/sys/class/graphics/fb0` | `mdssfb_90000  1440,5120  stride=5760  bpp=32` → **ARGB8888**, stride 5760 |
| L'eMMC? | `/proc/cmdline` | `androidboot.bootdevice=f9824900.sdhci` |
| Il touch? | `/sys/bus/i2c/devices` | `2-0020` → driver **`s3528`** (Synaptics); `2-0028` → `lg2411` (sensore LG, **non** il touch) |
| La RAM? | `/proc/iomem` (in recovery) | vedi sotto |
| Root? | `adb shell id` | in recovery sì (`uid=0`), in AOSP no |

### La mappa RAM reale (`/proc/iomem`)

```
00000000-063fffff : System RAM          100 MB
07200000-073fffff : System RAM            2 MB
0f400000-0fefffff : System RAM           11 MB
0ff00000-0ffdffff : persistent_ram         1 MB   (ramoops)
10000000-5fffffff : System RAM         1280 MB   <-- banco basso grande
60000000-7fffffff : (buco, non RAM)
80000000-dfffffff : System RAM         1536 MB   <-- banco alto
```

Totale 3 GiB (`androidboot.ddr_size=3221225472`). Il buco a
`60000000-7fffffff` è la regione che il bootloader tiene per sé.

**Per XNU la scelta è `PHYS_BASE = 0x80000000`, `MEM_SIZE = 0x60000000`
(1.5 GiB)**: il banco più alto, sopra tutti i carve-out della XPU
(`secure_mem` 0x0-0x9600000, `modem_mem` 0x9000000-0xd400000, …). È la
stessa logica con cui lo sweet sceglie la "upper DDR bank" sul Redmi.

## 2. Il problema: lo schermo non mostra niente

Il shim scrive il testo in `cont_splash_mem` (0x03400000, 12 MiB), il
pannello è in **command mode** e il bootloader ha riprogrammato il
controller DSI: la finestra non viene più scandita. Il telefono resta con
il logo LG.

La cosa importante è che **lo shim funziona**: il pannello non mostra
niente, ma il telefono torna nel bootloader dopo esattamente il ritardo
impostato dal `reset(N)` — cioè lo shim arriva in fondo a `shim_main`,
salva il log e si riavvia da solo. La prova è il timing:

| Ritardo nel codice | Tempo prima di riapparire in fastboot | Lettura |
| --- | --- | --- |
| `reset(40)` (percorso di panic) | ~50 s | panic a un certo punto |
| `reset(180)` | ~195 s | percorso normale completo |
| `reset(15)` | ~15 s | percorso normale completo |

## 3. Il canale log: la RAM sopravvive al reset

Il testo del console è disegnato come pixel, e il pannello non lo mostra.
Ma i pixel sono in RAM, e un warm reset **non scrubba la DRAM**.

Il shim tiene anche una **copia ASCII** di tutto quello che stampa
(`shim/console.c`, buffer `con_log`), e prima di ogni reset la copia in
un indirizzo scratch:

```
0x50000000 : "G4SHIMLG" + il log ASCII
```

`0x50000000` è dentro il banco `10000000-5fffffff`, abbastanza in alto da
non essere toccato né dal bootloader né dal kernel di recovery.

Per rileggerlo serve una recovery con root (il percorso è
`fastboot reboot` → Android → `adb reboot recovery`; **`fastboot reboot
recovery` non funziona** su questo LK):

```sh
./tools/pull-mem.sh dump 0x50000000 0x10000 /tmp/shim-log.bin
strings /tmp/shim-log.bin | tail -40
```

`tools/pull-mem.sh scan` fa anche il giro di cercare un flattened device
tree in una zona di RAM: LK riscrive `/memory` prima di saltare al kernel e
quel tree **non** è nel DTB su disco.

### Perché non abbiamo usato `/proc/device-tree`

Su questo telefono il kernel è compilato senza `CONFIG_PROC_DEVICETREE`:
`/proc/device-tree` e `/sys/firmware/devicetree/base` non esistono. Da
recovery con root si legge `/dev/mem`.

## 4. Il ciclo di boot per i test - VERIFICATO 2026-11-11

Il ciclo di test per XNU funziona correttamente:

```
1. fastboot boot shim-boot-g4.img      # boot XNU
2. XNU crasha (~5 secondi)              # crash immediato
3. LK boot in recovery automaticamente  # recovery con /dev/mem
4. leggi i log con tools/read-xnu-log.py
```

**Confermato funzionante**:
- ✅ Recovery flashata ha `/dev/mem` (device-test-boot.img)
- ✅ LK boot automaticamente in recovery dopo crash
- ✅ `/dev/mem` accessibile da recovery
- ✅ Script `read-xnu-log.py` e `read-shim-log.py` funzionano

**Problema attuale**: XNU crasha **troppo presto** (prima che console BSD sia
attivo), quindi i log sono vuoti. Il problema non è il ciclo di test, ma
l'early crash di XNU.

### 4.1 Come si scrive una partizione (e come si ripristina)

Questo LK non ha `fastboot boot`, quindi per eseguire il nostro shim
**va scritta una partizione**. La procedura usata:

```sh
# 0. backup (da recovery, con root) - una volta sola
B=/dev/block/bootdevice/by-name
for p in boot persist fsg fsc modemst1 modemst2 devinfo; do
    adb shell "dd if=$B/$p of=/sdcard/gg-$p.img bs=1M"
    adb pull /sdcard/gg-$p.img backup/$p-stock.img
done

# 1. scrivere lo shim
fastboot flash boot shim/out/shim-boot.img
fastboot reboot

# 2. quando il telefono torna nel bootloader, ripristinare
fastboot flash boot backup/boot-stock.img
```

`backup/boot-stock.img` è la copia **byte per byte** della partizione del
telefono (40 MB), non l'immagine dello zip AICP: le due differiscono (il
kernel è più grande nel ROM installato). Entrambe hanno però lo stesso
`dt.img` QCDT, che differisce dallo zip AICP solo in tre proprietà
irrilevanti (`qcom,idle-timeout`, i due `pm-qos-*`, `qcom,popp-enable`).

**`userdata` non è mai stata toccata.**

## 5. Root

Il telefono **non ha root**: nessun `su`, nessun pacchetto SuperSU, nessun
Magisk, `/data/adb` non esiste. SuperSU inoltre non supporta Android 13
(API 33) e su una ROM AICP non funziona.

Il root serve solo per la recovery (per il backup e per `/dev/mem`); per il
porting non è necessario, perché lo shim legge da solo il device tree che
il bootloader gli passa.

La recovery presente è una build "omni" (`omni_g4-eng 16.1.0
OPM8.181105.002 23 test-keys`) e **ha root**: `adb shell id` → `uid=0(root)`.

## 6. Cosa ci si aspetta al prossimo giro

Con il log in RAM si risolvono in un colpo:

1. se il DTB è stato trovato e a che indirizzo (`DTB at=… magic=…`);
2. la mappa RAM reale come la vede il bootloader (`/memory` compilato), che
   è la dato che serve per `physBase`/`memSize` dell'ADT;
3. `ID_AA64MMFR0_EL1.TGran1`, cioè se il G4 ha il granulo da 16 KiB che
   XNU usa (il kernel Android del G4 gira a 4 KiB);
4. la versione PSCI, se il trust zone risponde.

Il display resta il lavoro grosso della Fase 4: il pannello è in command
mode, quindi serve un driver che programmi MDP e DSI per mandare il
framebuffer, non basta scrivere in RAM.
