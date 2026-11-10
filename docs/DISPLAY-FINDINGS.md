# Display (G4 H815): findings from the live kernel, 2026-11-10

Evidence: `docs/display-2026-11-10.txt`, captured with
`tools/read-mdp-registers.sh` from TWRP (the kernel's own MDP is clocked,
so every register reads a live value). Read-only: debugfs only.

## The pipe that scans the panel is RGB2 on INTF2

- `RGB2` = pipe index 5 (MDSS_MDP_SSPP_RGB2), base 0xFD919000.
- Register 0x19000 (SRC_SIZE) = 0x0A0005A0 → 1440×2560.
- 0x19004 (SRC_IMG_SIZE) = 0x0A0005A0 (no crop).
- 0x19008 (SRC_XY) = 0.
- 0x1900C (OUT_SIZE) = 0x0A0005A0 (no scaling).
- 0x19014 (SRC0_ADDR) = 0x00E30000 — **this is an IOVA**, see below.
- 0x19024 (SRC_YSTRIDE0) = 0x00001680 = 5760 (= 1440 × 4).
- 0x19030 (SRC_FORMAT) = 0x000236FF, 0x19034 (UNPACK) = 0x03010002,
  format = RGBA8888, op_mode = 0 (no BWC).
- VIG0/VIG1/VIG2 counters are all 0; RGB0 and RGB1 show 2 each (one-shot
  overlay usage), RGB2 shows the play count (27). **RGB2 is the only pipe
  that drives the panel.**

## The display goes through the MDP IOMMU — the plan's "no SMMU" is wrong

- `mdss_fb` allocates the framebuffer from the ION **system** heap
  (`/d/ion/clients/mdss_fb_iclient-0`, size 0x1C20000 = 2 × 1440×2560×4)
  and calls `ion_map_iommu(...)`; the driver refuses to register fb0 without
  an IOMMU domain ("No IOMMU Domain" is a fatal error in `mdss_fb_alloc_fb_buf`).
- Therefore `SRC0_ADDR = 0x00E30000` is an **IOVA** on the MDP SMMU
  (device `mdp_iommu` @ 0xFD9B4000, context bank `mdp_0` @ 0xFD9BC000,
  SID 0), not a physical address.
- The MDP IOMMU is attached on the first commit (`mdss_iommu_ctrl(1)` in
  `mdss_mdp_intf_cmd.c`), i.e. only after the continuous splash has handed
  off. LK therefore runs the panel with the IOMMU bypassed and physical
  addresses — and that is the state XNU inherits.
- Consequence for the XNU port: either program context bank 0 of the MDP
  SMMU with an identity map for the framebuffer (page table + TTBR0 +
  SCTLR enable), or keep the IOMMU globally bypassed and use physical
  addresses, which is the simpler path and matches what LK does. The
  ios-topaz note "no SMMU on G4, use 0x20000000" is only true for the
  bypass path.

## INTF2 is in DSI command mode

- INTF2 base 0xFD96C000. TIMING_ENGINE_EN (0x6C000) = 0 (command mode).
- PANEL_FORMAT (0x6C090) = 0x0000213F, FRAME_LINE_COUNT_EN (0x6C0A8) =
  0x0000213F, border/underflow colours as expected.
- DSI0 link id 0x10007E57, DSI1 0x10009D57 (dual link).
- MDP clock 265.06 MHz, final bandwidth 1.21 GB/s, no underruns.

## CTL0

- 0x2000 (CTL_TOP) = 0x01008000.
- 0x2014 (CTL_TOP) = 0x00020020.
- 0x2018 (CTL_FLUSH) = 0x40000018 (write-strobe; the flush bits are
  BIT(17) for CTL plus BIT(pipe->num) for RGB/VIG 0-2, i.e. BIT(5) for
  RGB2, accumulated in software and written once per commit).
- The ios-topaz value 0x20001 does **not** apply verbatim: this kernel
  accumulates the CTL and pipe bits, and flushes with BIT(30) set.

## What is still needed before writing to the display

1. The DCS command sequence for the JDI NT36850 panel (reset sequence,
   init DCS, sleep out, display on) is **not** in this debugfs output; it
   comes from
   `G4-Titan-Kernel-titan_bpf2/.../dsi-panel-jdi-nt36850-dualmipi0-qhd-cmd.dtsi`
   (`qcom,mdss-dsi-reset-sequence`, `qcom,mdss-dsi-panel-timings`).
2. The clock sequence for MMSSCC (MDP core/bus/iface/vsync + DSI PLL) is in
   the LK binary (`dsi_pll_20nm_enable_seq`); disassemble it rather than
   guessing.
3. Decide bypass vs. program the SMMU; bypass is the lower-risk first attempt.
