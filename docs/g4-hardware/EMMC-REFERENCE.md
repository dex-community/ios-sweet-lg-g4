# eMMC reference: UniXNU-lumia950xl (MSM8992) → LG G4 H815 (MSM8992)

Research report. Every factual claim below is backed by a `file:line` reference and a
verbatim quote from the repository named in the section header.

---

## 0. Provenance and corrected paths

The brief gave the reference repo as `/Users/io/Dev/ios/UniXNU-lumia950xl`. **That path
does not exist.** The repo is at:

```
/Users/io/Dev/ios/lg g4/UniXNU-lumia950xl
```

Verified with `ls -la /Users/io/Dev/ios/UniXNU-lumia950xl` → `No such file or directory`,
and `ls -la /Users/io/Dev/ios/` listing `lg g4/`, `ios-sweet/`, `ios-sweet-lg-g4/`,
`ios-v60/`, `ipsw/`.

Repos used:

| Role | Path | Notes |
| --- | --- | --- |
| eMMC / storage reference | `/Users/io/Dev/ios/lg g4/UniXNU-lumia950xl` | read-only for this task |
| Porting base (target) | `/Users/io/Dev/ios/ios-sweet-lg-g4` | report written here |
| UFS reference (XNU integration) | `/Users/io/Dev/ios/ios-v60` | see §11 note |
| LG G4 device tree | `/Users/io/Dev/ios/lg g4/msm8992-lg-h815-boot.dts` | stock LG H815 DT |
| LG G4 AOSP tree | `/Users/io/Dev/ios/lg g4/device_lge_g4-common` | not needed for this report |

---

## 1. Executive summary

* The eMMC driver lives in **`drivers/NativeSdhci/`** of `UniXNU-lumia950xl` — five C++
  sources plus an assembly binding file. It is a **kernel-space IOKit driver**: an
  `IOBlockStorageDevice` subclass linked into the XNU kernel binary as a static archive,
  published at runtime through an IOKit personality.
* It is **not** a BSD `bdevsw` disk. It speaks SDHCI + MMC natively, using a **PIO bounce
  buffer** for the primary path and an optional **ADMA2** path for reads.
* The XNU-side contract is three-layered and *not* the one `ios-sweet` uses:
  1. a **personality** added at runtime by an XNU overlay patch (`native_sdhci_publish()`),
  2. the class **linked into the kernel** via `-Wl,-force_load`,
  3. the class's `IOBlockStorageDevice` imports **bound to absolute addresses** in the
     pinned 16.7.16 kernelcache's prelinked `IOStorageFamily` kext.
* `ios-sweet` / `ios-v60` use a **completely different** contract: a BSD `bdevsw` disk
  (`bsd/dev/sweet_ufs.c`), a `devfs` node named `ufsroot0`, and a hook in
  `IOFindBSDRoot` keyed on `rd=ufsroot0`. `ios-sweet` has **no IOMedia at all** (it patches
  `IOService::findRootMedia` to tolerate the missing `IOMedia` class).
* The LG G4 (H815) is also **MSM8992**, and its stock device tree puts the eMMC controller
  at **the same address and the same IRQs** as the Lumia 950 XL. The register-level work is
  therefore largely reusable; the **integration** is not.

---

## 2. The eMMC driver: exact location and role

`UniXNU-lumia950xl/drivers/NativeSdhci/`:

| File | Lines | Role |
| --- | --- | --- |
| `NativeSdhci.cpp` | 972 | IOKit wrapper: `IOBlockStorageDevice` subclass, MMIO mapping, card probe, GPT, DMA setup, `doAsyncReadWrite` |
| `NativeSdhci.h` | 129 | class declaration |
| `PollingHost.cpp` | 560 | SDHCI command engine + MMC card state machine (`PollingHost`, `WarmCard`) |
| `PollingHost.h` | 182 | declarations, transfer-size limits |
| `SdhciCore.cpp` | 195 | freestanding helpers: CSD/EXT_CSD decode, GPT parsing, CRC32 |
| `SdhciCore.h` | 89 | declarations |
| `SdhciRegs.h` | 188 | SDHCI register offsets and bit definitions |
| `SdhciTypes.h` | 28 | fixed-width integer typedefs (kernel builds have no hosted `<stdint.h>`) |
| `BoardConstants.h` | 94 | Lumia/MSM8992 board constants |
| `original-bindings-16.7.16.S` | 33 | absolute-address bindings for the prelinked `IOStorageFamily` |
| `Info.plist` | 16 | kext-style personality declaration (unused by the in-kernel path — see §10.4) |
| `Test/check-sdhci-core.cpp`, `Test/check-polling-host.cpp` | — | host unit tests |

The class is an IOKit service, not a BSD device:

`UniXNU-lumia950xl/drivers/NativeSdhci/NativeSdhci.h:15`
```cpp
class NativeSdhci : public IOBlockStorageDevice {
```

`UniXNU-lumia950xl/drivers/NativeSdhci/NativeSdhci.cpp:36`
```cpp
OSDefineMetaClassAndStructors(NativeSdhci, IOBlockStorageDevice)
```

**There is no userland daemon, no `ioctl` interface and no `IONotify` use in this driver.**
`grep` for `ioctl`, `IONotify`, `IOService` was run across the whole repo; `IONotify`
appears only as `IOInterruptEventSource`-adjacent IOKit headers pulled in by
`NativeSdhci.cpp` — the driver itself is a **polled** driver with no interrupt source.
Confirmed by `docs/storage-bringup.md:9-12`:

> ```
> - Explicit-width, volatile, barrier-ordered SDHCI MMIO; both mapped windows are
>   length-checked. No clock, voltage, bus-width, DLL, or vendor power writes.
> - Bounded CMD/DAT reset, inhibition, command, buffer, transfer, and card-state
>   waits. Polling status is enabled separately from disabled interrupt signaling.
> ```

---

## 3. Register map (verbatim)

`UniXNU-lumia950xl/drivers/NativeSdhci/SdhciRegs.h:23-54`
```cpp
// Register offsets, relative to the "hc" window.
enum : u32 {
    kRegDmaAddress       = 0x00,
    kRegBlockSize        = 0x04,
    kRegBlockCount       = 0x06,
    kRegArgument         = 0x08,
    kRegTransferMode     = 0x0C,
    kRegCommand          = 0x0E,
    kRegResponse         = 0x10,   // 0x10..0x1F
    kRegBufferPort       = 0x20,
    kRegPresentState     = 0x24,
    kRegHostControl1     = 0x28,
    kRegPowerControl     = 0x29,
    kRegBlockGapControl  = 0x2A,
    kRegWakeUpControl    = 0x2B,
    kRegClockControl     = 0x2C,
    kRegTimeoutControl   = 0x2E,
    kRegSoftwareReset    = 0x2F,
    kRegInterruptStatus  = 0x30,   // 32-bit: normal in the low half, error in the high half
    kRegInterruptEnable  = 0x34,
    kRegInterruptSignal  = 0x38,
    kRegAutoCmd12Error   = 0x3C,
    kRegHostControl2     = 0x3E,
    kRegCapabilities     = 0x40,
    kRegCapabilities1    = 0x44,
    kRegMaxCurrent       = 0x48,
    kRegAdmaErrorStatus  = 0x54,
    kRegAdmaAddress      = 0x58,   // 64-bit address in 0x58 and 0x5C
    kRegPresetValue      = 0x60,   // 0x60..0x7F
    kRegSlotIntStatus    = 0xFC,
    kRegHostVersion      = 0xFE,
};
```

Important detail on the interrupt register — `SdhciRegs.h:5-9`:

> ```
> // Offsets follow the SDHCI specification (Host Controller 3.0 layout, which is
> // what Linux's drivers/mmc/host/sdhci.h uses) and are relative to the "hc"
> // window at 0xf9824900 on this board. Note that the interrupt registers are
> // 32-bit words at 0x30/0x34/0x38: the low half is the normal status and the
> // high half the error status.
> ```

Error bits, `SdhciRegs.h:136-147`:
```cpp
    kIntError             = 0x00008000,
    kErrCommandTimeout    = 0x00010000,
    kErrCommandCrc        = 0x00020000,
    kErrCommandEndBit     = 0x00040000,
    kErrCommandIndex      = 0x00080000,
    kErrDataTimeout       = 0x00100000,
    kErrDataCrc           = 0x00200000,
    kErrDataEndBit        = 0x00400000,
    kErrCurrentLimit      = 0x00800000,
    kErrAutoCmd12         = 0x01000000,
    kErrAdma              = 0x02000000,
```

MMC command indices actually used, `SdhciRegs.h:167-184`:
```cpp
// eMMC commands used by the bring-up path.
enum : u8 {
    kCmd0GoIdleState       = 0,
    kCmd1SendOpCond        = 1,
    kCmd2AllSendCid        = 2,
    kCmd3SetRelativeAddr   = 3,
    kCmd6Switch            = 6,
    kCmd7SelectCard        = 7,
    kCmd8SendExtCsd        = 8,
    kCmd9SendCsd           = 9,
    kCmd12StopTransmission = 12,
    kCmd13SendStatus       = 13,
    kCmd16SetBlockLength   = 16,
    kCmd17ReadSingleBlock  = 17,
    kCmd18ReadMultiple     = 18,
    kCmd24WriteSingleBlock = 24,
    kCmd25WriteMultiple    = 25,
};
```

> **Note on `kCmd16SetBlockLength` / `kCmd18` / `kCmd25`:** the constants exist
> (`SdhciRegs.h:179,181,183`) but the bring-up path **never issues them**. Verified by
> `grep -n 'kCmd16SetBlockLength\|kCmd18ReadMultiple\|kCmd25WriteMultiple'` across
> `drivers/NativeSdhci/` → matches only in `SdhciRegs.h`. The implemented path uses
> **CMD17 single-block** (`PollingHost.cpp:264-275`) and CMD24 single-block writes
> (`PollingHost.cpp:288-298`). Block length is left at the controller default 512 set in
> `PollingHost.cpp:140`.

---

## 4. Base addresses and board constants

`UniXNU-lumia950xl/drivers/NativeSdhci/BoardConstants.h:29-71`
```cpp
namespace lumia950 {

// SDHCI host registers, the "hc" region of the device tree node.
constexpr sdhci::u64 kSdhciHcPhys   = 0xf9824900ULL;
constexpr sdhci::u32 kSdhciHcSize   = 0x1a0;

// Qualcomm QSDCC vendor window, the "core" region. The DLL and power-control
// registers needed for HS200/HS400 live here; this driver does not touch them
// yet, so the window is mapped only for diagnostics.
constexpr sdhci::u64 kSdhciCorePhys = 0xf9824000ULL;
constexpr sdhci::u32 kSdhciCoreSize = 0x800;

// GICv2 interrupts from the node. The Lumia 950's controller is a
// "qcom,msm-qgic2" at 0xf9000000/0xf9002000, not a GICv3 with redistributors.
constexpr sdhci::u32 kIrqHc           = 123;
constexpr sdhci::u32 kIrqPower        = 138;

// Card clock source: XO is 19.2 MHz and GCC supplies SDCC1, but the firmware
// leaves the controller clocked and the card selected. The first bring-up rung
// therefore never reprograms the clock; it measures what is there instead.
constexpr sdhci::u32 kXoHz = 19200000;

// Logical block size reported to IOBlockStorageDriver. eMMC logical sectors are
// 512 bytes; transfers are performed in 512-byte blocks for now.
constexpr sdhci::u32 kLogicalBlockBytes = 512;

// Global clock controller (msm8994 "qcom,gcc" at 0xfc400000). Only SDCC1's root clock
// generator and branch are touched, with the register layout and frequency plan of the
// cityman kernel's clock-gcc-8994.c: CMD_RCGR 0x4d0 (CFG +4, M +8, N +0xc, D +0x10),
// APPS_CBCR 0x4c4, AHB_CBCR 0x4c8. Sources: 0 is XO, 1 is GPLL0 (600 MHz).
constexpr sdhci::u64 kGccPhys = 0xfc400000ULL;
constexpr sdhci::u32 kGccSize = 0x1000;
constexpr sdhci::u32 kGccSdcc1AppsCbcr = 0x4c4;
constexpr sdhci::u32 kGccSdcc1AhbCbcr = 0x4c8;
constexpr sdhci::u32 kGccSdcc1AppsCmdRcgr = 0x4d0;

// The firmware's card does not answer after ExitBootServices (CMD13 times out at every
// RCA), so the driver identifies it again and assigns this address.
constexpr sdhci::u16 kColdRelativeAddress = 2;

// Legacy (backward-compatible) eMMC timing tops out at 26 MHz; 25 MHz is the plan's
// GPLL0 / 12 * 1/2 entry. HS and HS200 come later with CMD6 and the DLL.
constexpr sdhci::u32 kTransferHz = 25000000;
```

Remaining constants, `BoardConstants.h:73-90`:
```cpp
// High-speed SDR on the 8-bit bus once EXT_CSD offers it: GPLL0 / 12, the plan's 50 MHz entry.
constexpr sdhci::u32 kHighSpeedHz = 50000000;

// The one partition the write path may touch: p55, carved out of userdata for the iOS root. Writes
// also need boot-arg sdhcirw=1; without both the medium reports itself write protected.
constexpr char kWritablePartition[] = "ios";

// The console log: a header block and this many ring blocks at the very end of the writable
// partition, used only when they lie past the end of the HFS+ volume that starts it.
constexpr sdhci::u32 kConsoleLogBlocks = 65536;   // 32 MiB of text

// eMMC relative card address. The firmware assigned one and selected the card;
// these are the values EDK2 and Linux hosts normally use, tried in order.
constexpr sdhci::u16 kRcaCandidates[] = { 1, 2 };

// The GPT header sits in LBA 1 with the signature "EFI PART". Reading it is the
// cheapest end-to-end proof that the command path works on real silicon.
constexpr sdhci::u64 kGptHeaderLba = 1;
constexpr char kGptSignature[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
```

The header's provenance comment, `BoardConstants.h:3-22`:
> ```
> // Lumia 950 (MSM8992, Snapdragon 808) board constants for the eMMC path.
> //
> // Source for every value below: the mainline MSM8992 device tree, which is
> // msm8994.dtsi with two A57 cores removed. Node `sdhc1`, alias `mmc1`:
> //
> //   mmc@f9824900 {
> //       compatible = "qcom,msm8994-sdhci", "qcom,sdhci-msm-v4";
> //       reg = <0xf9824900 0x1a0>, <0xf9824000 0x800>;
> //       reg-names = "hc", "core";
> //       interrupts = <GIC_SPI 123 IRQ_TYPE_LEVEL_HIGH>,
> //                    <GIC_SPI 138 IRQ_TYPE_LEVEL_HIGH>;
> //       interrupt-names = "hc_irq", "pwr_irq";
> //       clocks = <&gcc GCC_SDCC1_AHB_CLK>, <&gcc GCC_SDCC1_APPS_CLK>, <&xo_board>;
> //       bus-width = <8>;
> //       non-removable;
> //   };
> ```

> **This is the single most important fact for the LG G4 port**: the constants are
> derived from the **SoC** device tree (`msm8994.dtsi` = MSM8992), not from anything
> Lumia-specific. See §16 for the LG H815 confirmation.

The window mapping is done from the ADT node's `reg` entries, not from the constants:

`UniXNU-lumia950xl/drivers/NativeSdhci/NativeSdhci.cpp:177-192`
```cpp
bool NativeSdhci::mapWindows(IOService *provider)
{
    // IOPlatformDevice publishes the ADT node's reg entries as device memory,
    // in order: 0 is the SDHCI window, 1 the Qualcomm core window.
    hcMap = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    coreMap = provider->mapDeviceMemoryWithIndex(1, kIOMapInhibitCache);
    if (!hcMap || !coreMap) {
        IOLog("NativeSdhci: provider has no reg windows (hc %p core %p); "
              "the ADT node needs both reg entries\n", hcMap, coreMap);
        return false;
    }
    if (hcMap->getLength() < lumia950::kSdhciHcSize) {
```

The GCC window is mapped separately, `NativeSdhci.cpp:199-208`:
```cpp
    // The clock controller is not part of the ADT node: map just the page that holds
    // SDCC1's clock registers. Without it the driver can still try the warm handoff.
    IOMemoryDescriptor *gccRange = IOMemoryDescriptor::withPhysicalAddress(
        lumia950::kGccPhys, lumia950::kGccSize, kIODirectionInOut);
    if (gccRange) {
        gccMap = gccRange->map(kIOMapInhibitCache);
        gccRange->release();
    }
    if (gccMap) gcc = reinterpret_cast<volatile uint8_t *>(gccMap->getVirtualAddress());
    else IOLog("NativeSdhci: GCC window did not map; no cold identification\n");
```

---

## 5. Initialization sequence

### 5.1 Entry point

`UniXNU-lumia950xl/drivers/NativeSdhci/NativeSdhci.cpp:740-777`
```cpp
bool NativeSdhci::start(IOService *provider)
{
    if (!IOBlockStorageDevice::start(provider)) return false;

    gate = IOLockAlloc();
    bounce = static_cast<uint8_t *>(IOMallocAligned(kMaxTransferBlocks * lumia950::kLogicalBlockBytes, 64));
    if (!gate || !bounce) {
        IOLog("NativeSdhci: no lock or bounce buffer\n");
        return false;
    }
    if (!mapWindows(provider)) return false;

    IOLog("NativeSdhci: hc at %p (virtual), core at %p\n",
          reinterpret_cast<void *>(const_cast<uint8_t *>(hc)),
          reinterpret_cast<void *>(const_cast<uint8_t *>(core)));

    logHost();
    logVendor();
    native_sdhci_stage = 0;

    if (!probeCard()) {
        IOLog("NativeSdhci: card probe failed; not publishing a medium\n");
        return false;
    }

    IOLockLock(gate);
    online = true;
    IOLockUnlock(gate);
    __atomic_store_n(&native_sdhci_ready, 1, __ATOMIC_RELAXED);

    IOLog("NativeSdhci: %llu blocks of %u bytes (%llu bytes), %s\n",
          static_cast<unsigned long long>(blockCount), lumia950::kLogicalBlockBytes,
          static_cast<unsigned long long>(capacityBytes),
          card.writable() ? "writable in the window above" : "read-only");
    registerService();
    startConsoleLog();
    return true;
}
```

### 5.2 Probe: warm attach first, cold identify as fallback

`NativeSdhci.cpp:702-738`
```cpp
bool NativeSdhci::probeCard()
{
    const RegisterIo registers = {
        this, ioRead8, ioRead32, ioWrite8, ioWrite16, ioWrite32, ioDelay
    };
    if (!host.configure(registers)) {
        logFailure();
        return false;
    }
    if (!card.attach(host, lumia950::kRcaCandidates,
                     sizeof lumia950::kRcaCandidates / sizeof lumia950::kRcaCandidates[0])) {
        logFailure();
        IOLog("NativeSdhci: firmware card did not answer; identifying it from CMD0\n");
        if (!identifyCold()) return false;
    }
    relativeAddress = card.info().relativeAddress;
    capacityBytes = card.info().capacityBytes;
    blockCount = capacityBytes / lumia950::kLogicalBlockBytes;
    __atomic_store_n(&native_sdhci_rca, relativeAddress, __ATOMIC_RELAXED);
    native_sdhci_stage = 3;
    IOLog("NativeSdhci: RCA %u, CSD %u, MMC %u, EXT_CSD %u, user partition, 512-byte sectors, "
          "%u-bit bus at %u Hz\n",
          relativeAddress, card.info().csd.structure, card.info().csd.mmcVersion,
          card.info().extCsd.revision, card.info().busWidth, card.info().clockHz);

    alignas(8) uint8_t gpt[lumia950::kLogicalBlockBytes] = {};
    if (!readBlocksPio(lumia950::kGptHeaderLba, 1, gpt)) return false;
    if (memcmp(gpt, lumia950::kGptSignature, sizeof lumia950::kGptSignature) != 0) {
        IOLog("NativeSdhci: GPT signature missing; not publishing the firmware medium\n");
        return false;
    }
    native_sdhci_stage = 4;
    IOLog("NativeSdhci: GPT signature read successfully\n");
    findWritableWindow();
    setupDma();
    return card.ready();
}
```

### 5.3 Warm attach — CMD13 at RCA 1/2, then CMD7 / CMD9 / CMD8

`UniXNU-lumia950xl/drivers/NativeSdhci/PollingHost.cpp:329-353`
```cpp
bool WarmCard::attach(PollingHost &controller, const u16 *candidates, u32 count)
{
    detach();
    card = {};
    lastError = WarmCardError::None;
    host = &controller;
    if (!candidates || !count || count > 16) return fail(WarmCardError::InvalidArgument);
    u32 response[4];
    bool selected = false;
    for (u32 candidate = 0; candidate < count; ++candidate) {
        if (!candidates[candidate]) continue;
        if (!host->start()) return fail(WarmCardError::Host);
        if (!host->command(kCmd13SendStatus, static_cast<u32>(candidates[candidate]) << 16,
                           ResponseType::R1, response)) continue;
        const u32 state = cardState(response[0]);
        if (!(response[0] & kReadyForData) || (state != kStateStandby && state != kStateTransfer)) {
            return fail(WarmCardError::UnexpectedState);
        }
        card.relativeAddress = candidates[candidate];
        selected = state == kStateTransfer;
        break;
    }
    if (!card.relativeAddress) return fail(WarmCardError::NoRelativeAddress);
    return finish(selected);
}
```

**RCA shifting**: the RCA is placed in the upper 16 bits of the command argument —
`static_cast<u32>(candidates[candidate]) << 16` (above). The same shift appears for
CMD9 and CMD7 in `finish()` (`PollingHost.cpp:413,416`) and for CMD3
(`PollingHost.cpp:389`).

### 5.4 Cold identify — CMD0 → CMD1 (OCR) → CMD2 (CID) → CMD3 (RCA)

`PollingHost.cpp:355-404`
```cpp
bool WarmCard::initialize(PollingHost &controller, u16 relativeAddress, const BusControl &bus, u32 transferHz)
{
    detach();
    card = {};
    lastError = WarmCardError::None;
    host = &controller;
    if (!relativeAddress || !bus.setClock || !bus.setBusWidth || !transferHz || transferHz > 26000000) {
        return fail(WarmCardError::InvalidArgument);
    }
    card.cold = true;
    if (!bus.setBusWidth(bus.context, 1) || !bus.setClock(bus.context, kIdentificationHz)) {
        return fail(WarmCardError::BusClock);
    }
    host->setClearSettle(40);
    if (!host->start() || !host->command(kCmd0GoIdleState, 0, ResponseType::None, nullptr)) {
        return fail(WarmCardError::Host);
    }
    host->pause(1000);
    u32 response[4];
    for (u32 poll = 0; poll < kOperatingConditionPolls; ++poll) {
        if (!host->command(kCmd1SendOpCond, kOcrRequest, ResponseType::R3, response)) {
            return fail(WarmCardError::Host);
        }
        card.ocr = response[0];
        if (card.ocr & kOcrReady) break;
        host->pause(1000);
    }
    if (!(card.ocr & kOcrReady)) return fail(WarmCardError::OperatingCondition);
    if (!(card.ocr & kOcrSectorMode)) return fail(WarmCardError::UnsupportedCard);
    if (!host->command(kCmd2AllSendCid, 0, ResponseType::R2, response)) return fail(WarmCardError::Host);
    for (u32 word = 0; word < 4; ++word) card.cid[word] = response[word];
    // CMD3 once completed nothing on hardware (no response, no timeout status). Reset the
    // command line and send it again; if the card already took the address it no longer
    // answers CMD3 in standby, so CMD13 settles which case it was.
    const u32 address = static_cast<u32>(relativeAddress) << 16;
    bool assigned = host->command(kCmd3SetRelativeAddr, address, ResponseType::R1, response);
    if (!assigned && host->start()) {
        assigned = host->command(kCmd3SetRelativeAddr, address, ResponseType::R1, response);
        if (!assigned && host->start()) {
            assigned = host->command(kCmd13SendStatus, address, ResponseType::R1, response) &&
                       cardState(response[0]) == kStateStandby;
        }
    }
    if (!assigned) return fail(WarmCardError::Host);
    card.relativeAddress = relativeAddress;
    if (!bus.setClock(bus.context, transferHz)) return fail(WarmCardError::BusClock);
    host->setClearSettle(0);
    card.clockHz = transferHz;
    return finish(false);
}
```

**OCR handling** — `PollingHost.cpp:13-18`:
```cpp
// CMD1 argument: sector addressing, 2.7-3.6 V and 1.70-1.95 V.
constexpr u32 kOcrRequest = 0x40ff8080u;
constexpr u32 kOcrReady = 1u << 31;
constexpr u32 kOcrSectorMode = 1u << 30;
constexpr u32 kIdentificationHz = 400000;
constexpr u32 kOperatingConditionPolls = 1000;
```

So: **CMD1 is polled up to 1000 times, 1 ms apart**, with the *same* argument
`0x40ff8080` each time (no busy-bit mirroring — the driver waits for the card's own
`OCR[31]` busy-ready bit rather than echoing it, which is the simpler and safer
behaviour). The card must report `OCR[31] = 1` (ready) and `OCR[30] = 1` (sector mode).

### 5.5 Finish — CMD7 select / CMD9 CSD / CMD8 EXT_CSD / qualification

`PollingHost.cpp:406-430`
```cpp
bool WarmCard::finish(bool selected)
{
    u32 response[4];
    if (selected && !host->command(kCmd7SelectCard, 0, ResponseType::None, nullptr)) {
        return fail(WarmCardError::Host);
    }
    if (!host->waitCardReady(card.relativeAddress, kStateStandby) ||
        !host->command(kCmd9SendCsd, static_cast<u32>(card.relativeAddress) << 16,
                       ResponseType::R2, response)) return fail(WarmCardError::Host);
    const bool validCsd = decodeCsd(response, &card.csd);
    if (!host->command(kCmd7SelectCard, static_cast<u32>(card.relativeAddress) << 16,
                       ResponseType::R1, response) ||
        !host->waitCardReady(card.relativeAddress, kStateTransfer)) return fail(WarmCardError::Host);
    if (!validCsd) return fail(WarmCardError::InvalidCsd);
    if (card.csd.mmcVersion < 4) return fail(WarmCardError::UnsupportedCard);
    if (!readExtCsd()) return false;
    if (card.extCsd.partitionAccess) return fail(WarmCardError::WrongPartition);
    if (card.extCsd.commandQueueEnabled) return fail(WarmCardError::CommandQueue);
    if (card.extCsd.revision < 2 || card.extCsd.sectorBytes != 512 ||
        card.extCsd.capacityBytes <= (2ULL << 30)) return fail(WarmCardError::UnsupportedCard);
    if (!host->waitCardReady(card.relativeAddress, kStateTransfer)) return fail(WarmCardError::Host);
    card.capacityBytes = card.extCsd.capacityBytes;
    attached = true;
    return true;
}
```

Note the CMD7 argument quirk: the *deselect* uses argument `0` with no response
(`ResponseType::None`), while the *select* uses `RCA << 16` with an R1 response.

### 5.6 CMD6 switch (bus width / HS timing)

`PollingHost.cpp:440-447`
```cpp
bool WarmCard::switchExtCsd(u8 index, u8 value)
{
    u32 response[4];
    // CMD6 access 3 (write byte): the card answers R1b and reports SWITCH_ERROR in the next status.
    return host->command(kCmd6Switch, (3u << 24) | (static_cast<u32>(index) << 16) | (static_cast<u32>(value) << 8),
                         ResponseType::R1Busy, response) &&
           host->waitCardReady(card.relativeAddress, kStateTransfer, kProgramPolls);
}
```
EXT_CSD indices used: `kExtCsdBusWidth = 183`, `kExtCsdHsTiming = 185`
(`PollingHost.cpp:21-22`).

### 5.7 CRC handling

CRC is **not computed by the driver**. It is delegated to the controller:

`PollingHost.cpp:153-160`
```cpp
    u16 flags = data ? kCmdDataPresent : 0;
    switch (type) {
    case ResponseType::None: break;
    case ResponseType::R1: flags |= kCmdResponse48 | kCmdCrcCheck | kCmdIndexCheck; break;
    case ResponseType::R1Busy: flags |= kCmdResponse48Busy | kCmdCrcCheck | kCmdIndexCheck; break;
    case ResponseType::R2: flags |= kCmdResponse136 | kCmdCrcCheck; break;
    case ResponseType::R3: flags |= kCmdResponse48; break;
    }
```

The controller reports a CRC failure via `kErrCommandCrc` / `kErrDataCrc` in the upper
half of the interrupt status register, and the driver treats any error bit as fatal
(§6). `SdhciCore.cpp` does expose a `crc32()` helper (`SdhciCore.h:72`) but it is used
for **GPT header/entry CRC validation**, not for MMC traffic — the comment at
`SdhciCore.h:81-85`:
> ```
> // `header` is LBA 1 (512 bytes) and `entries` the partition array it points to, `entriesBytes` long.
> // Both CRCs, the header revision and size, the entry geometry and the partition bounds against the
> // usable range and `lastLba` of the medium are checked; a missing or duplicated name is refused.
> ```

---

## 6. Command-status decode and how it waits for command-complete

### 6.1 The wait loop

`UniXNU-lumia950xl/drivers/NativeSdhci/PollingHost.cpp:96-108`
```cpp
bool PollingHost::waitStatus(u32 wanted, u32 &remaining, HostPhase phase, HostError timeout)
{
    u32 status = 0;
    while (remaining) {
        --remaining;
        status = io.read32(io.context, kRegInterruptStatus);
        lastStatus = status;
        if (status & kErrorStatus) return fail(HostError::Controller, phase, status);
        if ((status & wanted) == wanted) return true;
        io.delayUs(io.context, limits.delayUs);
    }
    return fail(timeout, phase, status);
}
```

`PollingHost.cpp:6-9`
```cpp
constexpr u32 kErrorStatus = 0xffff8000u;
constexpr u32 kPollingStatus = kIntCommandComplete | kIntTransferComplete |
                               kIntBufferReadReady | kIntBufferWriteReady | kIntError | 0x03ff0000u;
constexpr u32 kCardErrors = 0xffff0000u | (1u << 7);
```

So **every error bit** in the high half (0xffff8000) aborts the command, including
error bits that arrive without the `kIntError` summary bit. The doc confirms:

`docs/storage-bringup.md:12-14`
> ```
> - Bounded CMD/DAT reset, inhibition, command, buffer, transfer, and card-state
>   waits. Polling status is enabled separately from disabled interrupt signaling.
> - Controller error checks, including error bits without the summary bit. R1
>   error status is not mistaken for a successful response.
> ```

### 6.2 Full command issue

`PollingHost.cpp:110-197` (verbatim, abridged only where marked):
```cpp
bool PollingHost::issue(u8 index, u32 argument, ResponseType type, const DataPhase *data, u32 response[4])
{
    last = {};
    last.command = index;
    if (!running) return fail(HostError::NotReady, HostPhase::Idle);
    if (index > 63 || static_cast<u8>(type) > static_cast<u8>(ResponseType::R3) ||
        (type != ResponseType::None && !response)) {
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }
    if (data && (type != ResponseType::R1 || !data->blocks ||
                 data->blocks > (data->dmaTable ? kMaxDmaBlocks : kMaxTransferBlocks) ||
                 (data->dmaTable ? (data->in || data->out) : !data->in == !data->out))) {
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }

    const u32 inhibit = kPresentCmdInhibit |
                        ((data || type == ResponseType::R1Busy) ? static_cast<u32>(kPresentDataInhibit) : 0u);
    bool available = false;
    for (u32 poll = 0; poll < limits.commandPolls; ++poll) {
        if (!(io.read32(io.context, kRegPresentState) & inhibit)) {
            available = true;
            break;
        }
        io.delayUs(io.context, limits.delayUs);
    }
    if (!available) return fail(HostError::InhibitTimeout, HostPhase::Inhibit);

    acknowledge(kPollingStatus | kErrorStatus);
    u16 mode = 0;
    if (data) {
        io.write16(io.context, kRegBlockSize, 512);
        io.write16(io.context, kRegBlockCount, static_cast<u16>(data->blocks));
        mode = kModeBlockCountEnable | ((data->in || data->dmaTable) ? kModeDirectionRead : 0);
        if (data->blocks > 1) mode |= kModeMultiBlock | kModeAutoCmd12;
        if (data->dmaTable) {
            io.write32(io.context, kRegAdmaAddress, data->dmaTable);
            io.write32(io.context, kRegAdmaAddress + 4, 0);
            io.write8(io.context, kRegHostControl1, static_cast<u8>((io.read8(io.context, kRegHostControl1) &
                                                                     ~kHostDmaSelectMask) | kHostDmaSelectAdma2));
            mode |= kModeDmaEnable;
        }
    }
    io.write16(io.context, kRegTransferMode, mode);
    ...
    io.write32(io.context, kRegArgument, argument);
    io.write16(io.context, kRegCommand, static_cast<u16>((index << 8) | flags));
    ...
    const bool completed = waitStatus(kIntCommandComplete, remaining, HostPhase::Command,
                                      HostError::CommandTimeout);
    trace.status = lastStatus;
    if (!completed) return false;
    if (type != ResponseType::None) trace.response = io.read32(io.context, kRegResponse);
    if (response) {
        for (u32 word = 0; word < 4; ++word) response[word] = 0;
        if (type == ResponseType::R2) {
            u32 registers[4];
            for (u32 word = 0; word < 4; ++word) {
                registers[word] = io.read32(io.context, kRegResponse + word * 4);
            }
            normalizeResponse136(registers, response);
        } else if (type != ResponseType::None) {
            response[0] = io.read32(io.context, kRegResponse);
        }
    }
    acknowledge(kIntCommandComplete);
    if (type == ResponseType::R1 || type == ResponseType::R1Busy) {
        last.cardStatus = response[0];
        if (response[0] & kCardErrors) return fail(HostError::CardStatus, HostPhase::Card);
    }
    if (type == ResponseType::R1Busy) {
        remaining = limits.transferPolls;
        if (!waitStatus(kIntTransferComplete, remaining, HostPhase::Transfer, HostError::DataTimeout)) {
            return false;
        }
        acknowledge(kIntTransferComplete);
    }
    return !data || moveData(*data);
}
```

**R2 (136-bit) response handling**: the four 32-bit registers are read from
`0x10,0x14,0x18,0x1C` and passed through `normalizeResponse136()`
(`SdhciCore.h:28`), which un-reverses and un-shifts them. `docs/storage-bringup.md:15-16`:
> ```
> - Reversed and shifted SDHCI R2 registers; modern eMMC CSD structure 3 is valid.
>   MMC capacity decoding no longer applies an SDHC-like fixed multiplier.
> ```

**Card-status decode** — `kCardErrors = 0xffff0000u | (1u << 7)`:
the R1 status word's bits 8..15 are the "current state" field; bit 7 (`1u << 7`) is the
`ADDRESS_ERROR`/error flag group. The current-state extraction is:

`PollingHost.cpp:24-27`
```cpp
u32 cardState(u32 response)
{
    return (response >> 9) & 15;
}
```

with `kStateStandby = 3`, `kStateTransfer = 4` (`PollingHost.cpp:11-12`) and
`kReadyForData = 1u << 8` (`PollingHost.cpp:10`).

### 6.3 Card-ready wait

`PollingHost.cpp:299-315`
```cpp
bool PollingHost::waitCardReady(u16 relativeAddress, u32 state, u32 polls)
{
    ...
}
```
(uses `kCmd13SendStatus` with `RCA << 16`, polling `kProgramPolls = 2000` times, 1 ms
apart — `PollingHost.cpp:19-20`:
```cpp
// CMD13 polls (1 ms apart) after a write or a CMD6 switch: programming may take a while.
constexpr u32 kProgramPolls = 2000;
```
)

---

## 7. Data transfer method

### 7.1 PIO (primary path)

`PollingHost.cpp:199-258`
```cpp
// One 512-byte block per buffer-ready event, then the transfer completion. Errors are checked
// between blocks rather than between words: a status read per FIFO word would double the MMIO
// cost of every block, and a damaged block is never reported as transferred either way.
bool PollingHost::moveData(const DataPhase &data)
{
    if (data.dmaTable) {
        // The host walks the descriptor table; an ADMA error (or any other) ends the wait.
        ...
```

`docs/storage-bringup.md:21-22`:
> ```
> - Single-block CMD17 PIO. Transfer-complete may arrive after the last FIFO word;
>   it is polled with the same whole-block budget instead of sampled once.
> ```

The bounce buffer is a kernel heap allocation of `kMaxTransferBlocks * 512` bytes,
64-byte aligned (`NativeSdhci.cpp:745`).

### 7.2 ADMA2 (optional, reads only, below 4 GiB)

`UniXNU-lumia950xl/drivers/NativeSdhci/PollingHost.h:28,31`
```cpp
constexpr u32 kMaxTransferBlocks = 128;
...
constexpr u32 kMaxDmaBlocks = 1024;
```

`NativeSdhci.cpp:374-393`
```cpp
// Buffers, then the self-test: the GPT area and a stretch of the 'ios' partition read by PIO and by
// ADMA2 (1, 8, kMaxTransferBlocks and kMaxDmaBlocks blocks) must agree byte for byte, or reads stay PIO.
void NativeSdhci::setupDma()
{
    uint32_t enabled = 1;
    PE_parse_boot_argn("sdhcidma", &enabled, sizeof enabled);
    if (!enabled) {
        IOLog("NativeSdhci: ADMA2 off (sdhcidma=0), reads by PIO\n");
        return;
    }
    const uint32_t bytes = kMaxDmaBlocks * lumia950::kLogicalBlockBytes;
    dmaBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut |
        kIOMemoryPhysicallyContiguous, bytes, 0x00000000fffff000ULL);
    dmaTable = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut |
        kIOMemoryPhysicallyContiguous, 4096, 0x00000000fffff000ULL);
    if (!dmaBuffer || !dmaTable || !physicalBelow4G(dmaBuffer, dmaBufferPhys) ||
        !physicalBelow4G(dmaTable, dmaTablePhys)) {
        IOLog("NativeSdhci: no DMA buffers below 4 GiB, reads by PIO\n");
        return;
    }
```

`NativeSdhci.cpp:354-372`
```cpp
// The eMMC host is not cache coherent, and on this kernel IOMemoryDescriptor's incoherent-I/O
// operations can compile to a barrier only: clean and invalidate the lines by hand.
static void cleanInvalidate(const void *address, size_t length)
{
    uintptr_t line = reinterpret_cast<uintptr_t>(address) & ~static_cast<uintptr_t>(63);
    const uintptr_t end = reinterpret_cast<uintptr_t>(address) + length;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; line < end; line += 64) __asm__ volatile("dc civac, %0" :: "r"(line) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool physicalBelow4G(IOBufferMemoryDescriptor *buffer, uint32_t &phys)
{
    IOByteCount length = 0;
    const addr64_t at = buffer->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    if (!at || length < buffer->getLength() || at + buffer->getLength() > 0x100000000ULL) return false;
    phys = static_cast<uint32_t>(at);
    return true;
}
```

The hardware note confirms the measurement, `docs/lumia950xl/HARDWARE.md:70-76`:
> ```
> ## Storage
>
> - Samsung eMMC (CID 15010042…), 61,071,360 × 512 B, EXT_CSD rev 7. Host version 0x3C02, capabilities
>   0x742DC8B2 (ADMA2, 64-bit), base clock 200 MHz (the MSM host ignores the SDHCI divider; the GCC SDCC1 RCG
>   sets the card clock).
> - 8-bit HS at 50 MHz: PIO ~10 MB/s; ADMA2 (32-bit descriptors, buffers below 4 GiB, manual `dc civac`
>   around each transfer: the host is not cache-coherent) ~27 MB/s with 64 KiB commands.
> ```

**No FDT / device-tree-based DMA.** No FDT parsing anywhere in the driver.

---

## 8. How the block device is exposed to XNU

### 8.1 The class

`NativeSdhci.h:15-40` (declaration excerpt)
```cpp
class NativeSdhci : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(NativeSdhci)

public:
    ...
    IOReturn reportRemovability(bool *value) override;
    IOReturn reportWriteProtection(bool *value) override;
    IOReturn doSynchronizeCache() override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                              IOStorageAttributes *attributes, IOStorageCompletion *completion) override;
```

### 8.2 The I/O entry point

`NativeSdhci.cpp:873-942` (head of the function)
```cpp
IOReturn NativeSdhci::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                       IOStorageAttributes *, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !completion->action || !count) {
        refused("no buffer, completion or count", block, count);
        return kIOReturnBadArgument;
    }
    if (block >= blockCount || count > blockCount - block) {
        refused("beyond the medium", block, count);
        return kIOReturnBadArgument;
    }
    const IODirection direction = buffer->getDirection();
    ...
    IOLockLock(gate);
    if (!online) {
        IOLockUnlock(gate);
        refused("medium offline after an I/O failure", block, count);
        return kIOReturnNotReady;
    }

    uint64_t done = 0;
    IOReturn status = kIOReturnSuccess;
    while (done < bytes) {
        const uint64_t lba = block + done / lumia950::kLogicalBlockBytes;
        ...
        if (direction == kIODirectionIn) {
            const uint8_t *data = bounce;
            if (dma) {
                if (readBlocksDma(lba, chunk)) {
                    data = static_cast<const uint8_t *>(dmaBuffer->getBytesNoCopy());
                } else {
                    // Back to PIO for good: reset the lines and the card, then this stretch again in
                    // PIO-sized pieces (the bounce buffer holds kMaxTransferBlocks).
                    IOLog("NativeSdhci: ADMA2 read failed at LBA %llu +%u (host error %u phase %u, adma error "
                          "0x%02x); PIO from now on\n", ...);
                    dma = false;
                    card.recover();
                    continue;
                }
            }
            if (data == bounce && !readBlocksPio(lba, chunk, bounce)) {
                status = kIOReturnIOError;
                break;
            }
            if (buffer->writeBytes(done, data, length) != length) {
                status = kIOReturnIOError;
                break;
            }
        } else {
            if (buffer->readBytes(done, bounce, length) != length) {
                status = kIOReturnIOError;
                break;
```

### 8.3 Service registration

`NativeSdhci.cpp:774` — `registerService();` inside `start()`.

`registerService()` is what makes the device visible to the **prelinked
`com.apple.iokit.IOStorageFamily` kext's `IOBlockStorageDriver`**, which attaches to
`IOBlockStorageDevice` and creates the `IOMedia` and the `disk0` node. There is **no
`AppleSD`, `AppleAHCI` or `AppleNAND` code anywhere in this repo** — the storage stack
is the stock Apple one, from the kernelcache.

### 8.4 The personality

`UniXNU-lumia950xl/drivers/NativeSdhci/Info.plist:10-15`
```xml
  <key>IOKitPersonalities</key><dict><key>Lumia 950 eMMC</key><dict>
    <key>IOClass</key><string>NativeSdhci</string>
    <key>IOProviderClass</key><string>IOPlatformDevice</string>
    <key>IONameMatch</key><string>qcom,msm8994-sdhci</string>
    <key>IOProbeScore</key><integer>1000</integer>
  </dict></dict>
```

This plist is the *kext-style* declaration. The **actually used** path is the runtime
personality in §10.1.

---

## 9. GPT, partition selection and the write window

`NativeSdhci.cpp:473-495`
```cpp
void NativeSdhci::findWritableWindow()
{
    uint32_t enable = 0;
    const bool asked = PE_parse_boot_argn("sdhcirw", &enable, sizeof(enable)) && enable;
    // Header at LBA 1, then the 128-entry array the header points at (LBA 2..33 on this medium).
    alignas(8) uint8_t header[lumia950::kLogicalBlockBytes];
    sdhci::GptRange range = {};
    if (!readBlocksPio(1, 1, header)) return;
    const uint64_t entriesLba = static_cast<uint64_t>(header[72]) | (static_cast<uint64_t>(header[73]) << 8);
    if (entriesLba < 2 || entriesLba > 64 || !readBlocksPio(entriesLba, 32, bounce)) return;
    if (!sdhci::findGptPartition(header, bounce, 32 * lumia950::kLogicalBlockBytes, blockCount - 1,
                                 lumia950::kWritablePartition, &range)) {
        IOLog("NativeSdhci: no valid GPT partition '%s'; the medium stays read-only\n",
              lumia950::kWritablePartition);
        return;
    }
    windowFirst = range.first;
    windowLast = range.last;
    if (asked) card.allowWrites(range.first, range.last);
    IOLog("NativeSdhci: GPT '%s' LBA %llu-%llu, writes %s\n", lumia950::kWritablePartition,
          static_cast<unsigned long long>(range.first), static_cast<unsigned long long>(range.last),
          card.writable() ? "enabled there only" : "off (boot-arg sdhcirw=1 enables them)");
}
```

Key points for the port:
* GPT header at **LBA 1**, entry array at **LBA 2..33** (32 × 512-byte blocks).
* The writable partition is named **`"ios"`** (not `"userdata"` as in `ios-sweet`).
* Writes need **both** the partition name and the boot-arg **`sdhcirw=1`**.
* The driver publishes the **whole device**, not just the partition.

`docs/storage-bringup.md:25-26`:
> ```
> - The XNU wrapper requires an actual GPT-signature read before publishing media.
>   A signature is not a GPT CRC or partition-table validation.
> ```

---

## 10. The XNU-side integration pattern (UniXNU)

This is the part that differs most from `ios-sweet`. Three separate mechanisms, all
required.

### 10.1 Runtime personality publication

`UniXNU-lumia950xl/unixnu-overlay/patches/xnu-8796/zzv-aes-shim.patch:51-86`
```
+/*
+ * The eMMC driver (device NativeSdhci, bound to the 16.7.16 IOStorageFamily) has no entry in
+ * KernelConfigTables: a personality there overlaps other patches' context. When the class is linked,
+ * this adds it at runtime, matched to the ADT sdhci node by its compatible string.
+ */
+void
+native_sdhci_publish(void)
+{
+	const OSSymbol *name = OSSymbol::withCString("NativeSdhci");
+	if (name == NULL) {
+		return;
+	}
+	if (OSMetaClass::getMetaClassWithName(name) == NULL) {
+		name->release();
+		return;
+	}
+	OSDictionary *personality = OSDictionary::withCapacity(5);
+	OSArray *drivers = OSArray::withCapacity(1);
+	const OSSymbol *provider = OSSymbol::withCString("IOPlatformDevice");
+	OSString *compatible = OSString::withCString("qcom,msm8994-sdhci");
+	OSNumber *score = OSNumber::withNumber(1000ULL, 32);
+	if (personality && drivers && provider && compatible && score) {
+		personality->setObject(gIOClassKey, name);
+		personality->setObject(gIOProviderClassKey, provider);
+		personality->setObject(gIONameMatchKey, compatible);
+		personality->setObject(kIOProbeScoreKey, score);
+		drivers->setObject(personality);
+		IOLog("NativeSdhci: personality %s\n", gIOCatalogue->addDrivers(drivers, true) ? "published" : "REFUSED");
+	}
+	OSSafeReleaseNULL(personality);
+	OSSafeReleaseNULL(drivers);
+	OSSafeReleaseNULL(provider);
+	OSSafeReleaseNULL(compatible);
+	OSSafeReleaseNULL(score);
+	name->release();
+}
```

The patch touches **`iokit/Kernel/IOStartIOKit.cpp`** (`zzv-aes-shim.patch:1-2`).

### 10.2 The call site

`UniXNU-lumia950xl/unixnu-overlay/patches/xnu-8796/zzw-procmon.patch:324-333`
```
@@ -787,6 +1093,10 @@
 	 *	Start the user bootstrap.
 	 */
 #ifdef  MACH_BSD
+	native_aes_shim_publish();
+	native_sdhci_publish();
+	native_keystore_publish();
+	procmon_start();
 	bsd_init();
 #endif
```

The patch touches **`osfmk/kern/startup.c`** (`zzw-procmon.patch:1-2`). The declaration
is at `zzw-procmon.patch:17`:
```
+extern void native_sdhci_publish(void);
```

So: **`kernel_startup()` → `native_sdhci_publish()` → `gIOCatalogue->addDrivers()` →
personality matches the ADT `IOPlatformDevice` whose `compatible` is
`qcom,msm8994-sdhci` → `NativeSdhci::start(provider)`.**

### 10.3 Linking the archive into the kernel

`UniXNU-lumia950xl/unixnu-overlay/build-xnu-4k-baseline-native-sdhci.patch:1-22`
```
--- a/scripts/build-xnu-4k-baseline.sh
+++ b/scripts/build-xnu-4k-baseline.sh
@@ -251,6 +251,9 @@
 fi
 if [ "${XNU_NATIVE_UFS:-0}" = 1 ]; then
   PERSONALITY_CFLAGS="$PERSONALITY_CFLAGS -DXNU_NATIVE_UFS=1"
+fi
+if [ "${XNU_NATIVE_SDHCI:-0}" = 1 ]; then
+  PERSONALITY_CFLAGS="$PERSONALITY_CFLAGS -DXNU_NATIVE_SDHCI=1"
 fi
 NATIVE_VIRTIO_CONSOLE=${XNU_NATIVE_VIRTIO_CONSOLE:-0}
@@ -660,6 +663,12 @@
     KERNEL_EXTRA_ARCHIVES="$KERNEL_EXTRA_ARCHIVES -Wl,-force_load,$UFS_ARCHIVE"
     KERNEL_ARCHIVE_DEPENDENCIES="$KERNEL_ARCHIVE_DEPENDENCIES $UFS_ARCHIVE"
   fi
+  if [ "${XNU_NATIVE_SDHCI:-0}" = 1 ]; then
+    SDHCI_ARCHIVE=$ROOT/build/native-sdhci/libNativeSdhci.a
+    test -f "$SDHCI_ARCHIVE"
+    KERNEL_EXTRA_ARCHIVES="$KERNEL_EXTRA_ARCHIVES -Wl,-force_load,$SDHCI_ARCHIVE"
+    KERNEL_ARCHIVE_DEPENDENCIES="$KERNEL_ARCHIVE_DEPENDENCIES $SDHCI_ARCHIVE"
+  fi
```

The `XNU_NATIVE_SDHCI=1` define also gates the **parked** KernelConfigTables personality
(§10.4).

### 10.4 The parked (unused) KernelConfigTables personality

`UniXNU-lumia950xl/patches-parked/native-sdhci-personality.patch:1-17` (entire file)
```
--- a/iokit/KernelConfigTables.cpp
+++ b/iokit/KernelConfigTables.cpp
@@ -56,6 +56,14 @@
     "     'IOProviderClass' = IOPlatformExpertDevice;"
     "     'IOProbeScore'    = 0:32;"
     "   }"
+#if defined(XNU_NATIVE_SDHCI) && XNU_NATIVE_SDHCI
+    "   ,{"
+    "     'IOClass'         = NativeSdhci;"
+    "     'IOProviderClass' = IOPlatformDevice;"
+    "     'IONameMatch'     = 'qcom,msm8994-sdhci';"
+    "     'IOProbeScore'    = 1000:32;"
+    "   }"
+#endif
 #if defined(XNU_NATIVE_AES) && XNU_NATIVE_AES
     "   ,{"
     "     'IOClass'         = NativeAES;"
```

**This patch is parked and NOT applied.** The reason is stated in the runtime-publish
patch itself (`zzv-aes-shim.patch:52-54`): *"has no entry in KernelConfigTables: a
personality there overlaps other patches' context."* `patches-parked/` contains this
file and nothing else (verified: `ls -la patches-parked/` → one 572-byte file).

### 10.5 Binding `IOBlockStorageDevice` imports to the prelinked kext

`UniXNU-lumia950xl/drivers/NativeSdhci/original-bindings-16.7.16.S:1-15`
```
// NativeSdhci's IOBlockStorageDevice imports bound to the original IOStorageFamily of the pinned
// iOS 16.7.16 kernelcache (iPhone10,6 20H392), as placed by [kernel.prelink] (slide -0xdc4000).
// The class has upstream's audited Darwin 23 shape: 190 slots, 14 pure virtual, size 0x90, one
// protected constructor forwarding to IOService, D1/D0 abstract traps, D2 = D1 - 4 tail-calling
// IOService's D2. Inherited methods are bound through the original vtable 0xfffffff005eaff50.
.globl __ZN20IOBlockStorageDevice10gMetaClassE
.set __ZN20IOBlockStorageDevice10gMetaClassE, 0xfffffff006c451d0
.globl __ZTV20IOBlockStorageDevice
.set __ZTV20IOBlockStorageDevice, 0xfffffff005eaff50
.globl __ZN20IOBlockStorageDeviceC2EPK11OSMetaClass
.set __ZN20IOBlockStorageDeviceC2EPK11OSMetaClass, 0xfffffff004815d98
.globl __ZN20IOBlockStorageDeviceD2Ev
.set __ZN20IOBlockStorageDeviceD2Ev, 0xfffffff004815db8
.globl __ZN20IOBlockStorageDevice4initEP12OSDictionary
.set __ZN20IOBlockStorageDevice4initEP12OSDictionary, 0xfffffff004815e30
```

The builder enforces this. `UniXNU-lumia950xl/scripts/build-native-sdhci16.py:1-30`
(entire file)
```python
#!/usr/bin/env python3
"""Build NativeSdhci and bind its IOBlockStorageDevice imports to the pinned 16.7.16 IOStorageFamily.

build-arm-driver.py compiles drivers/NativeSdhci into build/native-sdhci/libNativeSdhci.a; this adds
the recorded original bindings (drivers/NativeSdhci/original-bindings-16.7.16.S) and refuses to
finish if the compiled driver imports a base-class symbol they do not cover, or the abstract
complete/deleting destructors (which trap in the original).
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/native-sdhci"

subprocess.run([sys.executable, str(ROOT / "scripts/build-arm-driver.py"), "NativeSdhci"], check=True)
bindings = ROOT / "drivers/NativeSdhci/original-bindings-16.7.16.S"
bound = set(re.findall(r"^\.globl (\S+)$", bindings.read_text(), re.M))
undefined = subprocess.check_output(["xcrun", "nm", "-u", str(OUT / "NativeSdhci.o")], text=True).split()
base = {name for name in undefined if "IOBlockStorageDevice" in name}
if any(re.search(r"IOBlockStorageDeviceD[01]Ev$", name) for name in base):
    raise SystemExit("NativeSdhci must not call the abstract complete/deleting destructors")
missing = sorted(base - bound)
if missing:
    raise SystemExit("unbound IOBlockStorageDevice imports: " + ", ".join(missing))
subprocess.run(["xcrun", "clang", "-target", "arm64-apple-ios16.0", "-Werror", "-Wno-incompatible-sysroot",
                "-c", str(bindings), "-o", str(OUT / "original-bindings.o")], check=True)
subprocess.run(["xcrun", "ar", "rs", str(OUT / "libNativeSdhci.a"), str(OUT / "original-bindings.o")], check=True)
print(f"NativeSdhci: {len(base)} IOBlockStorageDevice imports bound")
```

### 10.6 The prelinked `IOStorageFamily` must be in the kernelcache

`UniXNU-lumia950xl/board.toml:112-136`
```toml
[kernel.prelink]
tool = "kits/NativePrelink/emit_prelink.py"
kernel = "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped"
kernelcache_sha256 = "2ec297bac95bcba01046fd0148e97e7e5885c05a6c18827f39f534ba375c1221"
slide = "-0xdc4000"
early_security_init = "com.apple.driver.AppleMobileFileIntegrity=0xfffffff005703c54"
roots = [
  "com.apple.AppleFSCompression.AppleFSCompressionTypeZlib",
  # Code signing: exec of /sbin/launchd faults on the NULL `amfi` interface table
  # (ubc_cs_blob_add > accelerate_entitlement_queries) without it.
  "com.apple.driver.AppleMobileFileIntegrity",
  "com.apple.filesystems.apfs",
  # The merged iOS root on p55 is case-sensitive journaled HFS+ (the iOS 16 route of ios-sweet/negroni):
  # Apple's own HFS 627.100.6, which links only the base KPIs.
  "com.apple.filesystems.hfs.kext",
  "com.apple.iokit.IOStorageFamily",
  "com.apple.kec.corecrypto",
  # bsd_init panics "pthread kernel extension not loaded" in pth_proc_hashinit without it.
  # Upstream's source pthread (CONFIG_XNU_SOURCE_PTHREAD) is the Darwin 23 libpthread and
  # does not compile against xnu-8796, so the 16.7.16 kext itself is kept.
  "com.apple.kec.pthread",
]
```

`board.toml:101-111` explains the roots:
> ```
> # roots are the kernelcache components this port uses and does not replace with source:
> # the APFS root volume, its decmpfs provider, the storage stack that publishes IOMedia,
> # and corecrypto (the source corecrypto is linked in as well). The closure also carries
> # AppleEffaceableStorage, CoreAnalyticsFamily, IOCryptoAcceleratorFamily, AppleImage4 and
> # CoreTrust, which those declare as libraries; they have no matching node in the ADT and
> # stay unstarted. Userland components stay out until userland is in scope: the defconfig
> # is the storage bring-up.
> ```

### 10.7 Root device: `chosen/root-matching` on the IOMedia

`UniXNU-lumia950xl/scripts/make-lumia950-adt.py:331-338`
```python
    if root_match_volume_uuid is not None:
        xml = (
            "<dict><key>IOProviderClass</key><string>IOMedia</string>"
            "<key>IOPropertyMatch</key><dict><key>UUID</key><string>"
            + str(root_match_volume_uuid).upper()
            + "</string></dict></dict>\0"
        )
        chosen.append(("root-matching", xml.encode()))
```

CLI flag: `make-lumia950-adt.py:516`
```python
    parser.add_argument("--root-match-volume-uuid", type=uuid.UUID)
```

So the chain is:

> `IOPlatformExpertDevice` reads `/chosen/root-matching` → matches the `IOMedia` whose
> `UUID` property equals the HFS+ volume UUID → `IOService::findRootMedia()` →
> `disk0` → HFS+ mount.

This is the **IOKit-native** route. `ios-sweet` has no equivalent (it patches
`IOService::findRootMedia` to do nothing — see §11.3).

### 10.8 The ADT node the personality matches

`UniXNU-lumia950xl/scripts/make-lumia950-adt.py:380-388`
```python
    sdhci = node(
        "sdhci@f9824900",
        [
            ("compatible", string("qcom,msm8994-sdhci", "qcom,sdhci-msm-v4")),
            ("reg", b"".join(u64(address) + u64(size) for address, size in SDHCI_WINDOWS)),
            ("interrupts", b"".join(u32(0) + u32(irq) + u32(4) for irq in SDHCI_INTERRUPTS)),
            ("interrupt-parent", u32(1)),
            ("clock-frequency", u32(TIMEBASE_FREQUENCY)),
        ],
```

`make-lumia950-adt.py:58-62`
```python
TIMEBASE_FREQUENCY = 19_200_000
GIC_DISTRIBUTOR = (0xF9000000, 0x1000)
GIC_CPU_INTERFACE = (0xF9002000, 0x1000)
SDHCI_WINDOWS = ((0xF9824900, 0x1A0), (0xF9824000, 0x800))
SDHCI_INTERRUPTS = (123, 138)
```

**Both `reg` entries are mandatory** — `mapWindows()` fails without them
(`NativeSdhci.cpp:183-186`).

### 10.9 The build definition

`UniXNU-lumia950xl/configs/lumia950_defconfig:1-13`
```
# Kernel configuration of the lumia950 image. CONFIG_XNU_<NAME> becomes XNU_<NAME> for the
# kernel build.
#
# This is the storage-bring-up configuration: the platform expert, the CPU and interrupt
# path, and the eMMC driver. Everything that needs a working display, userland or power
# management is deliberately absent.

CONFIG_XNU_ARM_DT_PLATFORM=y
CONFIG_XNU_ARMCPU=y
# NativeSdhci subclasses IOBlockStorageDevice from the IOStorageFamily kext; scripts/
# build-native-sdhci16.py binds it to the 16.7.16 family (drivers/NativeSdhci/original-bindings-
# 16.7.16.S). Its personality is added at runtime by the zzv overlay patch (native_sdhci_publish).
CONFIG_XNU_NATIVE_SDHCI=y
```

---

## 11. The `ios-sweet` / `ios-v60` UFS contract (what our base project requires)

### 11.1 Important: `ios-v60`'s patches are byte-identical to `ios-sweet`'s

```
$ diff <(ls ios-sweet-lg-g4/xnu/patches) <(ls ios-v60/xnu/patches)   # no output
$ diff -q ios-sweet-lg-g4/xnu/patches/0028-sweet-ufs-root-device.patch \
          ios-v60/xnu/patches/0028-sweet-ufs-root-device.patch          # same
$ diff -q ios-sweet-lg-g4/xnu/patches/0030-sweet-ufs-writes.patch \
          ios-v60/xnu/patches/0030-sweet-ufs-writes.patch               # same
$ diff -q ios-sweet-lg-g4/xnu/patches/0034-sweet-ufs-dma-above-4g.patch \
          ios-v60/xnu/patches/0034-sweet-ufs-dma-above-4g.patch         # same
```

`ios-v60` adds only `xnu/patches/0069-sweet-ufs-faster.patch` (cache-clean DMA,
`ufscache=1` / `ufswait=1` boot args) on top. **The `ios-v60` XNU-side UFS integration
is the `ios-sweet` one, not a separate design.**

### 11.2 The contract, in three XNU files

**(a) `bsd/conf/files`** — `ios-sweet-lg-g4/xnu/patches/0028-sweet-ufs-root-device.patch:1-11`
```
diff --git a/bsd/conf/files b/bsd/conf/files
--- a/bsd/conf/files
+++ b/bsd/conf/files
@@ -104,6 +104,7 @@
 bsd/dev/random/randomdev.c		standard
 
 bsd/dev/memdev.c			standard
+bsd/dev/sweet_ufs.c			standard
 bsd/dev/mem.c				standard
 bsd/dev/munge.c 			optional config_requires_u32_munging
 bsd/dev/unix_startup.c			standard
```

**(b) `iokit/bsddev/IOKitBSDInit.cpp`** — the root hook, `0028:12-35`
```
diff --git a/iokit/bsddev/IOKitBSDInit.cpp b/iokit/bsddev/IOKitBSDInit.cpp
--- a/iokit/bsddev/IOKitBSDInit.cpp
+++ b/iokit/bsddev/IOKitBSDInit.cpp
@@ -766,6 +766,24 @@
 		}
 	}
 
+#if defined(ARM64_BOARD_CONFIG_SWEET)
+//
+//	SWEET: root on a GPT partition of the internal UFS (bsd/dev/sweet_ufs.c)
+//
+	if (strcmp(rdBootVar, "ufsroot0") == 0) {
+		extern int sweet_ufs_rootdev(dev_t *dev);
+		OSSafeReleaseNULL(matching);
+		int ufs_error = sweet_ufs_rootdev(root);
+		if (ufs_error) {
+			panic("IOFindBSDRoot: SWEET UFS root %s unavailable: %d", rdBootVar, ufs_error);
+		}
+		strlcpy(rootName, rdBootVar, rootNameSize);
+		IOLog("BSD root: %s, major %d, minor %d\n", rootName, major(*root), minor(*root));
+		*oflags = 0;
+		goto iofrootx;
+	}
+#endif
+
```

**(c) `bsd/dev/sweet_ufs.c`** — the driver, added wholesale by the same patch.
The parts that define the contract, `0028:501-580`:
```c
static int
sweet_ufs_init(void)
{
	DTEntry node;
	uintptr_t const *reg = NULL;
	unsigned int size = 0;
	char want[64] = "userdata";
	int error;

	if (SecureDTLookupEntry(NULL, "/sweet-ufs", &node) != kSuccess ||
	    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||
	    size < 2 * sizeof(uintptr_t)) {
		printf("SWEET UFS: no /sweet-ufs node in the device tree\n");
		return ENXIO;
	}
	PE_parse_boot_argn("ufsroot", want, sizeof(want));

	su.regs = ml_io_map(reg[0], reg[1]);
	...
	su.bmajor = bdevsw_add(-1, &sweet_ufs_bdevsw);
	if (su.bmajor < 0) {
		return ENXIO;
	}
	su.cmajor = cdevsw_add_with_bdev(-1, &sweet_ufs_cdevsw, su.bmajor);
	if (su.cmajor < 0) {
		return ENXIO;
	}
	su.bdev = makedev(su.bmajor, 0);
	devfs_make_node(su.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufsroot0");
	devfs_make_node(makedev(su.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufsroot0");
	su.ready = true;
	printf("SWEET UFS: \"%s\" on LUN %u: first block 0x%llx, %llu MiB, read-only\n",
	    want, su.lun, su.first_block, su.nblocks * UFS_BLOCK >> 20);
	return 0;
}

/* Root device for rd=ufsroot0 (IOFindBSDRoot). */
int sweet_ufs_rootdev(dev_t *dev);

int
sweet_ufs_rootdev(dev_t *dev)
{
	if (!su.ready) {
		int error = sweet_ufs_init();
		if (error) {
			return error;
		}
	}
	*dev = su.bdev;
	return 0;
}
```

And the `bdevsw`/`cdevsw` tables, `0028:474-499`:
```c
static const struct bdevsw sweet_ufs_bdevsw = {
	.d_open     = sweet_ufs_open,
	.d_close    = sweet_ufs_close,
	.d_strategy = sweet_ufs_strategy,
	.d_ioctl    = sweet_ufs_bioctl,
	.d_dump     = eno_dump,
	.d_psize    = sweet_ufs_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw sweet_ufs_cdevsw = {
	.d_open       = sweet_ufs_open,
	...
	.d_type       = D_DISK,
};
```

### 11.3 `ios-sweet` has **no IOMedia at all**

`ios-sweet-lg-g4/xnu/patches/0022-sweet-ramdisk-root-without-iomedia.patch:1-27` (entire file)
```
diff --git a/iokit/Kernel/IOService.cpp b/iokit/Kernel/IOService.cpp
--- a/iokit/Kernel/IOService.cpp
+++ b/iokit/Kernel/IOService.cpp
@@ -5126,7 +5126,9 @@
 	bool                wasHiding;
 
 	iomediaClass = OSMetaClass::getMetaClassWithName(gIOMediaKey);
+#if !defined(ARM64_BOARD_CONFIG_SWEET)
 	assert(iomediaClass);
+#endif
 
 	LOCKWRITENOTIFY();
 	wasHiding = gIOServiceHideIOMedia;
@@ -5135,6 +5137,13 @@
 	}
 	UNLOCKNOTIFY();
 
+#if defined(ARM64_BOARD_CONFIG_SWEET)
+	/* SWEET has no IOStorageFamily: without the IOMedia class no media exists to unhide. */
+	if (!iomediaClass) {
+		wasHiding = false;
+	}
+#endif
+
 	FindRootMediaContext ctx = { .services = NULL, .parent = parent };
 
 	if (wasHiding) {
```

### 11.4 The remaining storage patches in `ios-sweet`

| Patch | XNU files | What it does |
| --- | --- | --- |
| `0028-sweet-ufs-root-device.patch` | `bsd/conf/files`, `iokit/bsddev/IOKitBSDInit.cpp`, `bsd/dev/sweet_ufs.c` (new, 536 lines) | the UFS driver + `rd=ufsroot0` root hook |
| `0029-sweet-unsealed-root-volume.patch` | `bsd/kern/bsd_init.c` | replaces the `panic("rootvp not authenticated after mounting")` with a warning |
| `0030-sweet-ufs-writes.patch` | `bsd/dev/sweet_ufs.c` | adds the write path (see below) |
| `0031-sweet-read-write-root.patch` | `bsd/vfs/vfs_subr.c`, `bsd/sweet_hfs/core/hfs_vfsutils.c` | `rootrw=1` boot arg clears `MNT_RDONLY`; allows a dirty non-journaled root to mount rw |
| `0034-sweet-ufs-dma-above-4g.patch` | `bsd/dev/sweet_ufs.c` | rejects DMA above 4 GiB when the host lacks 64-bit addressing |
| `0022-sweet-ramdisk-root-without-iomedia.patch` | `iokit/Kernel/IOService.cpp` | tolerate a missing `IOMedia` class |
| `0069-sweet-ufs-faster.patch` (**v60 only**) | `bsd/dev/sweet_ufs.c` | `ufscache=1` clean-only DMA, `ufswait=1` command wait |

`0034` verbatim, `ios-sweet-lg-g4/xnu/patches/0034-sweet-ufs-dma-above-4g.patch:19-31`
```
@@ -305,6 +307,12 @@
 		printf("SWEET UFS: requests outstanding from the bootloader\n");
 		return EBUSY;
 	}
+	/* With RAM above 4 GiB (mkadt.py --highmem) the DMA memory can be there too. */
+	if (((su.dma_pa + DMA_SIZE - 1) >> 32) && !(ufs_rd(REG_CAP) & CAP_64AS)) {
+		printf("SWEET UFS: DMA memory at 0x%llx but the host has no 64-bit addressing\n",
+		    su.dma_pa);
+		return ENXIO;
+	}
 	ufs_wr(REG_IE, 0);                      /* polled operation */
```

### 11.5 The ADT node the UFS driver reads

`ios-sweet-lg-g4/tools/mkadt.py:200-205`
```python
    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])
```

### 11.6 The shim-stage UFS driver (bootloader, not the kernel)

`ios-sweet-lg-g4/shim/ufs.c:16`
```c
#define UFS_BASE	0x01d84000UL	/* ufshc@1d84000, sweet live FDT */
```

`ios-sweet-lg-g4/shim/ufs.c:195-197`
```c
void test_ufs(void)
{
	con_str("UFS", "milestone 5 probe (read-only)");
```

This is a **pre-OS diagnostic**: it lists GPT partitions and reads the MBR/GPT while the
MMU is still off (`shim/shim.c:386-388`):
```c
	/* No payload: probe the internal UFS while the MMU is still off. */
	stage = "ufs";
	test_ufs();
```

It is **not** the XNU driver. The XNU driver is `bsd/dev/sweet_ufs.c`, created by patch
0028.

### 11.7 Side-by-side contract comparison

| Concern | `UniXNU` (Lumia, eMMC) | `ios-sweet` (Sweet, UFS) |
| --- | --- | --- |
| Driver home | `drivers/NativeSdhci/` (in-kernel IOKit) | `bsd/dev/sweet_ufs.c` (BSD block device) |
| Base class | `IOBlockStorageDevice` | `struct bdevsw` |
| Device name | `disk0` (via IOMedia) | `ufsroot0` / `rufsroot0` (devfs) |
| Root selection | `/chosen/root-matching` → IOMedia UUID | `rd=ufsroot0` → `IOFindBSDRoot` hook |
| Needs IOStorageFamily | **yes** (prelinked kext) | **no** (patched out) |
| ADT node | `sdhci@f9824900`, `compatible = qcom,msm8994-sdhci` | `/sweet-ufs`, `compatible = qcom,ufshc` |
| Personality | runtime, `native_sdhci_publish()` | none (BSD, not IOKit) |
| Extra XNU link step | `-Wl,-force_load` + original-bindings `.S` | none (built into the kernel from `bsd/conf/files`) |
| Partition name | `"ios"` | `"userdata"` (boot-arg `ufsroot=`) |
| Write enable | boot-arg `sdhcirw=1` | boot-arg `ufsrw=1` |

---

## 12. Patch inventory

### 12.1 `UniXNU-lumia950xl/patches-parked/` — one file

`ls -la patches-parked/`:
```
-rw-r--r--@ 1 io staff  572 Nov  4 23:54 native-sdhci-personality.patch
```

| File | Summary |
| --- | --- |
| `patches-parked/native-sdhci-personality.patch` | Adds a `NativeSdhci` personality to `iokit/KernelConfigTables.cpp` matched to `qcom,msm8994-sdhci`, guarded by `XNU_NATIVE_SDHCI`. **Parked — not applied** (context collides with other patches; superseded by the runtime publish in `zzv-aes-shim.patch`). |

### 12.2 `UniXNU-lumia950xl/unixnu-overlay/patches/xnu-8796/` — nine patches

| File | XNU file(s) | Summary |
| --- | --- | --- |
| `verbose-text-console.patch` | `pexpert/arm/pe_init.c` | verbose early console |
| `zzq-native-diagnostics.patch` | `bsd/dev/mem.c`, `iokit/Kernel/IOUserClient.cpp`, `osfmk/kern/debug.c` | native diagnostics hooks |
| `zzr-console-mute.patch` | `osfmk/console/video_console.c` | mute the video console |
| `zzs-crash-log.patch` | `bsd/kern/kern_exit.c` | crash log on user-process exit |
| `zzt-percpu-single-cpu.patch` | `osfmk/arm/cpu_common.c` | single CPU |
| `zzu-unsealed-rw-root.patch` | `bsd/vfs/kpi_vfs.c`, `bsd/vfs/vfs_subr.c` | unsealed / read-write root (the `ios-sweet` `0029`+`0031` equivalents) |
| `zzv-aes-shim.patch` | `iokit/Kernel/IOStartIOKit.cpp` | **publishes the `NativeSdhci` personality at runtime** (`native_sdhci_publish()`); also the `IOAESAccelerator` personality |
| `zzw-procmon.patch` | `osfmk/kern/startup.c` | **calls `native_sdhci_publish()`** before `bsd_init()`; procmon thread, crash reporter, sandbox/MAC registration |
| `build-xnu-4k-baseline-native-sdhci.patch` (one level up) | `scripts/build-xnu-4k-baseline.sh` | **links `libNativeSdhci.a` into the kernel** and adds `-DXNU_NATIVE_SDHCI=1` |

### 12.3 `UniXNU-lumia950xl/ci/` — nine files

| File | Summary |
| --- | --- |
| `unixnu-16x.patch` | the base repo's patch set, re-targeted to the 16.x line (large; not storage-specific) |
| `hackios-gic-v2.patch` | native GICv2 support |
| `hackios-kernel-setup.patch` | kernel build setup |
| `hackios-userland-build.patch` | userland build |
| `darwinboot-handoff-label.patch` | DarwinBoot handoff label |
| `darwinboot-payload-ramdisk.patch` | DarwinBoot payload ramdisk |
| `darwinboot-payload-volume.patch` | DarwinBoot payload volume |
| `darwinboot-trustcache-v1.patch` | DarwinBoot trust cache v1 |
| `apply-hackios-patches.sh` | applies the above |

---

## 13. Build system

### 13.1 The driver is compiled by a generic archive builder

`UniXNU-lumia950xl/scripts/build-arm-driver.py:1-11`
```python
"""
Same recipe as the base tree's build-arm-driver.py: replay the compile command
XNU recorded for IOPlatformExpert, swap in the driver source, and check that the
object really is an ARM64 Mach-O object. The differences are the component list
(which also covers this device's drivers) and the archive step.

Run it from the build tree; `unixnu build kernel` does that for every entry of
[kernel] archives in board.toml.
"""
```

`scripts/build-arm-driver.py:22-40`
```python
# component -> sources, output directory, extra include directories
COMPONENTS = {
    "ArmDTPlatform": {
        "sources": ["drivers/ArmDTPlatform/ArmDTPlatform.cpp"],
        "out": "armdt-platform",
    },
    "ArmCpu": {
        "sources": ["drivers/ArmCpu/ArmCpu.cpp", "drivers/NativeGicV2/GicV2Core.cpp"],
        "out": "armcpu",
        "include": ["drivers/NativeGicV2"],
    },
    "NativeSdhci": {
        "sources": ["drivers/NativeSdhci/NativeSdhci.cpp", "drivers/NativeSdhci/SdhciCore.cpp",
                    "drivers/NativeSdhci/PollingHost.cpp"],
        "out": "native-sdhci",
        # The storage ABI headers are prepared from IOStorageFamily by the kernel step;
        # osfmk is where the recorded recipe resolves <mach/...> from.
        "include": ["drivers/NativeSdhci", "build/storage-abi/include", ".cache/xnu-work/osfmk"],
    },
```

The compile command is **replayed from XNU's own recorded compile record**
(`build-arm-driver.py:165-189`, `build_command()`), not hand-written. The archive step:

`build-arm-driver.py:225-230`
```python
    if shutil.which("ar") is None:
        raise SystemExit("ar is not on PATH")
    archive = output / f"lib{args.component}.a"
    subprocess.run(["ar", "rcs", str(archive)] + [str(o) for o in objects], check=True)
    print(f"{args.component}: {archive}")
    print(f"  link with  -Wl,-force_load,{archive}")
```

**Compiler / SDK / arch:** the `xcrun clang -target arm64-apple-ios16.0` triple appears
explicitly for the bindings file (`scripts/build-native-sdhci16.py:27-28`), and the
recorded XNU recipe resolves the SDK. The README states the requirement:

`UniXNU-lumia950xl/README_BEFORESTARTING!!!!!!!!!!!!!!.md:5-7`
> ```
> `scripts/build-arm-driver.py` replays the compile command XNU recorded and needs Xcode and
> the iPhoneOS SDK. This Mac has Command Line Tools but no verified iPhoneOS SDK. The
> standalone EFI diagnostics and freestanding backend cross-compiles are separate. So:
> ```

### 13.2 The archive is force-loaded into the kernel

`configs/lumia950_defconfig:37` (the `XNU_EXTRA_LD_ARCHIVES` line, abridged):
```
CONFIG_XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,{tree}/build/native-legacy-display/libNativeLegacyDisplay.a -Wl,-force_load,{tree}/build/native-keystore/libNativeKeyStoreEvents.a ... "
```
(`NativeSdhci` is *not* in that list — it goes in through the `KERNEL_EXTRA_ARCHIVES` hook
of `build-xnu-4k-baseline.sh`, §10.3.)

`configs/lumia950_defconfig:27-29`
```
# Run the archive builders of [kernel] archives. NativeSdhci is compiled by
# scripts/build-arm-driver.py against the compile records of the XNU pass.
CONFIG_XNU_BUILD_ARCHIVES=y
```

### 13.3 The host-side unit tests

`UniXNU-lumia950xl/scripts/test-storage.sh:1-14` (entire file)
```bash
#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:-$ROOT/build/storage-tests}
CXX=${CXX:-c++}
mkdir -p "$OUT"
for test_name in check-sdhci-core check-polling-host; do
  "$CXX" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$ROOT/drivers/NativeSdhci" \
    "$ROOT/drivers/NativeSdhci/SdhciCore.cpp" \
    "$ROOT/drivers/NativeSdhci/PollingHost.cpp" \
    "$ROOT/drivers/NativeSdhci/Test/$test_name.cpp" -o "$OUT/$test_name"
  "$OUT/$test_name"
done
```

This is the key structural property to copy: **`SdhciCore.cpp` and `PollingHost.cpp` are
freestanding** (no IOKit), so the whole MMC/SDHCI state machine is unit-testable on a
host with simulated MMIO. Only `NativeSdhci.cpp` touches IOKit.

`docs/storage-bringup.md:28-41`
> ```
> ## Validation
>
> Run `bash scripts/test-storage.sh`. Both executables use AddressSanitizer and
> UndefinedBehaviorSanitizer. The transport test models W1C status, separately
> enabled events, command/data delays, R2 encoding, standby/transfer state, wrong
> RCA, stuck reset, controller and card errors, partial FIFO failure, missing
> completion, destination guards, capacity limits, and unsupported EXT_CSD modes.
> The model never performs MMIO or reads the phone.
>
> The portable sources also compile freestanding for AArch64. Separately,
> `NativeSdhci.cpp`, `SdhciCore.cpp` and `PollingHost.cpp` compiled with the recorded
> XNU/iPhoneOS SDK recipe and linked in
> [run 40379567698](https://github.com/TwisstedToast/UniXNU-lumia950/actions/runs/40379567698),
> source `c76c869`. This qualifies the build, not an on-phone read or an iOS boot.
> ```

### 13.4 CI

`README_BEFORESTARTING!!!!!!!!!!!!!!.md:35-41`
> ```
> | Job | Runs when | What it is the only proof of | Artifact |
> | --- | --- | --- | --- |
> | `validate` | every push to `main`, every PR, **and every dispatch** | board/defconfig parsing, storage/GIC/GPU/display/probe core tests, addon and ADT checks, board acceptance and the incomplete-board guard; not IOKit compilation | none |
> ...
> | `kernel` | dispatch with `target=kernel` or `both` | **every driver compiles against the recorded XNU ABI with `-Werror`**, the kernel links, and the pinned 16.7.16 kernelcache merges | `lumia950-kernel` |
> ```

And the trap that matters most for a new driver, `README_BEFORESTARTING!!!!!!!!!!!!!!.md:55-58`
> ```
> 1. **A new driver is invisible to CI until it is in `[kernel] archives` in `board.toml`.**
>    `drivers/Foo/` existing is not enough. Add `"build-arm-driver.py Foo"` and the matching
>    `COMPONENTS` entry in `scripts/build-arm-driver.py`, or the job happily compiles everything
>    except your new file and goes green.
> ```

---

## 14. Board configuration (`board.toml`)

### 14.1 Format

`board.toml` is **TOML**, read by the `unixnu` build driver. Its sections, as used by the
Lumia:

| Section | Purpose | Example key |
| --- | --- | --- |
| `[board]` | identity | `name`, `soc`, `description`, `kit` |
| `[kernel]` | kernel build | `defconfig`, `archives` (list of builder scripts) |
| `[ipsw]` | the Apple reference image | `url`, `product`, `version`, `build`, `device_class`, `chip_id`, `board_id`, `fetch` |
| `[kernel.prelink]` | merging into the Apple kernelcache | `tool`, `kernel`, `kernelcache_sha256`, `slide`, `early_security_init`, `roots` |
| `[userdata]` | the root image geometry | `first_lba`, `last_lba`, `lba_bytes`, `windows` |
| `[payload]` | the bootable payload | `title`, `kernel_args`, `dram_size`, `dram_base` |
| `[payload.adt]` | the Apple DeviceTree | `tool`, `cpu_regs`, `psci_method`, `owned_memory_size`, `chip_id`, `iphone_identity` |

The header comment, `board.toml:1-7`
```toml
# Board description of the Microsoft Lumia 950 (MSM8992, Snapdragon 808), read by the unixnu
# build driver.
#
# P1 research skeleton. The incomplete-board guard is intentional: this board cannot build a
# boot image yet, and it must not pretend to. Unknown sections are absent rather than filled
# with zero addresses or invented hashes. Remove `example = true` only when every section
# below exists and has been checked on hardware.
```

### 14.2 The `[kernel] archives` list — where a new driver is registered

`board.toml:16-61` (excerpt)
```toml
[kernel]
defconfig = "lumia950_defconfig"
# Archive builders of the kernel step, in order. Names are files of the build tree's
# scripts/ directory, so they carry no path. The corecrypto pair comes first because
# ArmDTPlatform is the platform expert that consumes the ADT; ArmCpu owns the CPU, timer and interrupt path;
# NativeSdhci is this device's eMMC driver (see drivers/NativeSdhci and README.md).
archives = [
  "build-corecrypto-14.py",
  "build-corecrypto-14.py --namespace",
  "build-arm-driver.py ArmDTPlatform",
  "build-arm-driver.py ArmCpu",
  # NativeSdhci plus its IOStorageFamily 16.7.16 bindings (it subclasses IOBlockStorageDevice).
  "build-native-sdhci16.py",
  ...
]
```

### 14.3 The `[userdata]` section — partition geometry

`board.toml:138-149`
```toml
# The root image: `unixnu build userdata` writes the IPSW's unchanged OS and Cryptex1,SystemOS disk
# images behind a SHNGROOT manifest. On this phone it does not go into Android's userdata but into p55
# "ios" (type AF00), carved out of userdata on 2026-10-30; the eMMC has 512-byte LBAs. NativeSdhci
# finds the partition by that name and publishes the windows.
[userdata]
first_lba = 25100288
last_lba = 61069311
lba_bytes = 512
windows = [
  { name = "OS", component = "OS" },
  { name = "SystemOS", component = "Cryptex1,SystemOS" },
]
```

### 14.4 What a NEW board must declare

From the sections above, a new board (LG G4 / MSM8992) must supply:

1. `[board]` — `name` (e.g. `lg-g4`), `soc` (`MSM8992`), `description`, `kit`.
2. `[kernel] defconfig` — a new `configs/<board>_defconfig` with at least
   `CONFIG_XNU_ARM_DT_PLATFORM=y`, `CONFIG_XNU_ARMCPU=y`, `CONFIG_XNU_BUILD_ARCHIVES=y`.
3. `[kernel] archives` — the driver-builder list, including the eMMC builder.
4. `[ipsw]` — the Apple reference image identity (`product`, `version`, `build`,
   `device_class`, `chip_id`, `board_id`, `fetch`).
5. `[kernel.prelink]` — `kernelcache_sha256`, `slide`, `roots` (**including
   `com.apple.iokit.IOStorageFamily`** if the IOKit route is used), `early_security_init`.
6. `[userdata]` — `first_lba`, `last_lba`, `lba_bytes`, `windows`.
7. `[payload]` — `kernel_args`, `dram_size`, `dram_base`.
8. `[payload.adt]` — `tool` (the ADT generator), `cpu_regs`, `psci_method`,
   `owned_memory_size`, `chip_id`.

---

## 15. Device tree handling

### 15.1 The UniXNU board uses an **Apple DeviceTree (ADT)**, not a Linux DTB

The ADT is generated by `scripts/make-lumia950-adt.py` and emitted in Apple's flattened
encoding. `board.toml:175-180`
```toml
[payload.adt]
# The Apple flattened encoding with this SoC's addresses, a /chosen NVRAM proxy and the
# memory-map placeholders DarwinBoot fills in (`/chosen/memory-map/{RAMDisk,PackedRAMDisk,
# TrustCache}`). The validate job runs the generator and the checker, including the checker's
# defect self-test.
tool = "scripts/make-lumia950-adt.py"
```

Output path (`make-lumia950-adt.py:504`):
```python
    parser.add_argument("--output", type=Path, default=ROOT / "build/lumia950.adt")
```

The platform expert that consumes it is **`ArmDTPlatform`**
(`board.toml:20-21`: *"ArmDTPlatform is the platform expert that consumes the ADT"*).

### 15.2 The `ios-sweet` base uses the same ADT format, and also carries a Linux DTB

`ios-sweet-lg-g4/tools/mkadt.py:1-10` uses the same `adt` module:
```python
import adt  # noqa: E402
```

`ios-sweet-lg-g4/tools/mkadt.py:244-249`
```python
if __name__ == '__main__':
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
```

Separately, the **shim** receives a Linux-style FDT at boot:

`ios-sweet-lg-g4/shim/shim.c:373-382`
```c
void shim_main(uint64_t dtb)
{
	struct mmu_info mi;

	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage = "loader";
	con_clear(RED);
	con_str("SHIM", "sweet milestone 4: xnu loader");
	con_skip();
	const char *why = loader_boot(dtb);
```

and `shim/shim.c:410`
```c
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
```

So in `ios-sweet`: the **shim** parses the Android/Linux FDT (`shim/fdt.c`), and the
**kernel** consumes an **Apple ADT** generated by `tools/mkadt.py`. Two different trees
for two different stages. The eMMC driver's ADT node must be in the **ADT**, not the DTB.

### 15.3 Early boot

`UniXNU-lumia950xl/scripts/make-lumia950-adt.py:309-330` (the `/chosen` node)
```python
    ...
    chosen.extend(empty_nvram_proxy())
    if owned_memory_size is not None:
        chosen.append(("darwinboot-memory-size", u64(owned_memory_size)))
```

The `/chosen` node carries `dram-base`, `dram-size`, `random-seed`, `debug-enabled`,
`embedded-panic-log-size`, `amfi-only-platform-code`, `security-mode-change-enable`,
`darwinboot-memory-size` and optionally `root-matching`.

---

## 16. LG G4 (H815) hardware facts — from the stock device tree

Source: **`/Users/io/Dev/ios/lg g4/msm8992-lg-h815-boot.dts`** (LG's own boot device tree
for the H815).

### 16.1 The eMMC controller is `sdhc1` at the same address as the Lumia

`msm8992-lg-h815-boot.dts:6392-6424`
```
		sdhci@f9824900 {
			compatible = "qcom,sdhci-msm";
			reg = <0xf9824900 0x1a0 0xf9824000 0x800>;
			reg-names = "hc_mem", "core_mem";
			interrupts = <0x00 0x7b 0x00 0x00 0x8a 0x00>;
			interrupt-names = "hc_irq", "pwr_irq";
			qcom,bus-width = <0x08>;
			qcom,cpu-dma-latency-us = <0x12d 0x46>;
			qcom,cpu-affinity = "affine_cores";
			qcom,cpu-affinity-mask = <0x0f 0xf0>;
			qcom,wakeup-on-idle;
			qcom,msm-bus,name = "sdhc1";
			...
			qcom,clk-rates = <0x61a80 0x1312d00 0x17d7840 0x2faf080 0x5f5e100 0xa408300 0x14810600>;
			qcom,bus-speed-mode = "HS400_1p8v", "HS200_1p8v", "DDR_1p8v";
			qcom,nonremovable;
		};
```

`msm8992-lg-h815-boot.dts:13268-13272`
```
	aliases {
		spi0 = "/soc/spi@f9923000";
		sdhc1 = "/soc/sdhci@f9824900";
		sdhc2 = "/soc/sdhci@f98a4900";
		sdhc3 = "/soc/sdhci@f9864900";
```

### 16.2 Comparison table

| Property | Lumia 950 XL (`BoardConstants.h` / ADT) | LG G4 H815 (`msm8992-lg-h815-boot.dts`) | Same? |
| --- | --- | --- | --- |
| Controller base | `0xf9824900` | `0xf9824900` | **yes** |
| `hc` size | `0x1a0` | `0x1a0` | **yes** |
| `core` window | `0xf9824000`, size `0x800` | `0xf9824000`, size `0x800` | **yes** |
| hc IRQ | `123` | `0x7b` = 123 | **yes** |
| pwr IRQ | `138` | `0x8a` = 138 | **yes** |
| Bus width | 8 | `qcom,bus-width = <0x08>` | **yes** |
| Non-removable | yes | `qcom,nonremovable` | **yes** |
| `compatible` string | `"qcom,msm8994-sdhci", "qcom,sdhci-msm-v4"` | `"qcom,sdhci-msm"` | **NO** |
| Max bus mode | (n/a — not yet qualified) | `HS400_1p8v`, `HS200_1p8v`, `DDR_1p8v` | — |
| SD card slot | — | `sdhci@f98a4900` (`sdhc2`, 4-bit, removable) | — |

The `compatible` difference matters **only for the personality match string**: the
personality's `IONameMatch` must equal a string the ADT node actually carries. Since
`UniXNU` generates its own ADT, the simplest port is to **keep** `"qcom,msm8994-sdhci"` in
the G4's ADT generator so the existing personality string matches unchanged.

### 16.3 The G4's other SDHCI controllers — do not match them

`msm8992-lg-h815-boot.dts:6426-6428` (`sdhc2`, the microSD slot)
```
		sdhci@f98a4900 {
			compatible = "qcom,sdhci-msm";
			reg = <0xf98a4900 0x11c 0xf98a4000 0x800>;
```

`msm8992-lg-h815-boot.dts:6466-6468` (`sdhc3`)
```
		sdhci@f9864900 {
			compatible = "qcom,sdhci-msm";
			reg = <0xf9864900 0x11c 0xf9864000 0x800>;
```

`compatible = "qcom,sdhci-msm"` on **all three** means a personality keyed on that
string would match all three controllers. The `"qcom,msm8994-sdhci"` string of the Lumia
ADT is specific to `sdhc1` and is the reason the UniXNU approach works.

---

## 17. ADAPTATION CHECKLIST FOR MSM8992/eMMC (LG G4)

Target repo: `/Users/io/Dev/ios/ios-sweet-lg-g4`.
Source of truth for every "copy from" below:
`/Users/io/Dev/ios/lg g4/UniXNU-lumia950xl`.

### 17.0 First, decide the integration shape — this determines everything else

There are two viable shapes. **Do not mix them.**

| | **Option A — BSD bdevsw (RECOMMENDED)** | Option B — IOKit IOBlockStorageDevice |
| --- | --- | --- |
| Model | `bsd/dev/sweet_emmc.c`, a `struct bdevsw` disk | `drivers/NativeSdhci`-style `IOBlockStorageDevice` |
| Root | `rd=emmcroot0` + `IOKitBSDInit.cpp` hook | `/chosen/root-matching` → IOMedia UUID |
| Needs `IOStorageFamily` | no | **yes** (prelinked kext, absolute addresses) |
| Needs personality + force_load | no | yes (runtime publish + `-Wl,-force_load`) |
| Needs `original-bindings-*.S` | no | yes (kernelcache-version-specific) |
| Works with sweet's built-in HFS | **yes** | no (needs the prelinked HFS kext) |
| Porting effort | small | large |

**Choose Option A.** `ios-sweet` has no `IOStorageFamily` and no `IOMedia` — it patches
them *out* (`xnu/patches/0022-sweet-ramdisk-root-without-iomedia.patch:18-23`) and builds
HFS **into** the kernel (`xnu/patches/0026-sweet-builtin-hfs.patch`). Option B would
require re-introducing the whole Apple storage stack and binding it to a specific
kernelcache's absolute addresses. The **MMC/SDHCI protocol layer is reusable in both
cases** — that is where the UniXNU work actually is.

### 17.1 Phase 0 — hardware facts to establish before writing code

| # | Fact to establish | Why it matters | Evidence it must come from |
| --- | --- | --- | --- |
| 0.1 | The G4 eMMC's EXT_CSD `SEC_COUNT`, `sectorBytes`, revision, **`PARTITION_CONFIG`**, **`CMDQ_SUPPORT`** | The UniXNC qualifier **refuses** the card if any of these are wrong (`PollingHost.cpp:422-425`): `partitionAccess` → `WrongPartition`; `commandQueueEnabled` → `CommandQueue`; `revision < 2` / `sectorBytes != 512` / `capacityBytes <= 2 GiB` → `UnsupportedCard`. A **command-queue-capable** G4 eMMC would be refused outright. | read EXT_CSD on the handset |
| 0.2 | **GPT or MBR?** | `findGptPartition()` and `kGptSignature = "EFI PART"` (`BoardConstants.h:87-90`) assume GPT. The LG G4 ships an AOSP-layout eMMC (`device_lge_g4-common/BoardConfigCommon.mk:111-115`, fixed-size image partitions), which is **not** GPT. Either add an MBR parser or repartition. | `dd` LBA 0 on the handset |
| 0.3 | Which partition holds the iOS root, and its name | `kWritablePartition[] = "ios"` (`BoardConstants.h:77`) is the name the driver looks for; `ios-sweet` uses `"userdata"` (`0028-...patch:507`). They differ. | GPT/MBR dump |
| 0.4 | Does the firmware leave the card identified (warm) or not? | The Lumia needed the **cold** path (`BoardConstants.h:65-67`, `NativeSdhci.cpp:714-715`). The G4's bootloader is a different implementation. | console log |
| 0.5 | The eMMC's RCA after boot | `kRcaCandidates[] = { 1, 2 }` (`BoardConstants.h:85`) is a guess; a third value may be needed. | console log |

### 17.2 Phase 1 — port the freestanding MMC core (no IOKit involved)

These four files are **freestanding** — they include only `SdhciRegs.h` / `SdhciTypes.h`
and use fixed-width typedefs, so they compile in a BSD context unchanged in spirit. This
is the bulk of the value.

1. **Copy** `UniXNU-lumia950xl/drivers/NativeSdhci/SdhciCore.cpp` and
   `UniXNU-lumia950xl/drivers/NativeSdhci/SdhciCore.h`
   → new XNU files `bsd/dev/sweet_emmc_core.c` / `bsd/dev/sweet_emmc_core.h`
   (add them in the new patch, §17.4).
   * Contents: `decodeCsd()` (`SdhciCore.h:26`), `normalizeResponse136()` (`:28`),
     `decodeExtCsd()` (`:41`), `crc32()` (`:72`), `GptRange` (`:75-78`),
     `findGptPartition()` (`:84-85`).
   * Replace the `sdhci::u8/u16/u32/u64` typedefs from `SdhciTypes.h` with XNU's
     `<machine/_types.h>` (`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`).
   * `SdhciTypes.h:14-24` is the typedef block to drop; the `static_assert`s are the
     only other content.
2. **Copy** `UniXNU-lumia950xl/drivers/NativeSdhci/PollingHost.cpp` and
   `UniXNU-lumia950xl/drivers/NativeSdhci/PollingHost.h`
   → `bsd/dev/sweet_emmc_host.c` / `bsd/dev/sweet_emmc_host.h`.
   * This is the SDHCI command engine (`PollingHost`) and the MMC card state machine
     (`WarmCard`). Keep the `RegisterIo` **function-pointer injection seam**
     (`PollingHost.cpp:704-706` in the IOKit wrapper shows the pattern) so the whole
     thing stays host-testable.
   * Keep `kMaxTransferBlocks = 128` / `kMaxDmaBlocks = 1024`
     (`PollingHost.h:28,31`).
3. **Copy** `UniXNU-lumia950xl/drivers/NativeSdhci/SdhciRegs.h`
   → `bsd/dev/sweet_emmc_regs.h`. All offsets are SoC-generic SDHCI 3.0 and need **no**
   change for the G4 (the G4 uses the same 0x1a0-byte `hc` window — §16.2).
   * Merge `SdhciTypes.h` away; use XNU types.
4. **Create** `bsd/dev/sweet_emmc_board.h` from
   `UniXNU-lumia950xl/drivers/NativeSdhci/BoardConstants.h`.
   * **Keep unchanged** (identical on the G4, §16.2): `kSdhciHcPhys = 0xf9824900`,
     `kSdhciHcSize = 0x1a0`, `kSdhciCorePhys = 0xf9824000`, `kSdhciCoreSize = 0x800`,
     `kIrqHc = 123`, `kIrqPower = 138`, `kXoHz = 19200000`,
     `kLogicalBlockBytes = 512`, `kGccPhys = 0xfc400000`.
   * **Change**: `namespace lumia950` → `namespace lg_g4`;
     `kWritablePartition[] = "ios"` → the G4's partition name (0.3);
     `kRcaCandidates[] = { 1, 2 }` → the G4's actual RCA set (0.5);
     `kTransferHz = 25000000` → keep initially (legacy timing is what the driver
     qualifies; the G4's stock is HS400 — see 17.6).
   * The `GCC` clock constants (`BoardConstants.h:59-63`) are only used by the
     `identifyCold()` path of the IOKit wrapper; a BSD driver that never reprograms the
     clock can drop them, or keep them if you want the cold path.
5. **Do NOT port** `NativeSdhci.cpp`, `NativeSdhci.h`, `original-bindings-16.7.16.S`
   or `Info.plist` in Option A. They are the IOKit wrapper, the kernelcache-specific
   bindings and the kext personality — all three are Option-B-only.

### 17.3 Phase 2 — write the BSD eMMC driver

6. **New file** `bsd/dev/sweet_emmc.c`, modelled line-for-line on
   `bsd/dev/sweet_ufs.c` (the file patch `0028-sweet-ufs-root-device.patch:40-580`
   adds). Required elements:
   * `su` state struct: `regs`, `dma`, `dma_pa`, `rca`, `first_block`, `nblocks`,
     `secsize`, `bmajor`, `cmajor`, `bdev`, `ready` — copy the shape from
     `0028-...patch:125-136`.
   * `static inline uint32_t ufs_rd(uint32_t off)` → `emmc_rd`, reading
     `*(volatile uint32_t *)(su.regs + off)` (`0028-...patch:141-151`).
     **Important:** SDHCI mixes 8/16/32-bit registers; the IOKit wrapper uses
     *explicit-width* accessors (`NativeSdhci.h:42-44`: *"SDHCI mixes 8-, 16- and
     32-bit registers, so the width is explicit rather than derived from a 32-bit
     read"*). Port `readHc8/readHc16/readHc32/writeHc8/writeHc16/writeHc32` faithfully;
     do **not** use `readl`-style 32-bit access for `kRegCommand` (0x0E),
     `kRegTransferMode` (0x0C), `kRegBlockSize` (0x04), `kRegBlockCount` (0x06),
     `kRegHostControl1` (0x28), `kRegPowerControl` (0x29), `kRegSoftwareReset` (0x2F).
   * `bdevsw` / `cdevsw` tables — copy `0028-...patch:474-499`.
   * `sweet_emmc_open/close/strategy/ioctl/bioctl/cioctl/psize/read/write`.
   * `DKIOCGETBLOCKSIZE`, `DKIOCSETBLOCKSIZE`, `DKIOCGETPHYSICALBLOCKSIZE`,
     `DKIOCGETBLOCKCOUNT`, `DKIOCISWRITABLE`, `DKIOCGETMAXBLOCKCOUNTREAD/WRITE`,
     `DKIOCGETMAXSEGMENTCOUNTREAD/WRITE` — copy `0028-...patch:400-442`.
   * `emmc_find_partition()` — replace the UFS GPT parser
     (`0028-...patch:252-304`) with `findGptPartition()` from
     `SdhciCore.h:84-85`, **or** write an MBR parser if 0.2 says MBR.
   * `emmc_init()` — `SecureDTLookupEntry(NULL, "/sweet-emmc", &node)` +
     `SecureDTGetProperty(node, "reg", ...)` + `ml_io_map(reg[0], reg[1])`
     (copy `0028-...patch:501-523`), then the card identify (CMD0/CMD1/CMD2/CMD3/CMD7),
     then `bdevsw_add` / `cdevsw_add_with_bdev` / `devfs_make_node`.
   * `sweet_emmc_rootdev(dev_t *dev)` — copy `0028-...patch:565-579`.
   * **Device name:** use `"emmcroot0"` / `"remmcroot0"` (the UFS ones are
     `"ufsroot0"` / `"rufsroot0"`, `0028-...patch:557-558`).
   * **Partition name boot-arg:** `PE_parse_boot_argn("emmcroot", want, sizeof(want))`,
     defaulting to the G4's partition name (0.3) — copy `0028-...patch:507,516`.
7. **Cache maintenance:** the G4's eMMC host is **not cache coherent**. The UFS driver
   handles this with `flush_dcache64()` around every transfer
   (`0028-...patch:153-157`, `200-204`). The IOKit driver uses
   `dc civac` by hand (`NativeSdhci.cpp:354-363`). **Whichever you use, the eMMC path
   needs it** — the UniXNU measurement was ~27 MB/s ADMA2 vs ~10 MB/s PIO *with* the
   manual cache maintenance (`docs/lumia950xl/HARDWARE.md:75-76`).

### 17.4 Phase 3 — the XNU patch

8. **New patch** `ios-sweet-lg-g4/xnu/patches/0082-sweet-emmc-root-device.patch`
   (the next free number: `ls xnu/patches/*.patch | tail` → `0081-sweet-usb-link-state.patch`,
   81 patches; `tools/validate-repo.py:7-9` requires *"Sequential numbering starting at
   0001 with no duplicates or gaps"*). It must contain three parts:
   * `diff --git a/bsd/conf/files b/bsd/conf/files` — add
     `+bsd/dev/sweet_emmc.c			standard` (mirror `0028-...patch:8`).
   * `diff --git a/iokit/bsddev/IOKitBSDInit.cpp b/iokit/bsddev/IOKitBSDInit.cpp` —
     the `rd=emmcroot0` hook, mirroring `0028-...patch:19-35` exactly, including the
     `panic("IOFindBSDRoot: SWEET eMMC root %s unavailable: %d", ...)` and the
     `goto iofrootx`.
   * `diff --git a/bsd/dev/sweet_emmc.c b/bsd/dev/sweet_emmc.c` (new file) plus the
     three new headers/sources from Phase 1.
   * If the new `.c`/`.h` files are added rather than patched in, remember
     `bsd/conf/files` must list **each** `.c` that gets compiled.
9. **Root read/write:** `0031-sweet-read-write-root.patch` already provides
   `rootrw=1` → clears `MNT_RDONLY` (`0031-...patch:7-18`) and the dirty-root allowance
   (`0031-...patch:28-44`). **No new patch needed** for those.
10. **Unsealed root:** `0029-sweet-unsealed-root-volume.patch` already downgrades the
    root-authentication panic to a warning (`0029-...patch:8-21`). **No new patch needed.**
11. **DMA above 4 GiB:** port `0034-sweet-ufs-dma-above-4g.patch:19-31` to eMMC. The
    UFS check reads `REG_CAP` bit 24 (`CAP_64AS`). For SDHCI the equivalent is
    `kRegCapabilities` (0x40) bit 30 (64-bit addressing) — **add a constant** for it in
    the copied `SdhciRegs.h`; the UniXNU header does **not** define it (only
    `kCapTimeoutClockMask`, `kCapBaseClockShift`, `kCapBaseClockMask` at
    `SdhciRegs.h:150-154`). The UniXNU driver sidesteps the whole problem by pinning its
    DMA buffers below 4 GiB (`NativeSdhci.cpp:386-390`, `physicalBelow4G()` at
    `:365-372`); **the simplest port is to do the same**.

### 17.5 Phase 4 — the device tree node

12. **`ios-sweet-lg-g4/tools/mkadt.py`** — add an eMMC node next to the existing UFS one
    (`tools/mkadt.py:200-205`):
    ```python
    # bsd/dev/sweet_emmc.c: the eMMC host (two windows: hc, core).
    emmc = node('sweet-emmc', [
        ph(),
        ('compatible', cstr('qcom,sdhci-msm')),
        ('reg', u64(EMMC_HC, 0x1a0) + u64(EMMC_CORE, 0x800)),
    ])
    ```
    and add `emmc` to the root's child list at `tools/mkadt.py:241`
    (`+ [ufs]` → `+ [ufs, emmc]`).
    * `EMMC_HC = 0xf9824900`, `EMMC_CORE = 0xf9824000` — **identical to the Lumia**
      (§16.2).
    * The `reg` property must carry **both** windows; the UniXNC driver requires them
      (`NativeSdhci.cpp:181-186`). If you only need the `hc` window, emit just that one
      and simplify the BSD driver's mapping accordingly.
13. **Do not** use `"qcom,sdhci-msm"` as the *personality* match string if you ever go
    Option B: on the G4 that string is on **all three** SDHCI nodes
    (`msm8992-lg-h815-boot.dts:6393, 6427, 6467`). The Lumia's ADT uses the specific
    `"qcom,msm8994-sdhci"` (`make-lumia950-adt.py:383`), which is unique to `sdhc1`.

### 17.6 Phase 5 — the boot/shim stage

14. **New file** `ios-sweet-lg-g4/shim/emmc.c`, modelled on `shim/ufs.c`. The shim runs
    before the MMU is on (`shim/shim.c:386-388` calls `test_ufs()` at that point), so it
    can use plain physical addresses like `shim/ufs.c:16`:
    ```c
    #define UFS_BASE	0x01d84000UL	/* ufshc@1d84000, sweet live FDT */
    ```
    → `#define EMMC_BASE 0xf9824900UL` for the G4.
    * `shim/ufs.c` already contains the reusable pieces: `scsi()` (`:86`),
      `read_blocks()` (`:138`), `fs_type()` (`:154`), `list_partitions()` (`:167`).
      For eMMC these become an MMC command engine instead of SCSI — the
      `PollingHost::issue()` logic from §6.2 is the model.
    * Wire it into `shim/shim.c` next to `test_ufs()` (`shim/shim.c:386-392`).
15. **Boot route (out of scope for the driver, but a hard dependency):** the G4's
    bootloader is a **locked Qualcomm LK/ABL** with **no fastboot**. `ios-sweet`'s
    route is *"Stock Xiaomi bootloader → `fastboot boot` of our shim → XNU"*
    (`ios-sweet-lg-g4/README.md`, "How it boots"). The LG G4 has no equivalent.
    This must be solved **before** any eMMC code can be exercised on the handset.
    The UniXNU board solved it by **chainloading** an EFI payload from the phone's own
    UEFI (`board.toml:151-155`).

### 17.7 Phase 6 — test and CI

16. **Host unit tests.** The UniXNC core is testable on the build host because it is
    freestanding. Port `UniXNU-lumia950xl/drivers/NativeSdhci/Test/check-sdhci-core.cpp`
    and `.../Test/check-polling-host.cpp`, and the runner
    `UniXNU-lumia950xl/scripts/test-storage.sh:1-14`, into the sweet repo. The runner
    compiles `SdhciCore.cpp` + `PollingHost.cpp` with
    `-fsanitize=address,undefined -Wall -Wextra -Werror` — keep that.
    * The sweet repo has no `scripts/` directory; `tools/` is the convention
      (`tools/validate-repo.py:80` globs `tools/*.py`, `:105` globs `tools/*.sh`).
      Put the runner at `tools/test-emmc.sh`.
    * `tools/validate-repo.py:102-106` runs `bash -n` on `tools/*.sh` — the new runner
      must pass.
17. **CI.** The sweet repo has **no `board.toml`** and **no per-driver archive builder**
    — the XNU source is fetched and patched, so the driver arrives as patch content, not
    as an archive. `tools/validate-repo.py` is the equivalent of UniXNU's `validate` job.
    The eMMC driver therefore needs **no** `build-arm-driver.py` / `COMPONENTS` entry and
    **no** `original-bindings` step.
18. **Documentation.** Add a `docs/g4-hardware/` note (this file's siblings) recording
    the G4's EXT_CSD values, RCA, partition geometry and the cache-coherency
    workaround, as `UniXNU-lumia950xl/docs/storage-bringup.md` does.

### 17.8 Ordered summary of every file that must change

| # | File (relative to `ios-sweet-lg-g4`) | Change |
| --- | --- | --- |
| 1 | `xnu/patches/0082-sweet-emmc-root-device.patch` | **new** — `bsd/conf/files` + `IOKitBSDInit.cpp` hook + `bsd/dev/sweet_emmc.c` + the three new headers/sources |
| 2 | `tools/mkadt.py` | add the `sweet-emmc` node (line ~200) and add it to the root children (line ~241) |
| 3 | `tools/test-emmc.sh` | **new** — host unit-test runner (mirrors `UniXNU-lumia950xl/scripts/test-storage.sh`) |
| 4 | `shim/emmc.c` | **new** — bootloader-stage eMMC probe |
| 5 | `shim/shim.c` | call `test_emmc()` next to `test_ufs()` (lines ~386-392) |
| 6 | `shim/build.sh` | add the new object |
| 7 | `tools/validate-repo.py` | no change needed (it globs `tools/*.sh` automatically) — but confirm `bash -n` passes on the new runner |
| 8 | `README.md` / `NOTES.md` | record the G4 eMMC facts |
| 9 | *(optional)* `xnu/patches/0083-sweet-emmc-dma-above-4g.patch` | if you want the `0034`-equivalent 64-bit-addressing guard |

---

## 18. NOT FOUND / unresolved

Every item below was searched for and **not found**. The search command is given.

| Item | Search command | Result |
| --- | --- | --- |
| Any `AppleSD`, `AppleAHCI` or `AppleNAND` platform-entity code | `grep -rn 'AppleSD\|AppleAHCI\|AppleNAND' "/Users/io/Dev/ios/lg g4/UniXNU-lumia950xl"` | **NOT FOUND** — the port uses the prelinked `IOStorageFamily` kext unchanged; no Apple platform-entity source is vendored |
| Any `IONotify` / interrupt-driven completion in the eMMC driver | `grep -rn 'IONotify\|IOInterruptEventSource' .../drivers/NativeSdhci` | **NOT FOUND** — the driver is polled throughout |
| Any `ioctl` interface to the eMMC driver | `grep -rn 'ioctl' .../drivers/NativeSdhci` | **NOT FOUND** — no userland control path |
| Any FDT / Linux device-tree parsing in the eMMC driver | `grep -rn 'fdt_\|FDT_\|dtb' .../drivers/NativeSdhci` | **NOT FOUND** — the driver is driven by the Apple ADT node via `mapDeviceMemoryWithIndex` |
| Any `CMD16` (SET_BLOCK_LENGTH) actually issued | `grep -rn 'kCmd16SetBlockLength' .../drivers/NativeSdhci` | **NOT FOUND** outside `SdhciRegs.h:179` — the constant is declared but unused |
| Any `CMD18` (READ_MULTIPLE) or `CMD25` (WRITE_MULTIPLE) actually issued | `grep -rn 'kCmd18ReadMultiple\|kCmd25WriteMultiple' .../drivers/NativeSdhci` | **NOT FOUND** outside `SdhciRegs.h:181,183` — the bring-up path is single-block CMD17/CMD24 only |
| A 64-bit-DMA capability bit for SDHCI | `grep -n 'CAPABILITIES_64\|0x40000000' .../drivers/NativeSdhci/SdhciRegs.h` | **NOT FOUND** — `kRegCapabilities` (0x40) is defined but only `kCapTimeoutClockMask` / `kCapBaseClockShift` / `kCapBaseClockMask` are decoded (`SdhciRegs.h:150-154`) |
| The HS200/HS400 QSDCC vendor (DLL / power) register offsets | `grep -rn 'DLL\|dll' .../drivers/NativeSdhci` | **NOT FOUND** — explicitly out of scope: `SdhciRegs.h:11-15` (*"those offsets are deliberately absent until they are confirmed against the firmware that programs them"*) and `BoardConstants.h:35-37` |
| An `IOService`-level `root-device` / `RootDirectory` / `AppleVolume` mechanism in UniXNU | `grep -rn 'RootDirectory\|AppleVolume\|root-device' .../UniXNU-lumia950xl` | **NOT FOUND** — the root is selected by `/chosen/root-matching` → IOMedia UUID (§10.7) |
| A `disk0` naming mechanism in UniXNU | `grep -rn 'disk0' .../UniXNU-lumia950xl` | **NOT FOUND** — `disk0` comes from Apple's prelinked `IOStorageFamily`, not from this repo |
| The G4's eMMC partition table format (GPT vs MBR) | searched `device_lge_g4-common/`, `device_lge_g4-lineage/`, `G4-Titan-Kernel-titan_bpf2/` for `*partition*` | **NOT FOUND** — the partition layout lives in Qualcomm's `msm-common` repo, which is not present. `device_lge_g4-common/BoardConfigCommon.mk:111-115` shows only fixed-size AOSP image partitions. **This must be read off the handset.** |
| The G4's eMMC EXT_CSD contents, RCA, command-queue support | `grep -rn 'ext_csd\|EXT_CSD\|cid' .../lg g4/*.dts` | **NOT FOUND** — no eMMC data in the device trees |
| A `board.toml` in `ios-sweet-lg-g4` | `ls ios-sweet-lg-g4/board.toml` | **NOT FOUND** — the sweet repo has no board description; `UniXNU`'s `board.toml` format is documented in §14 from the UniXNU side only |
| A `build-arm-driver.py` equivalent in `ios-sweet-lg-g4` | `ls ios-sweet-lg-g4/tools/` | **NOT FOUND** — the sweet repo patches XNU source directly; there is no archive builder and no `original-bindings` step |
| The `ios-v60` project having an eMMC driver | `find ios-v60 -iname '*emmc*' -o -iname '*sdhci*'` | **NOT FOUND** — the V60 is SM8250/UFS; its storage work is `shim/ufs.c` + patches 0028/0030/0034/0069 |
| `IOHIDDriver` in either repo | `grep -rn 'IOHIDDriver' ios-sweet-lg-g4` | **NOT FOUND** — storage in `ios-sweet` is a BSD device, not a HID personality; the HID path in `ios-sweet` is the *touch* driver (`drivers/NativeHID` in UniXNU) |
| An `AppleVolume`/`AppleSD` stub patch | `ls UniXNU-lumia950xl/patches-parked/` | **NOT FOUND** — `patches-parked/` holds exactly one file (`native-sdhci-personality.patch`) |

---

## 19. Risk register

| Risk | Severity | Evidence | Mitigation |
| --- | --- | --- | --- |
| The G4's eMMC has **command queue enabled**, which the UniXNC qualifier refuses | **high** | `PollingHost.cpp:423` → `WarmCardError::CommandQueue` | Add a CMDQ-disable path (EXT_CSD `CMDQ_MODE`, index 231) or a CMDQ-aware data path, before anything else |
| The G4's eMMC is **MBR**, not GPT | **high** | `BoardConstants.h:87-90`, `SdhciCore.h:84-85` assume `"EFI PART"`; G4 partition layout not found (§18) | Read LBA 0 on the handset; if MBR, write an MBR parser (the UFS driver already parses GPT by hand at `0028-...patch:252-304`, so the pattern is there) |
| The `compatible` string is `"qcom,sdhci-msm"` on all three G4 SDHCI nodes | medium | `msm8992-lg-h815-boot.dts:6393, 6427, 6467` | Option A has no personality, so this only matters for ADT node selection; give the node a unique path (`/sweet-emmc`) |
| The G4 runs **HS400 1.8 V** in stock | medium | `msm8992-lg-h815.dts:1068` (`mmc-hs400-1_8v`) | Start at legacy 25 MHz (`kTransferHz`), exactly as the Lumia did; HS/HS200 needs the QSDCC DLL offsets, which the UniXNU driver deliberately does not have (`SdhciRegs.h:11-15`) |
| DMA above 4 GiB | medium | `0034-...patch:19-31` (UFS equivalent); UniXNU pins buffers below 4 GiB (`NativeSdhci.cpp:386-390`) | Reuse the UniXNU approach: `IOBufferMemoryDescriptor`-style allocation with a 4 GiB physical mask, or `kmem_alloc_contig` with a low-address constraint |
| The G4 bootloader is locked, **no fastboot** | **high** (boot route) | `ios-sweet-lg-g4/README.md` boot chain; no LG equivalent | Solve separately (EDL / unlocked-boot exploit / chainload). The eMMC driver can still be validated in a host test harness and, later, via EDL |
| The iOS root partition name differs | medium | `"ios"` (`BoardConstants.h:77`) vs `"userdata"` (`0028-...patch:507`) | Make it a boot-arg (`emmcroot=`) as the UFS driver does |
| Cache incoherency | medium | `NativeSdhci.cpp:354-363`, `HARDWARE.md:75-76` | `dc civac` by hand around each transfer, or `flush_dcache64` as the UFS driver does |
| The G4's DRAM starts at a different physical base | low | `msm8992-lg-h815-boot.dts` memory node not parsed in this report | Check `/proc/iomem` / the ADT `memory` node when the board config is written |

---

## 20. Source-file index (for quick reference)

| Topic | File | Key lines |
| --- | --- | --- |
| SDHCI registers | `UniXNU-lumia950xl/drivers/NativeSdhci/SdhciRegs.h` | 23-54 (offsets), 56-147 (bits), 167-184 (MMC commands) |
| Board constants | `UniXNU-lumia950xl/drivers/NativeSdhci/BoardConstants.h` | 29-90 |
| Command engine | `UniXNU-lumia950xl/drivers/NativeSdhci/PollingHost.cpp` | 96-108 (wait), 110-197 (issue), 199-258 (moveData), 329-353 (attach), 355-404 (initialize), 406-430 (finish) |
| CSD/EXT_CSD/GPT | `UniXNU-lumia950xl/drivers/NativeSdhci/SdhciCore.h` / `.cpp` | 26, 28, 41, 72, 75-85 |
| IOKit wrapper | `UniXNU-lumia950xl/drivers/NativeSdhci/NativeSdhci.cpp` | 177-217 (mapWindows), 354-372 (cache/4G), 473-495 (GPT window), 702-738 (probe), 740-777 (start), 873-942 (doAsyncReadWrite) |
| Personality publish | `UniXNU-lumia950xl/unixnu-overlay/patches/xnu-8796/zzv-aes-shim.patch` | 51-86 |
| Personality call site | `UniXNU-lumia950xl/unixnu-overlay/patches/xnu-8796/zzw-procmon.patch` | 17, 324-333 |
| Kernel link | `UniXNU-lumia950xl/unixnu-overlay/build-xnu-4k-baseline-native-sdhci.patch` | 3-22 |
| Parked personality | `UniXNU-lumia950xl/patches-parked/native-sdhci-personality.patch` | 1-17 |
| Original bindings | `UniXNU-lumia950xl/drivers/NativeSdhci/original-bindings-16.7.16.S` | 1-33 |
| Archive builder | `UniXNU-lumia950xl/scripts/build-native-sdhci16.py` | 1-30 |
| Component list | `UniXNU-lumia950xl/scripts/build-arm-driver.py` | 22-40 |
| ADT generator | `UniXNU-lumia950xl/scripts/make-lumia950-adt.py` | 58-62, 331-338, 380-388, 504-516 |
| Board description | `UniXNU-lumia950xl/board.toml` | 1-14, 16-61, 112-136, 138-149, 161-196 |
| Defconfig | `UniXNU-lumia950xl/configs/lumia950_defconfig` | 1-13, 27-29, 37 |
| Host tests | `UniXNU-lumia950xl/scripts/test-storage.sh` | 1-14 |
| Bring-up status | `UniXNU-lumia950xl/docs/storage-bringup.md` | 1-60 |
| Hardware notes | `UniXNU-lumia950xl/docs/lumia950xl/HARDWARE.md` | 16, 70-76 |
| CI notes | `UniXNU-lumia950xl/README_BEFORESTARTING!!!!!!!!!!!!!!.md` | 35-41, 55-58 |
| Sweet UFS driver | `ios-sweet-lg-g4/xnu/patches/0028-sweet-ufs-root-device.patch` | 1-11 (conf/files), 12-35 (root hook), 40-580 (driver) |
| Sweet writes / 4G | `ios-sweet-lg-g4/xnu/patches/0030-…`, `0034-…` | 0030:1-224; 0034:19-31 |
| Sweet rw root / unsealed | `ios-sweet-lg-g4/xnu/patches/0031-…`, `0029-…` | 0031:1-46; 0029:1-24 |
| Sweet no-IOMedia | `ios-sweet-lg-g4/xnu/patches/0022-…` | 1-27 |
| Sweet ADT | `ios-sweet-lg-g4/tools/mkadt.py` | 200-205, 241 |
| Sweet shim UFS | `ios-sweet-lg-g4/shim/ufs.c` | 16, 86-136, 138-143, 167-193, 195-266 |
| Sweet shim entry | `ios-sweet-lg-g4/shim/shim.c` | 373-392, 410 |
| Sweet patch validation | `ios-sweet-lg-g4/tools/validate-repo.py` | 7-9, 25-75, 102-106 |
| LG G4 device tree | `/Users/io/Dev/ios/lg g4/msm8992-lg-h815-boot.dts` | 6392-6424 (sdhc1), 6426-6464 (sdhc2), 6466-… (sdhc3), 13268-13272 (aliases) |
| LG G4 runtime DT | `/Users/io/Dev/ios/lg g4/msm8992-lg-h815.dts` | 1068 (`mmc-hs400-1_8v`) |
| LG G4 Linux driver | `/Users/io/Dev/ios/lg g4/G4-Titan-Kernel-titan_bpf2/drivers/mmc/host/sdhci-msm.c` | (Linux reference for the exact controller) |

---

*Report generated 2026-11-04. Read-only research; no file in
`/Users/io/Dev/ios/lg g4/UniXNU-lumia950xl` was modified.*
