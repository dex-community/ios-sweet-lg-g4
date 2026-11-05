# LG G4 (H815, MSM8992) — Hardware Facts Extracted from Device Tree

**Purpose:** ground truth for a custom bootloader shim + XNU kernel on LG G4 H815.
**Primary source of truth:** `/Users/io/Dev/ios/lg g4/msm8992-lg-h815-boot.dts`
(13 441 lines, decompile of `msm8992-lg-h815-boot.dtb`).

Every fact below is tagged:

* **CONFIRMED** — read directly from a DTS/DT source, with `file:line`.
* **INFERRED** — logically derived; the derivation is stated.
* **NOT FOUND** — searched with the listed command/pattern and absent.

Absolute paths below are shortened:

| Short name | Absolute path |
|---|---|
| `BOOT` | `/Users/io/Dev/ios/lg g4/msm8992-lg-h815-boot.dts` |
| `AOSPHTML` | `/Users/io/Dev/ios/lg g4/msm8992-lg-h815.dts` (**not a DTS — see §0.2**) |
| `KDT` | `/Users/io/Dev/ios/lg g4/G4-Titan-Kernel-titan_bpf2/arch/arm64/boot/dts` |
| `KDRV` | `/Users/io/Dev/ios/lg g4/G4-Titan-Kernel-titan_bpf2/drivers` |

---

## 0. Provenance and source hygiene

### 0.1 The four extracted DTBs are near-identical clones

**CONFIRMED.** `md5` of every DTB in the workspace:

```
MD5 (aicp/dtb_estratto/dtb_0x1000.dtb) = 7213c13e0186ac1a78165d31310c0e55
MD5 (aicp/dtb_estratto/dtb_0x4c000.dtb) = 9eb9bc32cd8af6304df6b03e314cf662
MD5 (aicp/dtb_estratto/dtb_0x97000.dtb) = dea7b3e6ec41716c0cb01554bf785bda
MD5 (aicp/dtb_estratto/dtb_0xe2000.dtb) = 41310ea5d5c5c12a64270a4a9bb8cde1
MD5 (msm8992-lg-h815-boot.dtb)         = 41310ea5d5c5c12a64270a4a9bb8cde1   <-- identical to dtb_0xe2000
```

`diff aicp/dtb_estratto/dtb_0x1000.dts msm8992-lg-h815-boot.dts` shows the **only**
differences across the four trees are:

* root `qcom,board-id`: `<0x464 0x00 0x364 0x00>` (0x1000) vs `<0xb64 0x00>` (others)
* one `status = "ok"` / `"disable"` flip pair at lines 5836 / 5850 (a single peripheral).

**INFERRED:** these are the four hardware revisions of the same board; the H815
unit in hand is the `board-id = 0xb64` variant. The LGE kernel builds exactly
four such DTBs — `msm8992-p1_global_com-rev-a|b|c|10`
(`KDT/lge/Makefile:20`, `KDT/../arch/arm64/configs/h815_defconfig:442`), and
`KDT/lge/msm8992-p1_global_com-rev-10.dts:22` carries `qcom,board-id = <0xB64 0>`.
So `BOOT` == **rev-10**.

### 0.2 `msm8992-lg-h815.dts` is an HTML page, not a DTS

**CONFIRMED.** `head -40` shows `<!DOCTYPE html>` and
`<title>arch/arm64/boot/dts/qcom/msm8992-lg-h815.dts — kernel/common.git — Git at Google</title>`.
It is a saved Gitiles web view of an **AOSP `kernel/common`** file (not the LGE
kernel in this workspace). Line numbers cited below as `AOSP-extract:N` refer to
a de-HTML'd copy produced by stripping tags; the file no longer exists upstream:

```
$ curl -sL ".../kernel/common/+/refs/heads/common/arch/arm64/boot/dts/qcom/msm8992-lg-h815.dts?format=TEXT"
NOT_FOUND: Requested entity was not found   (HTTP 404)
```

It is used here **only as a cross-check**, never as primary evidence.

### 0.3 `dtc` re-decompiles cleanly

**CONFIRMED.** `dtc -I dtb -O dts` on all four `aicp/dtb_estratto/*.dtb` succeeds and
yields 13 441-line trees matching the supplied `.dts`.

---

## A. Framebuffer / Display

### A.1 `simple-framebuffer` — NOT FOUND

**NOT FOUND.** Searched:

```bash
grep -c "simple-framebuffer" /tmp/dtb_0x1000.dts /tmp/dtb_0x4c000.dts \
                              /tmp/dtb_0x97000.dts /tmp/dtb_0xe2000.dts   # -> 0 0 0 0
grep -n "simple-framebuffer\|boot-logo\|winoh" BOOT                        # -> no hits
grep -rn "simple-framebuffer" KDT/                                          # -> no hits
```

The `boot-logo` / `active` / `winoh` properties belong to the Linux `simplefb`
binding. **There is no `simple-framebuffer` node in any H815 DTB, and no
`boot-logo` property anywhere.** Consequently:

* **framebuffer physical address** → **NOT FOUND as an explicit DTS property.**
* **stride in bytes** → **NOT FOUND as an explicit DTS property.**
* **pixel format string (`a8r8g8b8` / `x8r8g8b8` …)** → **NOT FOUND**; only the
  DSI wire bpp is present (see A.5). The Android `mdss_fb` format is selected at
  runtime by the MDP driver, not in DT.

**INFERRED (framebuffer address):** the display buffer pool is the
`cont_splash_mem` carve-out at **physical `0x0340_0000`**, size **0x00C0_0000
(12 MiB)** — see A.8. The AOSP reference explicitly names that region
`cont_splash_mem: fb@3400000` (`AOSP-extract:41-44`), i.e. upstream treats it as
*the* framebuffer region. This is a **reserved-memory pool the MDP driver
allocates from**, not a fixed MMIO framebuffer window.

### A.2 MDSS MDP display controller

**CONFIRMED** — `BOOT:759`:

```
		qcom,mdss_mdp@fd900000 {
			compatible = "qcom,mdss_mdp";
			reg = <0xfd900000 0x90000 0xfd9b0000 0x4d4>;
			reg-names = "mdp_phys", "vbif_phys";
			interrupts = <0x00 0x53 0x00>;
```

| Property | Value | Where |
|---|---|---|
| node path | `/soc/qcom,mdss_mdp@fd900000` | `BOOT:759` |
| compatible | `qcom,mdss_mdp` | `BOOT:760` |
| MDSS MMIO base | `0xfd900000`, size `0x90000` (576 KiB) | `BOOT:761` |
| VBIF base | `0xfd9b0000`, size `0x4d4` | `BOOT:761` |
| **Display interrupt** | **GIC SPI 83** (`0x53`), type `0x00`, flags `0x00` | `BOOT:763` |
| primary intf | `qcom,mdss-pref-prim-intf = "dsi"` | `BOOT:835` |
| clocks | `iface_clk`, `bus_clk`, `core_clk_src`, `core_clk`, `vsync_clk` | `BOOT:823-824` |
| clock controller | phandle `0x18` → `qcom,mmsscc@fd8c0000` | `BOOT:3881-3882` |
| max mixer width | `0x1000` | `BOOT:772` |
| max pipe width | `0x800` | `BOOT:773` |

Cross-check **CONFIRMED**: `KDT/qcom/msm8992-mdss.dtsi:14` `mdss_mdp: qcom,mdss_mdp@fd900000`,
`:16` `reg = <0xfd900000 0x90000>`, `:19` `interrupts = <0 83 0>;` — identical IRQ 83.

### A.3 DSI controllers (two, dual-link panel)

**CONFIRMED** — `BOOT:1623` and `BOOT:1742`:

```
		qcom,mdss_dsi@fd994000 {
			compatible = "qcom,mdss-dsi-ctrl";
			label = "MDSS DSI CTRL->0";
			cell-index = <0x00>;
			reg = <0xfd994000 0x260 0xfd994500 0x280 0xfd994780 0x30 0xfd828000 0x108>;
			reg-names = "dsi_ctrl", "dsi_phy", "dsi_phy_regulator", "mmss_misc_phys";
```

| | DSI0 | DSI1 |
|---|---|---|
| node path | `/soc/qcom,mdss_dsi@fd994000` | `/soc/qcom,mdss_dsi@fd996000` |
| `BOOT` line | 1623 | 1742 |
| compatible | `qcom,mdss-dsi-ctrl` | `qcom,mdss-dsi-ctrl` |
| `dsi_ctrl` reg | `0xfd994000` len `0x260` | `0xfd996000` len `0x260` |
| `dsi_phy` reg | `0xfd994500` len `0x280` | `0xfd996500` len `0x280` |
| `dsi_phy_regulator` | `0xfd994780` len `0x30` | `0xfd994780` len `0x30` (shared) |
| `mmss_misc_phys` | `0xfd828000` len `0x108` | `0xfd828000` len `0x108` (shared) |
| `qcom,mdss-fb-map` | `<0x2a>` → `qcom,mdss_fb_primary` | `<0x2a>` (same) |
| `qcom,mdss-mdp` | `<0x2b>` → `qcom,mdss_mdp@fd900000` | `<0x2b>` (same) |
| preferred panel | `<0x2f>` → **JDI NT36850 mipi0 cmd** | `<0x37>` → **JDI NT36850 mipi1 cmd** |
| `BOOT` lines | 1634-1635, 1646 | 1753-1754, 1765 |

**CONFIRMED:** phandle `0x2f` is defined at `BOOT:1236-1237` (inside
`qcom,mdss_dsi_jdi_nt36850_1440p_mipi0_cmd`, node starts `BOOT:1171`);
phandle `0x37` at `BOOT:1304-1305` (inside `qcom,mdss_dsi_jdi_nt36850_1440p_mipi1_cmd`,
starts `BOOT:1240`).

**CONFIRMED — no separate DSI interrupt.** Neither DSI controller node carries an
`interrupts` property; **all MDSS/DSI interrupting is done through MDP IRQ 83**
(`BOOT:763`). The DSI nodes have no `interrupt-names` either.

**CONFIRMED — DSI clock names** (`BOOT:1636-1637` for DSI0, `1755-1756` for DSI1):
`mdp_core_clk, iface_clk, core_mmss_clk, bus_clk, byte_clk, pixel_clk, core_clk,
mdss_byte_clk_mux, mdss_pixel_clk_mux, byte_clk_src, pixel_clk_src,
shadow_byte_clk_src, shadow_pixel_clk_src, clk_mdss_dsi1_vco_clk_src`.

### A.4 DSI PLLs

**CONFIRMED**:

* `BOOT:611` `qcom,mdss_dsi_pll@fd994300`, compatible `qcom,mdss_dsi_pll_8992`,
  label "MDSS DSI 0 PLL", `reg = <0xfd994300 0x500 0xfd8c2300 0x08 0xfd994200 0x64 0xfd996300 0x500>`,
  `reg-names = "pll_base","gdsc_base","dynamic_pll_base","pll_1_base"` (`BOOT:616-617`).
* `BOOT:662` `qcom,mdss_dsi_pll@fd996300`, compatible `qcom,mdss_dsi_pll_8992`,
  label "MDSS DSI 1 PLL", `reg = <0xfd996300 0x500 0xfd8c2300 0x08>` (`BOOT:667`).
* HDMI PLL: `BOOT:711` `qcom,mdss_hdmi_pll@0xfd9a0600`, compatible `qcom,mdss_hdmi_pll_8992`.

### A.5 Panel nodes (six panel definitions; JDI is preferred)

**CONFIRMED.** Panel node list and key properties:

| node path | `BOOT` line | name | type | width | height | bpp | phandle |
|---|---|---|---|---|---|---|---|
| `qcom,mdss_dsi_sim_video` | 865 | Simulator video mode | video | `0x280` (640) | `0x1e0` (480) | `0x18` | – |
| `qcom,mdss_dsi_sim_video_0` | 910 | – | – | – | – | – | – |
| `qcom,mdss_dsi_sim_video_1` | 956 | – | – | – | – | – | – |
| `qcom,mdss_dsi_sim_cmd` | 1003 | – | – | – | – | – | – |
| `qcom,mdss_dsi_sim_cmd_0` | 1058 | – | – | – | – | – | – |
| `qcom,mdss_dsi_sim_cmd_1` | 1114 | – | – | – | – | – | – |
| **`qcom,mdss_dsi_jdi_nt36850_1440p_mipi0_cmd`** | **1171** | "JDI INCELL 1440p Dual 0 cmd mode dsi panel" | `dsi_cmd_mode` | `0x2d0` (720) | `0xa00` (2560) | `0x18` (24) | `0x2f` |
| **`qcom,mdss_dsi_jdi_nt36850_1440p_mipi1_cmd`** | **1240** | JDI … Dual 1 | `dsi_cmd_mode` | `0x2d0` | `0xa00` | `0x18` | `0x37` |
| `qcom,mdss_dsi_lgd_r69007_1440p_mipi0_cmd` | 1308 | LGD R69007 | `dsi_cmd_mode` | `0x2d0` | `0xa00` | `0x18` | – |
| `qcom,mdss_dsi_lgd_r69007_1440p_mipi1_cmd` | 1390 | LGD R69007 | `dsi_cmd_mode` | `0x2d0` | `0xa00` | `0x18` | – |
| `qcom,mdss_dsi_lgd_lg4943_1440p_mipi0_cmd` | 1468 | "LGD SIC INCELL 1440p Dual 0 cmd mode dsi panel" | `dsi_cmd_mode` | `0x2d0` (1476) | `0xa00` (1477) | `0x18` (1489) | – |
| `qcom,mdss_dsi_lgd_lg4943_1440p_mipi1_cmd` | 1547 | LGD SIC … Dual 1 | `dsi_cmd_mode` | `0x2d0` | `0xa00` | `0x18` | – |

JDI panel detail (`BOOT:1171-1215`):

```
			qcom,mdss-dsi-panel-controller = <0x26>;      /* -> MDSS DSI CTRL->0 */
			qcom,mdss-dsi-panel-type = "dsi_cmd_mode";
			qcom,mdss-dsi-panel-destination = "display_1";
			qcom,mdss-dsi-panel-framerate = <0x3c>;        /* 60 */
			qcom,mdss-dsi-panel-width = <0x2d0>;           /* 720  (per DSI link) */
			qcom,mdss-dsi-panel-height = <0xa00>;           /* 2560 */
			qcom,mdss-dsi-bpp = <0x18>;                    /* 24 bpp on the DSI link */
			qcom,mdss-dsi-color-order = "rgb_swap_rgb";
			qcom,mdss-dsi-traffic-mode = "burst_mode";
			qcom,mdss-dsi-lane-map = "lane_map_0123";
			qcom,cont-splash-enabled;
```

**INFERRED (resolution = 1440 × 2560).** The panel is described as *"1440p Dual"*
(`BOOT:1172`) and driven by **two** DSI controllers (DSI0 `BOOT:1623` +
DSI1 `BOOT:1742`). Per-link width is `0x2d0` = 720, so the full panel width is
2 × 720 = **1440**; height `0xa00` = **2560**.
Cross-check **CONFIRMED**: `KDT/lge/msm8992-p1_global_com/msm8992-p1_global_com-panel.dtsi`
includes `dsi-panel-jdi-nt36850-dualmipi0-qhd-cmd.dtsi` and `…mipi1…`, and sets
`&mdss_dsi0 { qcom,dsi-pref-prim-pan = <&dsi_dual_jdi_cmd_0>; }`,
`&mdss_dsi1 { qcom,dsi-pref-prim-pan = <&dsi_dual_jdi_cmd_1>; }`.

**INFERRED (pixel format).** `qcom,mdss-dsi-bpp = <0x18>` = **24 bpp on the DSI
wire** ⇒ RGB888 / XRGB8888-class. The **MDSS** framebuffer format is chosen at
runtime by the `mdss_fb` driver from `mdss_fb_primary`'s
`qcom,memory-reservation-size`; it is **not** stated in DT.
**No RGB565 anywhere** was found.

**INFERRED (stride).** If the MDP buffer is XRGB8888 (4 B/px) at 1440 px wide,
stride = 1440 × 4 = **5760 bytes**. If the reservation is double-buffered at
4 B/px, total = 14 745 600 B = `0xE10000`. See A.6 for the `0x1C20000` figure,
which is exactly **8 bytes/px** × 3 686 400 px — i.e. the pool is sized for a
**64 bpp** (2 × ARGB8888) allocation. **Treat stride as NOT CONFIRMED.**

### A.6 `qcom,mdss-fb` framebuffer nodes

**CONFIRMED** — `BOOT:839-863`:

```
			qcom,mdss_fb_primary {
				cell-index = <0x00>;
				compatible = "qcom,mdss-fb";
				qcom,memory-reservation-size = <0x1c20000>;
				linux,phandle = <0x2a>;
				phandle = <0x2a>;

				qcom,cont-splash-memory {
					linux,contiguous-region = <0x25>;
				};
			};

			qcom,mdss_fb_external {
				cell-index = <0x01>;
				compatible = "qcom,mdss-fb";
				linux,phandle = <0x3a>;
			};

			qcom,mdss_fb_wfd {
				cell-index = <0x02>;
				compatible = "qcom,mdss-fb";
				linux,phandle = <0x38>;
			};
```

* `qcom,mdss_fb_primary` — `BOOT:839`, `compatible = "qcom,mdss-fb"` `BOOT:841`,
  **`qcom,memory-reservation-size = <0x1c20000>` = 29 491 200 B** `BOOT:842`,
  points at contiguous region `0x25` = `cont_splash_mem` (`BOOT:846-848`).
* Cross-check **CONFIRMED**: `KDT/lge/…/msm8992-p1_global_com-panel.dtsi`
  `&mdss_fb0 { qcom,memory-reservation-size = <0x1C20000>; };`
  and `KDT/qcom/msm8992-mdss.dtsi:186` `mdss_fb0: qcom,mdss_fb_primary`.

**INFERRED:** `0x1C20000` / (1440 × 2560) = **8 bytes per pixel** — the pool is
sized for a 64 bpp (double-buffered ARGB8888) MDP allocation. This is the
strongest available evidence for the framebuffer layout, but the *actual* format
and stride chosen at runtime remain **NOT CONFIRMED**.

### A.7 Display power / panel control GPIOs (TLMM)

**CONFIRMED** — `BOOT:1650-1653` (DSI0) and `BOOT:1766-1769` (DSI1):

```
			qcom,platform-reset-gpio    = <0x34 0x4e 0x00>;   /* TLMM GPIO 78  */
			qcom,platform-vddio_en-gpio = <0x34 0x6e 0x00>;   /* TLMM GPIO 110 */
			qcom,platform-avdd-gpio    = <0x34 0x59 0x00>;   /* TLMM GPIO 89  */
			lge,platform-touch-io       = <0x34 0x3f 0x00>;   /* TLMM GPIO 63  */
```

Phandle `0x34` = TLMM GPIO controller (§B.4). Cross-check **CONFIRMED** in
`KDT/lge/…/msm8992-p1_global_com-panel.dtsi`: `&msm_gpio 78`, `&msm_gpio 110`,
`&msm_gpio 89`, `&msm_gpio 63`. Also `lge,reset-low-hold = <0x0a>` on DSI1
(`BOOT:1772`).

**CONFIRMED** — backlight control is via PMIC WLED: `qcom,mdss-dsi-bl-pmic-control-type = "bl_ctrl_wled"`,
`qcom,mdss-dsi-bl-min-level = <0x01>`, `qcom,mdss-dsi-bl-max-level = <0xfff>` (`BOOT:1543-1545`).

### A.8 Memory reservations relevant to display

**CONFIRMED** — `BOOT:13285-13379`, the legacy Qualcomm style (**not** a standard
`/reserved-memory` node; see E.5). Display-relevant entries:

| node | `BOOT` line | reg (base, size) | flags |
|---|---|---|---|
| `cont_splash_mem@0` | 13334 | `0x0340_0000`, `0x00C0_0000` | `linux,reserve-contiguous-region`, `linux,reserve-region`, phandle `0x25` |
| `crash_fb_mem@4000000` | 13373 | `0x0400_0000`, `0x00C0_0000` | `linux,reserve-contiguous-region`, `linux,reserve-region` |
| `dfps_data_mem@0` | 13324 | `0x0340_0000`, `0x1000` | **`status = "disabled"`** |

```
			cont_splash_mem@0 {
				linux,reserve-contiguous-region;
				linux,reserve-region;
				reg = <0x00 0x3400000 0x00 0xc00000>;
				label = "cont_splash_mem";
				linux,phandle = <0x25>;
				phandle = <0x25>;
			};
```

**INFERRED:** the display/splash buffer pool = **physical `0x0340_0000`**, 12 MiB
(`0xC00000` = 12 582 912 B). The AOSP reference confirms this is *the*
framebuffer region: `cont_splash_mem: fb@3400000 { reg = <0x0 0x03400000 0x0 0x00c00000>; no-map; }`
(`AOSP-extract:41-44`).

**INFERRED:** 12 MiB is *not* enough for a single 1440×2560 ARGB8888 buffer
(14 745 600 B) — so the MDP must allocate out of a *larger* pool, or use
FBC/tiled formats, or the 12 MiB region is only the **boot splash** buffer.
**Do not hard-code `0x03400000` as the XNU framebuffer base without reading the
MDP's actual buffer descriptor.**

---

## B. Touch controller

### B.1 The I²C bus

**CONFIRMED** — `BOOT:5493-5512`:

```
		i2c@f9924000 {
			compatible = "qcom,i2c-msm-v2";
			#address-cells = <0x01>;
			#size-cells = <0x00>;
			reg-names = "qup_phys_addr";
			reg = <0xf9924000 0x1000>;
			interrupt-names = "qup_irq";
			interrupts = <0x00 0x60 0x00>;
			qcom,clk-freq-out = <0x61a80>;
			qcom,clk-freq-in = <0x124f800>;
			clock-names = "iface_clk", "core_clk";
			clocks = <0xa4 0x8caa5b4f 0xa4 0x1076f220>;
			…
			qcom,master-id = <0x56>;
```

| Property | Value | Where |
|---|---|---|
| node path | `/soc/i2c@f9924000` | `BOOT:5493` |
| compatible | `qcom,i2c-msm-v2` | `BOOT:5494` |
| MMIO base | `0xf9924000`, size `0x1000` | `BOOT:5498` |
| **I²C IRQ** | **GIC SPI 96** (`0x60`) | `BOOT:5500` |
| bus index | **i2c bus 2** (`i2c2` alias) | `BOOT:13274` |
| bus HW | BLSP1 QUP2 | `KDT/qcom/msm8992.dtsi:1189` (`i2c_2: i2c@f9924000 { /* BLSP1 QUP2 */ }`) |
| SCL frequency | `0x61a80` = 400 000 Hz | `BOOT:5501` |
| clocks | `iface_clk`, `core_clk` (phandle `0xa4` = GCC) | `BOOT:5503-5504` |

**CONFIRMED** alias table: `BOOT:13274` `i2c2 = "/soc/i2c@f9924000";`

### B.2 Touch candidate 1 — Synaptics S3528 (RMI4)

**CONFIRMED** — `BOOT:5514-5553`:

```
			synaptics@20 {
				status = "ok";
				compatible = "synaptics,s3528";
				reg = <0x20>;
				interrupt-parent = <0x34>;
				interrupts = <0x16 0x2008>;
				vcc_i2c-supply = <0x35>;
				vdd_ana-supply = <0x36>;
				pinctrl-names = "pmx_ts_active", "pmx_ts_suspend";
				pinctrl-0 = <0xc8 0xc9>;
				pinctrl-1 = <0xca 0xcb>;
				synaptics,display-coords = <0x00 0x00 0x59f 0x9ff>;
				synaptics,panel-coords = <0x00 0x00 0x59f 0x9ff>;
				reset-gpio = <0x34 0x08 0x00>;
				irq-gpio = <0x34 0x16 0x2008>;
				maker_id-gpio = <0x34 0x0c 0x00>;
				synaptics,disable-gpios;
```

| Property | Value | Where |
|---|---|---|
| node path | `/soc/i2c@f9924000/synaptics@20` | `BOOT:5514` |
| compatible | `synaptics,s3528` | `BOOT:5516` |
| **I²C slave address** | **`0x20`** | `BOOT:5517` |
| interrupt-parent | `0x34` = TLMM GPIO | `BOOT:5518` |
| **interrupts** | **`<0x16 0x2008>` → TLMM GPIO 22**, flags `0x2008` | `BOOT:5519` |
| **irq-gpio** | **`<0x34 0x16 0x2008>` → GPIO 22** | `BOOT:5528` |
| reset-gpio | `<0x34 0x08 0x00>` → **TLMM GPIO 8** | `BOOT:5527` |
| maker_id-gpio | `<0x34 0x0c 0x00>` → **TLMM GPIO 12** | `BOOT:5529` |
| interrupt-names | **absent** (no `interrupt-names` property) | – |
| `synaptics,display-coords` | `<0 0 0x59f 0x9ff>` = 0,0,1439,2559 | `BOOT:5525` |
| `synaptics,panel-coords` | `<0 0 0x59f 0x9ff>` = 0,0,1439,2559 | `BOOT:5526` |
| `max_x` | `0x59f` = **1439** | `BOOT:5534` |
| `max_y` | `0x9ff` = **2559** | `BOOT:5535` |
| `max_pressure` | `0xff` = 255 | `BOOT:5536` |
| `max_width` | `0x0f` = 15 | `BOOT:5537` |
| `max_orientation` | `0x01` = 1 | `BOOT:5538` |
| `max_id` | `0x0a` = 10 | `BOOT:5539` |
| `protocol_type` | `0x01` (RMI4) | `BOOT:5540` |
| `use_rmi_dev` | `0x01` | `BOOT:5552` |
| `irqflags` | `0x02` | `BOOT:5553` |
| `vcc_i2c-supply` | `0x35` → PM8994 L10 (1.8 V) | `BOOT:5520` |
| `vdd_ana-supply` | `0x36` → PM8994 L22 (3.0 V) | `BOOT:5521` |

**Flag decode `0x2008` (INFERRED, legacy MSM GPIO flag encoding):**
bit0 = 0 → **active high**; bits[2:1] = 0 → **`IRQ_TYPE_EDGE_RISING`**;
bit13 (`0x2000`) → **`GPIO_PULL_UP`**. Note bit13 is *also* the Qualcomm
`direct-connect` style bit in some bindings — treat the pull interpretation as
probable, not certain.

### B.3 Touch candidate 2 — LG SIC LG2411 (same bus, same IRQ)

**CONFIRMED** — `BOOT:5667-5702`:

```
			lgesic_lg2411@28 {
				compatible = "lgesic,lg2411";
				status = "ok";
				reg = <0x28>;
				interrupt-parent = <0x34>;
				interrupts = <0x16 0x2008>;
				vcc_i2c-supply = <0x35>;
				vdd_ana-supply = <0x36>;
				…
				reset-gpio = <0x34 0x08 0x00>;
				irq-gpio = <0x34 0x16 0x2008>;
				…
				max_x = <0x59f>;
				max_y = <0x9ff>;
				…
				use_rmi_dev = <0x00>;
```

* node path: `/soc/i2c@f9924000/lgesic_lg2411@28`, `BOOT:5667`
* compatible `lgesic,lg2411` (`BOOT:5668`), **I²C address `0x28`** (`BOOT:5670`)
* **identical IRQ**: `<0x16 0x2008>` → TLMM GPIO 22 (`BOOT:5672`, `5679`)
* identical reset GPIO 8 (`BOOT:5678`), identical `max_x`/`max_y` = 1439/2559
  (`BOOT:5683-5684`), identical supplies.

### B.4 Which touch is actually populated on the H815?

**NOT FOUND / UNRESOLVED.** The DTB enables **both** (`status = "ok"` on
`BOOT:5515` and `BOOT:5669`) with **identical reset and IRQ GPIOs**, so exactly
one can be physically populated. Evidence:

* **CONFIRMED:** `KDT/lge/msm8992-p1_global_com/msm8992-p1_global_com-touch.dtsi:13-14`
  includes *both* `../msm8992-touch-synaptics-s3528.dtsi` and
  `../msm8992-touch-sic-lg2411.dtsi`; neither is disabled.
* **CONFIRMED:** the board rev-*.dts files do not override either node
  (`KDT/lge/msm8992-p1_global_com-rev-{a,b,c,10}.dts` only change `qcom,board-id`
  and one I²C codec address).
* **CONFIRMED:** two distinct drivers exist and bind by `compatible`:
  `KDRV/input/touchscreen/touch_synaptics.c:8160` → `{ .compatible = "synaptics,s3528" }`
  and `KDRV/input/touchscreen/touch_sic.c:5104` → `{ .compatible = "lgesic,lg2411" }`.

**Practical consequence for the XNU driver:** probe **both** I²C addresses
(`0x20` and `0x28`) on bus `i2c@f9924000` (i2c bus 2) at 400 kHz, and select
the one that ACKs. Do not assume Synaptics.

### B.5 TLMM GPIO controller (phandle `0x34`)

**CONFIRMED** — `BOOT:11192-11213`:

```
		pinctrl@fd510000 {
			compatible = "qcom,msm-tlmm-8994", "qcom,msm-tlmm-8974";
			reg = <0xfd510000 0x4000>;
			interrupts = <0x00 0xd0 0x00>;

			gp {
				qcom,num-pins = <0x92>;
				#qcom,pin-cells = <0x01>;
				linux,phandle = <0x195>;
				phandle = <0x195>;

				msm_gpio {
					compatible = "qcom,msm-tlmm-gp";
					gpio-controller;
					#gpio-cells = <0x02>;
					interrupt-controller;
					#interrupt-cells = <0x02>;
					qcom,direct-connect-irqs = <0x08>;
					num_irqs = <0x92>;
					linux,phandle = <0x34>;
					phandle = <0x34>;
				};
			};
```

| Property | Value | Where |
|---|---|---|
| node path | `/soc/pinctrl@fd510000/gp/msm_gpio` | `BOOT:11192`, `11197`, `11203` |
| compatible | `qcom,msm-tlmm-8994`, `qcom,msm-tlmm-8974` | `BOOT:11193` |
| **base / size** | **`0xfd510000` / `0x4000`** | `BOOT:11194` |
| TLMM IRQ | GIC SPI 208 (`0xd0`) | `BOOT:11195` |
| #gpio-cells | 2 (pin, flags) | `BOOT:11206` |
| #interrupt-cells | 2 (pin, flags) | `BOOT:11208` |
| num_irqs / num-pins | 146 (`0x92`) | `BOOT:11210`, `11198` |
| direct-connect-irqs | `<0x08>` → GPIO 8 | `BOOT:11209` |

### B.6 ABS_MT axis ranges (Linux reference for the XNU driver)

**CONFIRMED** — `KDRV/input/touchscreen/lge/touch_core.c:377-393`:

```
	input_set_abs_params(input, ABS_MT_POSITION_X, 0,
			ts->caps.max_x, 0, 0);
#ifdef CONFIG_MACH_MSM8992_PPLUS
	input_set_abs_params(input, ABS_MT_POSITION_Y, 80,
			2800, 0, 0);
#else
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0,
			ts->caps.max_y, 0, 0);
#endif
	input_set_abs_params(input, ABS_MT_PRESSURE, 0,
			ts->caps.max_pressure, 0, 0);
	input_set_abs_params(input, ABS_MT_WIDTH_MAJOR, 0,
			ts->caps.max_width, 0, 0);
	input_set_abs_params(input, ABS_MT_WIDTH_MINOR, 0,
			ts->caps.max_width, 0, 0);
	input_set_abs_params(input, ABS_MT_ORIENTATION, 0,
			ts->caps.max_orientation, 0, 0);
```

* `ts->caps.max_x` / `max_y` come from the DT `max_x` / `max_y`
  (`KDRV/include/linux/input/lge_touch_core.h:131-132`).
* `input_mt_init_slots(input, ts->caps.max_id, 0)` → `max_id = 10`
  (`KDRV/input/touchscreen/lge/touch_core.c:395`).

**INFERRED (XNU ABS_MT space for H815):**
`ABS_MT_POSITION_X ∈ [0, 1439]`, `ABS_MT_POSITION_Y ∈ [0, 2559]`,
`ABS_MT_PRESSURE ∈ [0, 255]`, `ABS_MT_WIDTH_MAJOR/MINOR ∈ [0, 15]`,
`ABS_MT_ORIENTATION ∈ [0, 1]`, **11 slots** (max_id 10 ⇒ `num_slots+1`).
The `CONFIG_MACH_MSM8992_PPLUS` branch (Y ∈ [80, 2800]) applies to the LG V10
"Plus" variant, **not** the P1/H815.

**CONFIRMED:** the Synaptics driver itself performs **no** `input_event` /
`ABS_*` reporting (`grep -c "input_event" touch_synaptics.c` → 0); it feeds
LGE's shared touch core, which does the `input_report_abs` calls
(`KDRV/input/touchscreen/lge/touch_core.c:131-143`).

---

## C. Buttons / keys / PMIC

### C.1 `gpio-keys` node — only TWO keys

**CONFIRMED** — `BOOT:13207-13231`:

```
		gpio_keys {
			compatible = "gpio-keys";
			input-name = "gpio-keys";
			pinctrl-names = "tlmm_gpio_key_active", "tlmm_gpio_key_suspend";
			pinctrl-0 = <0x1a7>;
			pinctrl-1 = <0x1a8>;

			vol_up {
				label = "volume_up";
				gpios = <0xd2 0x03 0x01>;
				linux,input-type = <0x01>;
				linux,code = <0x73>;
				gpio-key,wakeup;
				debounce-interval = <0x0f>;
			};

			hall_ic {
				label = "hall_ic";
				gpios = <0x34 0x4b 0x01>;
				linux,input-type = <0x05>;
				linux,code = <0x00>;
				gpio-key,wakeup;
				debounce-interval = <0x0f>;
			};
		};
```

| key | node | `gpios` | decoded | `linux,code` | wakeup | debounce |
|---|---|---|---|---|---|---|
| **volume up** | `vol_up` (`BOOT:13214`) | `<0xd2 0x03 0x01>` (`13216`) | **PMIC PM8994 GPIO 3**, active-**low** | `0x73` = 115 = **`KEY_VOLUMEUP`** (`13218`) | `gpio-key,wakeup` (`13219`) | `0x0f` = 15 ms (`13220`) |
| **hall / flip sensor** | `hall_ic` (`13223`) | `<0x34 0x4b 0x01>` (`13225`) | **TLMM GPIO 75**, active-**low** | `0x00` = 0 = `KEY_RESERVED` (`13227`) | `gpio-key,wakeup` (`13228`) | 15 ms (`13229`) |

* **`keyup-gpios` is NOT FOUND** — the node uses the legacy `gpios` property.
  (`grep -n "keyup-gpios" BOOT` → no hits.)
* **No `volume_down` and no `power` key in `gpio-keys`.** Those are PMIC PON pins
  (§C.2).
* `linux,input-type`: `0x01` = `EV_KEY` (volume_up), `0x05` = `EV_SW` (hall).
* Cross-check **CONFIRMED**: `KDT/lge/msm8992-touch-synaptics-s3528.dtsi:22-29`
  defines the same `hall_ic` key with `gpios = <&msm_gpio 75 0x1>`;
  `AOSP-extract:64-73` defines `key-vol-up` with `gpios = <&pm8994_gpios 3 GPIO_ACTIVE_LOW>`,
  `linux,code = <KEY_VOLUMEUP>`, `debounce-interval = <15>`.

### C.2 PMIC PON keys (power + volume-down)

**CONFIRMED** — `BOOT:4025-4064`, node `qcom,power-on@800` inside `qcom,pm8994@0`:

```
				qcom,power-on@800 {
					compatible = "qcom,qpnp-power-on";
					reg = <0x800 0x100>;
					interrupts = <0x00 0x08 0x00 0x00 0x08 0x01 0x00 0x08 0x04 0x00 0x08 0x05>;
					interrupt-names = "kpdpwr", "resin", "resin-bark", "kpdpwr-resin-bark";
					…
					qcom,pon_1 {
						qcom,pon-type = <0x00>;
						qcom,pull-up = <0x01>;
						linux,code = <0x74>;
						…
					};

					qcom,pon_2 {
						qcom,pon-type = <0x01>;
						qcom,pull-up = <0x01>;
						linux,code = <0x72>;
						…
					};

					qcom,pon_3 {
						qcom,pon-type = <0x03>;
						…
					};
				};
```

| key | node (`BOOT` line) | `qcom,pon-type` | `linux,code` | meaning |
|---|---|---|---|---|
| **power** | `qcom,pon_1` (4035) | `0x00` (4036) | `0x74` = **116 = `KEY_POWER`** (4038) | PMIC PON1 pin |
| **volume down** | `qcom,pon_2` (4046) | `0x01` (4047) | `0x72` = **114 = `KEY_VOLUMEDOWN`** (4049) | PMIC "resin" / PON2 pin |
| (aux) | `qcom,pon_3` (4054) | `0x03` (4055) | *(none)* | `qcom,s2-type = <0x07>`, uses bark |

Cross-check **CONFIRMED**: `KDT/qcom/msm-pm8994.dtsi:54` `qcom,pon_1 { qcom,pon-type = <0>; … linux,code = <116>; }`,
`:64` `qcom,pon_2 { qcom,pon-type = <1>; … linux,code = <114>; }`.
And `AOSP-extract:93-96`: `&pm8994_resin { linux,code = <KEY_VOLUMEDOWN>; status = "okay"; };`

**INFERRED:** the PMIC's PON2/"resin" pin carries **volume down** on this board.
Note the asymmetry: **volume up = PMIC GPIO 3** (a plain GPIO, `gpio_keys`) while
**volume down = PMIC PON2** (an SPMI interrupt, `irq 0x08` / index 1 in
`qcom,power-on@800`'s `interrupts`). An XNU key driver must handle both paths.

### C.3 SPMI / PMIC-ARB controller

**CONFIRMED** — `BOOT:3989-4002`:

```
		qcom,spmi@fc4c0000 {
			compatible = "qcom,spmi-pmic-arb";
			reg-names = "core", "intr", "cnfg";
			reg = <0xfc4cf000 0x1000 0xfc4cb000 0x1000 0xfc4ca000 0x1000>;
			interrupts = <0x00 0xbe 0x00>;
			qcom,pmic-arb-channel = <0x00>;
			qcom,pmic-arb-ee = <0x00>;
			#interrupt-cells = <0x03>;
			interrupt-controller;
			#address-cells = <0x01>;
			#size-cells = <0x00>;
			cell-index = <0x00>;
			qcom,pmic-arb-max-peripherals = <0x100>;
			qcom,pmic-arb-max-periph-interrupts = <0x100>;
```

| Property | Value | Where |
|---|---|---|
| node path | `/soc/qcom,spmi@fc4c0000` | `BOOT:3989` |
| compatible | `qcom,spmi-pmic-arb` | `BOOT:3990` |
| **core reg** | **`0xfc4cf000` / `0x1000`** | `BOOT:3992` |
| **intr reg** | **`0xfc4cb000` / `0x1000`** | `BOOT:3992` |
| **cnfg reg** | **`0xfc4ca000` / `0x1000`** | `BOOT:3992` |
| **SPMI arbiter IRQ** | **GIC SPI 190** (`0xbe`) | `BOOT:3993` |
| interrupt-controller | yes, `#interrupt-cells = 3` | `BOOT:3996-3997` |

**INFERRED (SPMI bus address):** the node is named `qcom,spmi@fc4c0000` — the
`@fc4c0000` suffix is the **SPMI bus base** `0xfc4c0000` (the three reg windows
`0xfc4cf000`/`0xfc4cb000`/`0xfc4ca000` sit inside it). The `0xb000000` value
mentioned in the task brief is **NOT FOUND** in this tree — searched
`grep -n "b000000" BOOT` → no hits. The per-IC address is given by the child
`reg` property, not a bus-wide address.

### C.4 PMIC children on SPMI

**CONFIRMED** — `BOOT:4004` and `BOOT:4587`:

| node | `BOOT` line | SPMI slave addr (`reg`) | note |
|---|---|---|---|
| `qcom,pm8994@0` | 4004 | **`0x00`** (`BOOT:4006`) | main PMIC (PM8994) |
| `qcom,pm8994@1` | 4587 | **`0x01`** | (PMIC / PMI8994 pair node) |
| `qcom,pmi8994@2` / `@3` | — | `0x02` / `0x03` | present per `KDT/lge/…panel.dtsi` (`&spmi_bus { qcom,pmi8994@3 {…} }`) |

**CONFIRMED** PM8994 sub-nodes (inside `qcom,pm8994@0`):

| node | `BOOT` line | reg |
|---|---|---|
| `qcom,revid@100` | 4010 | `0x100` / `0x100` |
| `qcom,temp-alarm@2400` | 4015 | `0x2400` / `0x100`, IRQ `0x24` |
| `qcom,power-on@800` | 4025 | `0x800` / `0x100` |

### C.5 PMIC GPIO controllers

**CONFIRMED** — two of them:

**(a) PM8994 GPIO — phandle `0xd2` (`BOOT:4066-4075`):**

```
			gpios {
				spmi-dev-container;
				compatible = "qcom,qpnp-pin";
				gpio-controller;
				#gpio-cells = <0x02>;
				#address-cells = <0x01>;
				#size-cells = <0x01>;
				label = "pm8994-gpio";
				linux,phandle = <0xd2>;
				phandle = <0xd2>;
```

| node | `BOOT` line | `reg` | `qcom,pin-num` | status |
|---|---|---|---|---|
| `gpios/gpio@c000` | 4077 | `0xc000` / `0x100` | 1 | disabled |
| `gpios/gpio@c100` | 4083 | `0xc100` / `0x100` | 2 | disabled |
| **`gpios/gpio@c200`** | **4089** | **`0xc200` / `0x100`** | **3** | **okay** |
| `gpios/gpio@c300` | 4099 | `0xc300` / `0x100` | 4 | okay |

**INFERRED:** `gpio@c200` carries `qcom,pin-num = 3`, and it is the only
"okay" PMIC GPIO whose pin number equals the `0x03` used by
`gpio_keys/vol_up` (`BOOT:13216`) ⇒ **volume-up button = PM8994 GPIO 3**,
whose SPMI register block is **`0x0c200`**, size `0x100`.

**(b) PMI8994 GPIO — phandle `0x147` (`BOOT:4709-4718`):**

```
			gpios {
				spmi-dev-container;
				compatible = "qcom,qpnp-pin";
				gpio-controller;
				#gpio-cells = <0x02>;
				#address-cells = <0x01>;
				#size-cells = <0x01>;
				label = "pmi8994-gpio";
				linux,phandle = <0x147>;
				phandle = <0x147>;
```

This is the second PMIC's GPIO block (`qcom,pm8994@1` child, `BOOT:4587`).

**Answer to "exact GPIO number for volume up/down/power if expressed via PMIC gpio":**

| key | expression |
|---|---|
| **volume up** | **PM8994 GPIO 3** (SPMI reg `0x0c200`), active-low, `KEY_VOLUMEUP` (115) |
| **volume down** | **PMIC PON2 / "resin"** — *not* a plain GPIO; SPMI IRQ index 1 within `qcom,power-on@800` (`0x800`), GIC-level via PMIC-arb IRQ 190. `KEY_VOLUMEDOWN` (114) |
| **power** | **PMIC PON1** — SPMI IRQ index 0 within `qcom,power-on@800`. `KEY_POWER` (116) |

**`qcom,pmic-gpio` is NOT FOUND** as a property name — Qualcomm uses
`qcom,qpnp-pin` + `qcom,pin-num` instead. Searched
`grep -n "qcom,pmic-gpio" BOOT` → no hits.

### C.6 Other PMIC GPIO consumers

**CONFIRMED**:

* `bcm,bcm4335` BT: `bcm,bt-reset-gpio = <0x34 0x19 0x00>` → **TLMM GPIO 25** (`BOOT:13202`).
* `lge,appsport`: `interrupt-parent = <0x34>`, `interrupts = <0x24 0x00>` → **TLMM GPIO 36**, `interrupt-names = "ext_det_irq"` (`BOOT:13233-13239`).

---

## D. eMMC / SD / SDIO

### D.1 eMMC host — `sdhc1` (Linux `mmc0`)

**CONFIRMED** — `BOOT:6392-6424`:

```
		sdhci@f9824900 {
			compatible = "qcom,sdhci-msm";
			reg = <0xf9824900 0x1a0 0xf9824000 0x800>;
			reg-names = "hc_mem", "core_mem";
			interrupts = <0x00 0x7b 0x00 0x00 0x8a 0x00>;
			interrupt-names = "hc_irq", "pwr_irq";
			qcom,bus-width = <0x08>;
			…
			qcom,msm-bus,name = "sdhc1";
			…
			clock-names = "iface_clk", "core_clk";
			clocks = <0xa4 0x691e0caa 0xa4 0x9ad6fb96>;
			status = "ok";
			…
			qcom,clk-rates = <0x61a80 0x1312d00 0x17d7840 0x2faf080 0x5f5e100 0xa408300 0x14810600>;
			qcom,bus-speed-mode = "HS400_1p8v", "HS200_1p8v", "DDR_1p8v";
			qcom,nonremovable;
		};
```

| Property | Value | Where |
|---|---|---|
| node path | `/soc/sdhci@f9824900` | `BOOT:6392` |
| compatible | `qcom,sdhci-msm` | `BOOT:6393` |
| **hc_mem base / size** | **`0xf9824900` / `0x1a0`** | `BOOT:6394` |
| **core_mem base / size** | **`0xf9824000` / `0x800`** | `BOOT:6394` |
| **SDCC IRQ (hc_irq)** | **GIC SPI 123** (`0x7b`) | `BOOT:6396` |
| **SDCC pwr_irq** | **GIC SPI 138** (`0x8a`) | `BOOT:6396` |
| interrupt-names | `hc_irq`, `pwr_irq` | `BOOT:6397` |
| **bus-width** | **8** | `BOOT:6398` |
| **non-removable** | **yes** (`qcom,nonremovable`) | `BOOT:6423` |
| bus name | `sdhc1` | `BOOT:6403` |
| clocks | `iface_clk`, `core_clk` (phandle `0xa4` = GCC) | `BOOT:6408-6409` |
| max clock rates | 400 k … 343 MHz | `BOOT:6421` |
| bus-speed-mode | `HS400_1p8v`, `HS200_1p8v`, `DDR_1p8v` | `BOOT:6422` |
| `cd-gpios` | **absent** (fixed device) | – |
| `cap-*` (SD core caps) | **absent** — this is the Qualcomm SDHCI binding | – |

**CONFIRMED host index:** `KDT/qcom/msm8992.dtsi:31-33`

```
		sdhc1 = &sdhc_1; /* SDC1 eMMC slot */
		sdhc2 = &sdhc_2; /* SDC2 SD card slot */
		sdhc3 = &sdhc_3; /* SDC3 WLAN slot - BCMDHD */
```

with `KDT/qcom/msm8992.dtsi:1761` `sdhc_1: sdhci@f9824900` → **eMMC = sdhc1 = mmc0**.

**CONFIRMED** eMMC is the root device: `BOOT:13391`
`dev = "/dev/block/platform/soc.0/f9824900.sdhci/by-name/system";`
(the `f9824900.sdhci` platform path matches `sdhci@f9824900`).

Cross-check **CONFIRMED**: `AOSP-extract:170-176` —
`&sdhc1 { mmc-hs400-1_8v; vmmc-supply = <&pm8994_l20>; vqmmc-supply = <&pm8994_s4>; non-removable; status = "okay"; };`

### D.2 SD card slot — `sdhc2` (removable)

**CONFIRMED** — `BOOT:6426-6464`:

| Property | Value | Where |
|---|---|---|
| node path | `/soc/sdhci@f98a4900` | `BOOT:6426` |
| reg | `<0xf98a4900 0x11c 0xf98a4000 0x800>` (`hc_mem`, `core_mem`) | `BOOT:6428` |
| interrupts | `<0x00 0x01 0x02>` — **one** cell pair only | `BOOT:6430` |
| interrupt-names | `hc_irq`, `pwr_irq`, `status_irq` | `BOOT:6431` |
| bus-width | 4 | `BOOT:6434` |
| bus name | `sdhc2` | `BOOT:6439` |
| clocks | `iface_clk`, `core_clk` | `BOOT:6432-6433` |
| **cd-gpios** | **`<0xd2 0x08 0x01>` → PM8994 GPIO 8, active-low** | `BOOT:6456` |
| bus-speed-mode | SDR12/25/50, DDR50, SDR104 | `BOOT:6461` |
| **non-removable** | **absent ⇒ removable** | – |
| interrupt-map | `0x00 0x01 0x00 0x7d 0x00 0x01 0x01 0x00 0xdd 0x00 0x02 0xd2 0x08 0x03` | `BOOT:6455` |

Cross-check **CONFIRMED**: `AOSP-extract:177-182` `&sdhc2 { … cd-gpios = <&pm8994_gpios 8 GPIO_ACTIVE_LOW>; status = "okay"; };`

### D.3 SDIO / WLAN host — `sdhc3` (non-removable)

**CONFIRMED** — `BOOT:6466-6496`:

| Property | Value | Where |
|---|---|---|
| node path | `/soc/sdhci@f9864900` | `BOOT:6466` |
| reg | `<0xf9864900 0x11c 0xf9864000 0x800>` | `BOOT:6468` |
| interrupts | `<0x00 0x7f 0x00 0x00 0xe0 0x00>` → hc_irq **SPI 127** (`0x7f`), pwr_irq **SPI 224** (`0xe0`) | `BOOT:6470` |
| interrupt-names | `hc_irq`, `pwr_irq` | `BOOT:6471` |
| bus-width | 4 | `BOOT:6475` |
| bus name | `sdhc3` | `BOOT:6479` |
| **cd-gpios** | **`<0x34 0x71 0x00>` → TLMM GPIO 113**, active-high | `BOOT:6490` |
| **non-removable** | **yes** | `BOOT:6495` |
| bus-speed-mode | SDR12/25/50, DDR50, SDR104 | `BOOT:6494` |
| qcom,dat1-mpm-int | `<0x2f>` | `BOOT:6484` |

### D.4 `qcom,sdc-*` configuration nodes — NOT FOUND

**NOT FOUND.** Searched
`grep -n "sdc-eom\|sdc-quirks\|sdc-config\|mmc-\|eom\|quirk" BOOT`
→ the only matches are pinctrl labels (`mas-sdcc-1..4` at `BOOT:2905-2934`,
`slv-sdcc-1..4` at `BOOT:2962-3004`, `pmx_sdc1/2/3_{clk,cmd,data,rclk}` at
`BOOT:11427-11620`) and `qcom,msm-bus,name = "sdhcN"`. **There are no
`qcom,sdc-eom`, `qcom,sdc-quirks`, or similar nodes in this DTB.**

### D.5 Pinmux for the SD hosts

**CONFIRMED** — eMMC sdc1 pins `BOOT:11427-11510` (`sdc`, `pmx_sdc1_clk`,
`pmx_sdc1_cmd`, `pmx_sdc1_data`, `pmx_sdc1_rclk`), SD sdc2 `BOOT:11512-11571`,
SDIO sdc3 `BOOT:11572-11620`. eMMC pinctrl handles are
`pinctrl-0 = <0x11e 0x11f 0x120 0x121>` / `pinctrl-1 = <0x122 0x123 0x124 0x125>`
(`BOOT:6419-6420`); SD slot `pinctrl-0 = <0x129 0x12a 0x12b>` (`BOOT:6458`);
SDIO `pinctrl-0 = <0x12f 0x130 0x131>` (`BOOT:6492`).

---

## E. Clocks / CPUs / MMIO basics

### E.1 Root node

**CONFIRMED** — `BOOT:1-12`:

```
/dts-v1/;

/memreserve/	0x0000000000000000 0x0000000000001000;
/ {
	#address-cells = <0x02>;
	#size-cells = <0x02>;
	model = "Qualcomm Technologies, Inc. MSM8992 P1 GLOBAL COM";
	compatible = "qcom,msm8992-cdp", "qcom,msm8992", "qcom,cdp";
	qcom,msm-id = <0xfb 0x00 0xfc 0x00>;
	qcom,pmic-id = <0x10009 0x1000a 0x00 0x00>;
	interrupt-parent = <0x01>;
	qcom,board-id = <0xb64 0x00>;
```

| Property | Value | Where |
|---|---|---|
| model | "Qualcomm Technologies, Inc. MSM8992 P1 GLOBAL COM" | `BOOT:7` |
| compatible | `qcom,msm8992-cdp`, `qcom,msm8992`, `qcom,cdp` | `BOOT:8` |
| `qcom,msm-id` | `<0xfb 0x00 0xfc 0x00>` → 251, 252 | `BOOT:9` |
| `qcom,pmic-id` | `<0x10009 0x1000a 0x00 0x00>` | `BOOT:10` |
| `qcom,board-id` | `<0xb64 0x00>` = **2916** | `BOOT:12` |
| root `interrupt-parent` | `<0x01>` → the GIC | `BOOT:11` |
| root `#address-cells` / `#size-cells` | 2 / 2 | `BOOT:5-6` |
| root `ranges` | **absent** (root is not a bus) | – |
| `/memreserve/` | `0x0` len `0x1000` | `BOOT:3` |

**CONFIRMED — `soc` node** `BOOT:232-235`:

```
	soc {
		…
		ranges = <0x00 0x00 0x00 0xffffffff>;
```

The `soc` child bus maps **the whole 32-bit space** (base 0, size `0xffffffff`).

### E.2 CPUs

**CONFIRMED** — `BOOT:16-50` (cpu-map) and `BOOT:51-…` (cpu nodes):

| cluster | cores | `cpu` reg values | `BOOT` lines |
|---|---|---|---|
| cluster0 (A53) | core0..core3 | 0x02, 0x03, 0x04, 0x05 | 22-47 |
| cluster1 (A53) | core0..core1 | 0x06, 0x07 | 36-48 |

* `cpu@0` — `BOOT:51-85`: `compatible = "arm,cortex-a53"` (`BOOT:53`),
  `reg = <0x00>` (`54`), **`enable-method = "qcom,8994-arm-cortex-acc"`** (`55`),
  `qcom,acc = <0x08>`, `qcom,ldo = <0x09>`, `next-level-cache = <0x0a>`.
* 6 CPU nodes total, all `arm,cortex-a53`.
* **No `clocks` property and no `operating-points` on any CPU node** in the boot
  DTB — searched `grep -n "operating-points\|clocks = " BOOT` in the cpu range:
  no hits within lines 16-230.

**⇒ CPU clock node: NOT FOUND in the boot DTB.**
The only clock controllers present are the three Qualcomm CC blocks:

| node | `BOOT` line | compatible | reg | phandle |
|---|---|---|---|---|
| `qcom,gcc@fc400000` | 3850 | `qcom,gcc-8992` | `0xfc400000` / `0x2000` (3852) | `0xa4` (3858) |
| `qcom,rpmcc@fc401880` | 3862 | `qcom,rpmcc-8992` | `0xfc401880` / `0x04` (3864) | `0x46` (3867) |
| `qcom,mmsscc@fd8c0000` | 3871 | `qcom,mmsscc-8992` | `0xfd8c0000` / `0x5200` (3873) | `0x18` (3881) |

Plus per-subsystem `clock-controller@f9070000` … `f9075000` (`BOOT:3562-3600`),
`clock-controller@f900d000` (`BOOT:3652`), `clock-controller@f900f000` (`BOOT:3660`).

**INFERRED:** CPU frequency must be driven through the **RPM** (`qcom,rpmcc`) or
SMD-RPM, not through a DT clock node. The AOSP reference confirms the direction:
`AOSP-extract:75-92` sets `enable-method = "spin-table"` on `&cpu0`…`&cpu5`
and reserves `spin-table@6000000` (`AOSP-extract:29-32`).

### E.3 GIC (interrupt controller)

**CONFIRMED** — `BOOT:3484-3491`:

```
		interrupt-controller@f9000000 {
			compatible = "qcom,msm-qgic2";
			interrupt-controller;
			#interrupt-cells = <0x03>;
			reg = <0xf9000000 0x1000 0xf9002000 0x1000>;
			linux,phandle = <0x01>;
			phandle = <0x01>;
		};
```

| Property | Value | Where |
|---|---|---|
| node path | `/soc/interrupt-controller@f9000000` | `BOOT:3484` |
| compatible | `qcom,msm-qgic2` (Qualcomm GIC-2 wrapper around GIC-400) | `BOOT:3485` |
| **base / size** | **`0xf9000000` / `0x1000`** | `BOOT:3488` |
| **second window** | **`0xf9002000` / `0x1000`** | `BOOT:3488` |
| #interrupt-cells | **3** → `<type, irq, flags>` | `BOOT:3487` |
| phandle | `0x01` = root `interrupt-parent` | `BOOT:3489-3490`, `BOOT:11` |

### E.4 Timer

**CONFIRMED** — `BOOT:3493-3497`:

```
		timer {
			compatible = "arm,armv8-timer";
			interrupts = <0x01 0x02 0xf08 0x01 0x03 0xf08 0x01 0x04 0xf08 0x01 0x01 0xf08>;
			clock-frequency = <0x124f800>;
		};
```

| Property | Value | Where |
|---|---|---|
| node path | `/soc/timer` | `BOOT:3493` |
| compatible | **`arm,armv8-timer`** (not `arm,arch_timer`) | `BOOT:3494` |
| interrupts | 4 × 3 cells, all **type `0x01` = GIC PPI**, IRQs **2, 3, 4, 1**, flags `0xf08` | `BOOT:3495` |
| interrupt-parent | inherited = `0x01` (the GIC) | `BOOT:11` |
| **clock-frequency** | **`0x124f800` = 19 200 000 Hz (19.2 MHz)** | `BOOT:3496` |

**INFERRED:** the ARM generic timer tick frequency is **19.2 MHz**; the four PPIs
are the standard ARMv8 arch-timer interrupts (PPI 1 = non-secure physical timer,
PPI 2 = hypervisor physical timer, PPI 3 = virtual timer, PPI 4 = reserved/deferred).

Also present:

* `BOOT:3499-3503` `qcom,mpm2-sleep-counter@fc4a3000`, compatible
  `qcom,mpm2-sleep-counter`, reg `0xfc4a3000`/`0x1000`, `clock-frequency = <0x8000>` (32 kHz).
* `BOOT:3505-3511` `timer@f9020000`, compatible `arm,armv7-timer-mem`,
  reg `0xf9020000`/`0x1000`, `clock-frequency = <0x124f800>`.
  Frame 0 `BOOT:3513-3517`: `frame@f9021000`, `interrupts = <0x00 0x09 0x04 0x00 0x08 0x04>`
  (**GIC SPI 9 and SPI 8**), `reg = <0xf9021000 0x1000 0xf9022000 0x1000>`.
  Frames 1-7 (`BOOT:3519-3560`) are `status = "disabled"`.

### E.5 Memory node and reserved regions

**CONFIRMED** — `BOOT:13285-13379`:

```
	memory {
		device_type = "memory";
		reg = <0x00 0x00 0x00 0x00>;
		#address-cells = <0x02>;
		#size-cells = <0x02>;
```

* `reg = <0x0 0x0 0x0 0x0>` — base 0, size 0: **filled in by the bootloader**
  (`BOOT:13287`).

**CONFIRMED** reserved regions (legacy Qualcomm layout **inside `/memory`**, *not*
a standard `/reserved-memory` node):

| node | `BOOT` line | base | size | flags |
|---|---|---|---|---|
| `secure_region@0` | 13291 | `0x0` | `0x9600000` (150 MiB) | `linux,reserve-contiguous-region`; label `secure_mem` |
| `qsecom_region@0` | 13299 | `0x0` | `0x400000` (4 MiB) | `secure_mem`-style; label `qseecom_mem` |
| `audio_region@0` | 13307 | `0x0` | `0x400000` (4 MiB) | reserve-contiguous + reserve-region; label `audio_mem` |
| `removed_regions@0` | 13316 | `0x6400000` | `0xe00000` (14 MiB) | **remove-completely**; label `memory_hole` |
| `dfps_data_mem@0` | 13324 | `0x3400000` | `0x1000` | **`status = "disabled"`**; label `dfps_data_mem` |
| **`cont_splash_mem@0`** | **13334** | **`0x3400000`** | **`0xc00000` (12 MiB)** | reserve-contiguous + reserve-region; phandle `0x25`; label `cont_splash_mem` |
| `peripheral_region@0` | 13343 | `0x7400000` | `0x1c00000` (28 MiB) | **remove-completely**; label `peripheral_mem` |
| `modem_region@0` | 13353 | `0x9000000` | `0x6400000` (100 MiB) | **remove-completely**; label `modem_mem` |
| `ramoops_region@0` | 13363 | `0xff00000` | `0x100000` (1 MiB) | remove-completely; label `ramoops_mem` |
| `crash_fb_mem@4000000` | 13373 | `0x4000000` | `0xc00000` (12 MiB) | reserve-contiguous + reserve-region; label `crash_fb_mem` |

**`/reserved-memory` (standard) is NOT FOUND** — searched
`grep -n "reserved-memory" BOOT` → no hits. The AOSP reference uses the modern
form (`AOSP-extract:25-49`: `reserved-memory { … spin-table@6000000, ramoops@ff00000,
cont_splash_mem: fb@3400000, crash_fb_mem: crash-fb@4000000 }`) and additionally
does `/delete-node/ &cont_splash_mem;` (`AOSP-extract:13`) to *remove* the
legacy entry — i.e. upstream re-declares the same physical region.

### E.6 PSCI

**NOT FOUND (and known-broken).**

* **CONFIRMED:** `grep -rn "psci" KDT/` → **zero hits** in the entire
  `arch/arm64/boot/dts` tree of the LGE "Titan" kernel. There is **no
  `arm,psci-0.2` node**, and no `psci` node of any kind.
* **CONFIRMED:** the only PSCI references in the whole kernel are the drivers:
  `arch/arm/kernel/psci.c:316` and `arch/arm64/kernel/psci.c:338`
  (`{ .compatible = "arm,psci-0.2", .data = psci_0_2_init }`).
* **CONFIRMED (cross-check):** the AOSP reference explicitly disables it —
  `AOSP-extract:23-24`:

  ```
             	/* psci is broken */
             	/delete-node/ psci;
  ```

  i.e. AOSP `kernel/common` *does* have a `psci` node in `msm8992.dtsi`, but it
  **does not work on the H815**, so the LG G4 port deletes it and uses
  **`enable-method = "spin-table"`** for all six cores (`AOSP-extract:75-92`).
* The boot DTB's CPU nodes instead use
  **`enable-method = "qcom,8994-arm-cortex-acc"`** (`BOOT:55` and the other five
  CPU nodes) — the Qualcomm ACC (Application Processor Core Controller) handoff.

**⇒ INFERRED (XNU implication):** do **not** assume PSCI/SMC-based CPU bring-up
works on this device. Plan for either (a) all cores already running, (b) the
Qualcomm ACC mailbox handshake, or (c) a spin-table at `0x0600_0000`
(AOSP reserves `spin-table@6000000`, 4 KiB). This needs live validation.

### E.7 Other MMIO of interest

| peripheral | base | `BOOT` line |
|---|---|---|
| TLMM GPIO | `0xfd510000` / `0x4000` | 11194 |
| MDSS MDP | `0xfd900000` / `0x90000` | 761 |
| MDSS VBIF | `0xfd9b0000` / `0x4d4` | 761 |
| MMSS clock controller | `0xfd8c0000` / `0x5200` | 3873 |
| DSI0 ctrl / phy | `0xfd994000` / `0xfd994500` | 1627 |
| DSI1 ctrl / phy | `0xfd996000` / `0xfd996500` | 1746 |
| DSI0 PLL | `0xfd994300` / `0x500` | 616 |
| DSI1 PLL | `0xfd996300` / `0x500` | 667 |
| HDMI PLL | `0xfd9a0600` | 711 |
| Global clock controller | `0xfc400000` / `0x2000` | 3852 |
| RPM clock controller | `0xfc401880` / `0x04` | 3864 |
| SPMI arbiter (core) | `0xfc4cf000` / `0x1000` | 3992 |
| SPMI arbiter (intr) | `0xfc4cb000` / `0x1000` | 3992 |
| SPMI arbiter (cnfg) | `0xfc4ca000` / `0x1000` | 3992 |
| GIC | `0xf9000000` / `0xf9002000` | 3488 |
| ARMv7 timer (mem) | `0xf9020000` / `0x1000` | 3510 |
| MPM2 sleep counter | `0xfc4a3000` / `0x1000` | 3501 |
| eMMC SDHCI | `0xf9824900` / `0xf9824000` | 6394 |
| SD SDHCI | `0xf98a4900` / `0xf98a4000` | 6428 |
| SDIO SDHCI | `0xf9864900` / `0xf9864000` | 6468 |
| I²C (touch) QUP2 | `0xf9924000` / `0x1000` | 5498 |

---

## F. Summary of what is MISSING (and how to get it)

| Needed by XNU | Status | Where to get it |
|---|---|---|
| `simple-framebuffer` node | **NOT FOUND** in any H815 DTB | none — the display is a raw MDP/DSI command-mode pipeline; XNU needs a real MDP driver, or a shim that renders into the `cont_splash_mem` pool |
| framebuffer physical address | **INFERRED** `0x0340_0000` (12 MiB pool) | read the MDP buffer descriptors at runtime, or dump the bootloader splash |
| framebuffer pixel format / stride | **NOT FOUND** | runtime; `qcom,mdss-dsi-bpp = 0x18` only gives the 24-bpp DSI wire format |
| touch controller identity | **UNRESOLVED** (Synaptics `0x20` vs LG SIC `0x28`) | probe both I²C addresses at runtime |
| PSCI | **NOT FOUND / known broken** | use ACC or spin-table |
| CPU clock node | **NOT FOUND** | use RPM / SMD-RPM |

## G. Method / reproducibility

* Primary reading used `grep -n` on `BOOT` for every claim; every `file:line`
  above is a real line in that file.
* `dtc -I dtb -O dts` was run on all four `aicp/dtb_estratto/*.dtb` to confirm
  the supplied `.dts` matches the binary DTBs (only the board-id / status
  differences noted in §0.1).
* Cross-checks against the LGE "Titan" kernel (`KDT`, `KDRV`) were used to
  confirm IRQ numbers, key codes, GPIO numbering and ABS_MT ranges.
* The AOSP `kernel/common` H815 file was used **only** as a secondary cross-check
  (§0.2 explains why it is a saved HTML page and no longer exists upstream).
* No files were modified other than this report.
