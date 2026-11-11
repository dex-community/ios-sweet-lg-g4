# Build XNU per Cortex-A53 senza LSE/PAN — come è stato fatto e come replicarlo

Data: 2026-11-11. Questo documento raccoglie tutto il lavoro della giornata:
i tre problemi che impedivano a XNU di girare sul G4 (LSE, registro `pan`,
LSE nel firehose), le patch applicate, il pre-test locale e la procedura
esatta per riprodurre tutto da zero.

TL;DR: il kernel ora non contiene **nessuna** istruzione LSE
(`ldadd`/`casa`/`ldset`/`ldclr`/`swp`), cosa che sulla CPU ARMv8.0 del G4
(Cortex-A53/A57) provoca `Undefined Instruction` al primo atomic.

---

## 1. Il problema

Il G4 (MSM8992) ha Cortex-A53 e A57: **ARMv8.0**, cioè senza LSE
(ARMv8.1 Atomic Extension) e senza il registro `PAN` (ARMv8.1).
XNU 13.5 (xnu-8796) è pensato per Apple silicon, dove LSE e PAN esistono.

Tre sintomi distinti, tutti risolti:

| # | Sintomo | Root cause | Fix |
| --- | --- | --- | --- |
| 1 | `instruction requires: lse` / `casa` nel kernel | `commpage_asm.s` usa `casa` esplicitamente | `0005b-sweet-commpage-llsc.patch` |
| 2 | `invalid register "pan" for llvm.read_register` | `__builtin_arm_rsr/wsr("pan")` in 5 file | `0010b-sweet-no-pan.patch` |
| 3 | 8 LSE residue nel kernel finale | `libfirehose_kernel.a` compilata da xcodebuild | workflow: slice arm64 con `-mcpu=cortex-a53+crypto` |

---

## 2. Le patch

### 2.1 `xnu/patches/0005b-sweet-commpage-llsc.patch`

`osfmk/arm/commpage/commpage_asm.s`, funzioni `pfz_trylock_and_enqueue` e
`pfz_trylock_and_dequeue`: `casa` (compare-and-swap con acquire barrier)
diventa un loop LL/SC:

```asm
1:	ldaxr	w10, [x3]	 // LL/SC CAS: LSE (casa) traps on Cortex-A53
	cbnz	w10, 9f		 // old value != 0 -> failed
	stlxr	w14, w11, [x3]	 // store locked value if [x3] still 0
	cbnz	w14, 1b		 // lost arbitration, retry
	b		Ltrylock_enqueue_success
9:
	mov		x0, #-1			// Failed
	b Ltrylock_enqueue_exit
```

`w14` è usato come scratch per `stlxr` (la funzione usa solo `x9-x15`).
Il `casa` in `osfmk/arm64/machine_routines_asm.s` **non** è un problema:
è dentro `#if defined(__ARM_ARCH_8_2__)`, mai assemblato in questa build.

Nota sul nome: `0006` era già occupato, quindi la patch è `0005b` (ordina
dopo `0005`, prima di `0006`).

### 2.2 `xnu/patches/0010b-sweet-no-pan.patch`

Cinque file, tutte le occorrenze di `__builtin_arm_rsr/wsr("pan")`:

- `osfmk/arm64/copyio.c`
- `osfmk/arm64/pcb.c`
- `osfmk/arm64/platform_tests.c`
- `osfmk/arm/arm_init.c`
- `osfmk/arm/pmap/pmap.c`

Le **letture** diventano `1`, le **scritture** diventano no-op:

```c
(void)(0); /* sweet: msm8992 is armv8.0, no PAN register */;
```

Perché non si abilita `+pan`: il registro non esiste in hardware,
`msr pan` traperebbe a runtime. Lo stub è la stessa via di
UniXNU-lumia950xl (stesso SoC).

Perché `0010b` e non `0005c`: la patch `0010-sweet-early-console.patch` usa
la riga `__builtin_arm_wsr("pan", 1);` come **contesto** del suo hunk, quindi
`0010b` deve essere applicata dopo `0010`.

### 2.3 `-mcpu=cortex-a53+crypto` in libfirehose (workflow, non patch)

`libfirehose_kernel.a` non è compilata dai makefile di XNU ma da
`xcodebuild` dentro `blacktop/darwin-xnu-build`. Con il default Apple
(`arm64-apple-macos15`) il compilatore emette LSE: 8 istruzioni in
`firehose_buffer_create` (`casa`, `ldclr`, `ldset`, `ldadd`, `ldaddh`, `ldaddl`).

Il workflow `/.github/workflows/xnu.yml` patcha `dxb/build.sh` in due punti:

1. **solo la slice arm64**: `ARCHS="x86_64 arm64e arm64"` → `ARCHS="arm64"`
   (la slice arm64e richiede armv8.5 e non serve al kernel SWEET);
2. **flag nella xcconfig**:
   `OTHER_CFLAGS = -mkernel -nostdinc -Wno-packed -mcpu=cortex-a53+crypto -mno-outline-atomics`

Regole da rispettare quando si tocca questo:

- **`-march=armv8-a+crypto` NON basta**: il triple `arm64-apple-macos15.0`
  implica `+lse` (tutte le Apple silicon lo hanno). Solo `-mcpu=` resetta
  davvero il set di feature. Verificato in locale:

  ```
  -march=armv8-a+crypto   -> LSE=1 (ldaddal)   LL/SC=0
  -mcpu=cortex-a53+crypto -> LSE=0              LL/SC=2
  ```

- **in xcconfig `$(inherited)` non esiste** (a differenza di make): va
  riscritto l'elenco completo dei flag, non accodato a `$(inherited)`.
- la xcconfig esiste solo dopo che `build.sh` ha clonato libdispatch →
  l'injection va fatta **dentro** `build.sh` (subito dopo le sue sed), non
  in uno step separato della CI.
- `-mno-lse` non esiste per clang/AArch64 (`unknown argument '-mno-lse'`).

---

## 3. Pre-test locale (senza aspettare la CI)

`tools/local-pretest.sh` ricompila **in locale** le translation unit
toccate dalle patch, con le **stesse flag del CI** (estratte dal log del
run) e conta le LSE nel `.o` risultante.

```bash
# una sola volta: scarica l'albero di build di un run CI verde
gh run download 43080427388 -n xnu-buildtree -D /tmp/ci-buildtree

# poi, quante volte vuoi
bash tools/local-pretest.sh
```

Output tipico:

```
== libfirehose (synthetic LSE test) ==
     [-march=armv8-a+crypto] LSE=1 LL/SC=0
     [-mcpu=cortex-a53+crypto] LSE=0 LL/SC=2
ok   osfmk/arm/commpage/commpage_asm.s  (LSE=0 LL/SC=7)
ok   osfmk/arm64/copyio.c  (LSE=0 LL/SC=0)
ok   osfmk/arm64/pcb.c  (LSE=0 LL/SC=0)
ok   osfmk/arm64/platform_tests.c  (LSE=0 LL/SC=0)
ok   osfmk/arm/arm_init.c  (LSE=0 LL/SC=0)
ok   osfmk/arm/pmap/pmap.c  (LSE=0 LL/SC=40)
ok   bsd/dev/sweet_emmc.c  (LSE=0 LL/SC=0)
```

Il gate è **reale**, non un no-op: su sorgente non patchata riproduce
`instruction requires: lse` (commpage) e `invalid register "pan"` (copyio).

Attenzione al regex: `\bldadd\b` NON matcha `ldaddal`/`ldaddh`/`ldaddl`.
Quello giusto è `\b(ldadd|casa|ldclr|ldset|swp)[a-z]*\b`.

**La build XNU completa in locale non è possibile**: servono il KDK 13.5
(installazione con `sudo`) e un SDK ≤ 15; in locale c'è solo Xcode 27
(SDK 27, senza `Kernel.framework`). Il pre-test copre le modifiche; la
build completa resta in CI.

---

## 4. Procedura completa, da zero

### 4.1 Verificare la catena di patch (locale, ~30 s)

```bash
git clone --branch xnu-8796.141.3 --depth 1 \
  https://github.com/apple-oss-distributions/xnu.git /tmp/xnu-patchtest
bash tools/check-xnu-patch-chain.sh /tmp/xnu-patchtest
```

Atteso: `applied: 84  failed: 1`, l'unico fallito è
`0012-sweet-keep-debug-console-after-ttbr0.patch` (fallimento **preesistente**,
file non correlati: `osfmk/arm64/arm_vm_init.c`, `osfmk/arm/arm_init.c`).

La CI **non** segnala i patch che non si applicano: `build.sh` fa
`git apply --check` e, se fallisce, stampa solo un warning e va avanti.
Per questo esiste `tools/check-xnu-patch-chain.sh`.

### 4.2 Costruire il kernel (CI)

```bash
git push origin main           # la CI parte sui path xnu/patches/** e sul workflow
gh run list --workflow xnu.yml -L 1
```

Il workflow:

1. clona `blacktop/darwin-xnu-build` al SHA `663906528d1cd5ea8fde7de6f16de73db9511549`;
2. copia `xnu/patches/*` in `dxb/patches/`;
3. patcha `dxb/build.sh` (slice arm64 + flag del firehose, vedi 2.3);
4. builda XNU 13.5 con `MACHINE_CONFIG=SWEET`, `KERNEL_CONFIG=DEVELOPMENT`;
5. pubblica l'artifact `xnu-kernel` (il kernel) e `xnu-buildtree`
   (sorgenti + header generati, per il pre-test locale).

Runner: `macos-15` (macos-14 ritirato il 2026-11-02), Xcode 16.0 (la 15 non
c'è più su macos-15).

### 4.3 Check 4 (verifica del kernel)

```bash
RUN=43080427388
gh run download $RUN -n xnu-kernel -D xnu/out-$RUN/
K=xnu/out-$RUN/kernel.development.sweet

file $K
# -> kernel.development.sweet: Mach-O 64-bit executable arm64

/usr/bin/objdump -d $K | grep -cE '\b(casa|casp|ldadd|ldclr|ldeor|ldset|swp)[a-z]*\b'
# -> 0     (zero istruzioni LSE)

/usr/bin/objdump -d $K | grep -cE '\b(ldxr|stxr|ldaxr|stlxr)[a-z]*\b'
# -> 2226  (solo LL/SC)
```

### 4.4 Immagine e boot (solo `fastboot boot`)

```bash
bash tools/build-g4-image.sh $RUN g4
# produce shim/out/g4.adt, shim/out/g4.bin, shim/out/shim-boot-g4.img

fastboot boot shim/out/shim-boot-g4.img
```

**Mai `fastboot flash`**: `userdata` (rootfs iOS 16.7.16) si legge e basta.

### 4.5 Leggere i log

Il telefono, dopo il crash di XNU, torna in recovery da solo (watchdog +
`LK_RECOVERY_MODE`, ~60 s). La RAM viene riusata, quindi i ring di XNU
(`sweet_early_buf`, console ring) sono persi se si legge dopo.

Il log dello **shim** si legge dalla recovery tramite pstore:

```bash
adb shell 'cat /sys/fs/pstore/console-ramoops-0' | tr -d '\r' > /tmp/ramoops.txt
```

Le ultime righe del ramoops sono il log dello shim di quell'ultimo boot:

```
INITRD lo=02200000 hi=03239340
PHYS base=80000000 size=5FF00000
VIRT FFFFFE0006000000
ENTRY FFFFFE00074A8488
SEGS n=0000000A end=8252C000
BOOTARG pa=82530000 top=82534000
JUMP 00000000814A8488
XNU jumping (fbcon clears screen)
[G4SHIMPS1-END]
```

Per i ring di XNU (`tools/read-xnu-early.py`, `tools/read-xnu-log.py`)
servono `/dev/mem` e leggere **subito** dopo il crash, prima che la recovery
riusi la RAM.

---

## 5. Stato al 2026-11-11 (fine giornata)

- Kernel CI `43080427388`: **verde**, Mach-O arm64, **0 LSE**, 2226 LL/SC.
- Build delle immagini: OK, `shim-boot-g4.img` (sha256
  `f517639e95cd768e3a9c69016f798306ee9b197831e35cc955c45f57b2eb7b9c`).
- Boot sul G4: lo shim carica 10 segmenti e **salta a XNU**
  (`JUMP 00000000814A8488`); il telefono torna in recovery via watchdog.
- **Non ancora stabilito** a che punto XNU si ferma: i ring in RAM
  (`sweet_early_buf`, console ring) erano vuoti al momento della lettura
  perché la recovery aveva già riusato la RAM.
- Prossimo passo: catturare l'output di XNU (buffer early/console) prima
  che la recovery lo sovrascriva, poi il driver eMMC per XNU.

## 6. Comandi utili per il prossimo giro

```bash
# riepilogo dei run
gh run list --workflow xnu.yml -L 5

# log di un run
gh run view --log --job=<jobid> | grep -a "Applying patch\|Skipping patch"

# check LSE su un kernel qualsiasi
/usr/bin/objdump -d <kernel> | grep -oE '\b(casa|casp|ldadd|ldclr|ldset|swp)[a-z]*\b' | sort | uniq -c

# catena di patch
bash tools/check-xnu-patch-chain.sh /tmp/xnu-patchtest

# pre-test locale
bash tools/local-pretest.sh
```
