/*
 * display-data.h -- SINGLE SOURCE OF TRUTH for the LG G4 (H815) display.
 *
 * Everything measured on the phone or read from its own kernel/DTS lives
 * here. display.c includes this file; nothing below is guessed. When a new
 * measurement is taken, update THIS file first, then the code. Do not
 * re-derive these numbers from docs or from memory.
 *
 * Sources:
 *   K  = /Users/io/Dev/ios/lg g4/G4-Titan-Kernel-titan_bpf2
 *   DT = /Users/io/Dev/ios/lg g4/msm8992-lg-h815-boot.dts
 *   P  = dsi-panel-jdi-nt36850-dualmipi{0,1}-qhd-cmd.dtsi (K/arch/arm64/boot/dts/lge)
 *   B  = BOOT dump in the pstore log of 2026-11-16 (D4/D11 runs)
 *   R  = docs/display-2026-11-10.txt (live kernel register dump)
 *   T  = measured timing from the pstore of 2026-11-16
 *
 * Version is compiled into the binary and printed as "DATA id=" in pstore,
 * so a log can always be paired with the exact data set that produced it.
 */
#pragma once
#include <stdint.h>

#define DISPLAY_DATA_ID		0x20261163UL	/* 2026-11-16 rev 3 */
#define DISPLAY_DATA_ID_STR	"2026-11-16.3"

/* ------------------------------------------------------------------ */
/* 1. PANEL                                                            */
/* ------------------------------------------------------------------ */
/* P: qcom,mdss-dsi-panel-type = "dsi_cmd_mode". COMMAND MODE, so the
 * panel has its own GRAM and self-refreshes. Writing DDR alone never
 * changes the glass: a frame must be PUSHED over DSI. */
#define DD_PANEL_NAME		"JDI NT36850 (INCELL 1440p Dual)"
#define DD_PANEL_W		1440		/* physical panel */
#define DD_PANEL_H		2560
#define DD_PANEL_BPP		24		/* P: qcom,mdss-dsi-bpp */
#define DD_PANEL_PIXFMT_DCS	0x77		/* DCS 0x3A, 24bpp */
#define DD_PANEL_FPS		60		/* P: panel-framerate */
#define DD_PANEL_CMD_MODE	1
#define DD_PANEL_HAS_GRAM	1

/* P: TWO controllers, each qcom,mdss-dsi-panel-width = <720>,
 * height = <2560>. Every DCS/pixel command MUST go to BOTH links.
 * K mdss_dsi_host.c:1769-1781 broadcasts the same dma_addr to mctrl. */
#define DD_DSI_LINKS		2
#define DD_LINK_W		720
#define DD_LINK_H		2560
#define DD_LINK_BYTES_PER_ROW	(DD_LINK_W * 3)		/* 2160 */
#define DD_LINK_FRAME_BYTES	((uintptr_t)DD_LINK_W * DD_LINK_H * 3) /* 5,529,600 */

/* The MDP mixer output is 1440 wide (B: LM0.outsz = 0x0A0005A0), split
 * into two 720-wide DSI links. The inherited framebuffer is therefore a
 * SINGLE 1440x2560 RGB888 image with a 4320-byte stride, NOT two 720-wide
 * images. This is the single most misread number in this project. */
#define DD_FB_W			1440
#define DD_FB_H			2560
#define DD_FB_STRIDE		(DD_FB_W * 3)		/* 4320 = 0x10E0 */
#define DD_FB_BYTES		((uintptr_t)DD_FB_STRIDE * DD_FB_H) /* 11,059,200 = 0xA8C000 */
#define DD_FB_BASE		0x03400000UL		/* cont_splash_mem, B: RGB0/1.addr */
#define DD_FB_SIZE		0x00C00000UL		/* 12 MiB, B: FB size=00C00000 */
#define DD_FB_FITS		(DD_FB_BYTES <= DD_FB_SIZE)	/* 0xA8C000 < 0xC00000 */

/* Shim scratch framebuffer, plain DRAM, never touched by MDP. */
#define DD_SCRATCH_BASE		0x20000000UL		/* G4_DISP_FB_BASE */
#define DD_SCRATCH_SIZE		0x00C00000UL

/* DSI DMA command buffer (DRAM read by the DSI host). */
#define DD_DSI_CMD_BUF		0x50010000UL
#define DD_DSI_CMD_BUF_SIZE	0x00010000UL		/* 64 KiB */
#define DD_DCS_WC_MAX		0x0ffffUL		/* 16-bit word count */
#define DD_DCS_MAX_PAYLOAD	(DD_DSI_CMD_BUF_SIZE - 4UL)

/* ------------------------------------------------------------------ */
/* 2. REGISTER MAP (phone map = kernel tree map + 4)                  */
/* ------------------------------------------------------------------ */
#define DD_MAP_SHIFT		4
#define DD_MDP_BASE		0xfd900000UL		/* DT: qcom,mdss_mdp */
#define DD_DSI0_BASE		0xfd994000UL		/* DT: qcom,mdss_dsi */
#define DD_DSI1_BASE		0xfd996000UL
#define DD_DSI_PHY_BASE		0xfd998000UL
#define DD_DSI_PLL_BASE		0xfd994300UL
#define DD_MMSS_CC_BASE		0xfd8c0000UL

#define DD_CTL_OFF		0x00002000UL
#define DD_RGB0_OFF		0x00015000UL
#define DD_RGB1_OFF		0x00017000UL
#define DD_RGB2_OFF		0x00019000UL
#define DD_LM0_OFF		0x00045000UL
#define DD_INTF1_OFF		0x0006b000UL
#define DD_INTF2_OFF		0x0006c000UL
#define DD_PP0_OFF		0x00071000UL

#define DD_INTF_FRAME_COUNT	0x000acUL		/* K: mdss_mdp_hwio.h:531 */
#define DD_PP_TEAR_CONFIG	0x00000UL
#define DD_PP_AUTOREFRESH	0x00030UL		/* K: mdss_mdp_hwio.h:578 */

/* DSI controller offsets, tree form; add DD_MAP_SHIFT for the phone. */
#define DD_DSI_CTRL		0x0000
#define DD_DSI_STATUS		0x0004
#define DD_DSI_FIFO_STATUS	0x0008
#define DD_DSI_ACK_ERR		0x0064
#define DD_DSI_RDBK_DATA0	0x0068
#define DD_DSI_DMA_CTRL		0x0038		/* phone: 0x003c */
#define DD_DSI_MDP_CTRL		0x003c		/* phone: 0x0040 */
#define DD_DSI_DCS_CMD_CTRL	0x0040		/* phone: 0x0044 */
#define DD_DSI_DMA_CMD_OFFSET	0x0044		/* phone: 0x0048 */
#define DD_DSI_DMA_CMD_LENGTH	0x0048		/* phone: 0x004c */
#define DD_DSI_STREAM0_CTRL	0x0054		/* phone: 0x0058 */
#define DD_DSI_STREAM0_TOTAL	0x0058		/* phone: 0x005c */
#define DD_DSI_TRIG_CTRL	0x0080		/* phone: 0x0084 */
#define DD_DSI_DMA_SW_TRIGGER	0x008c		/* phone: 0x0090 */
#define DD_DSI_MDP_SW_TRIGGER	0x0090		/* phone: 0x0094 */
#define DD_DSI_LANE_CTRL	0x00a8
#define DD_DSI_CLKOUT_TIMING	0x00c0
#define DD_DSI_EOT_PKT_CTRL	0x00c8
#define DD_DSI_ERR_INT_MASK0	0x0108
#define DD_DSI_INT_CTRL		0x010c		/* phone: 0x0110 = DSI_INTR_CTRL status */
#define DD_DSI_INT_CTRL_DONE	0x00000001UL	/* CMD_DMA_DONE  (dsi_host_v2.h:27) */
#define DD_DSI_INT_CTRL_MDP_DONE 0x00000100UL	/* CMD_MDP_DONE  (dsi_host_v2.h:25) */
#define DD_DSI_INTR_ALL_MASK	0x02220202UL	/* dsi_host_v2.h:28 = 0x2220202 */
#define DD_DSI_DMA_CTRL_LP	(1UL << 26)	/* K: dsi_set_tx_power_mode() */

/* ------------------------------------------------------------------ */
/* 3. STATE INHERITED FROM LK (BOOT dump, 2026-11-16)                 */
/* ------------------------------------------------------------------ */
/* The pipeline is ALIVE and CORRECTLY CONFIGURED, just FROZEN. Do not
 * rebuild MDP: point it at a frame and kick it. */
#define DD_BOOT_DSI_CTRL	0x000001F5UL	/* both links: DSI_EN|CMD_MODE|CLK_EN */
#define DD_BOOT_DSI_CLKCTL	0x0000023FUL
#define DD_BOOT_DSI_TRIG	0x00000004UL
#define DD_BOOT_DSI_MDPCTL	0x00000008UL
#define DD_BOOT_DSI_DCSCTL	0x00013C2CUL
#define DD_BOOT_DSI_STR0_CTRL	0x08710039UL
#define DD_BOOT_DSI_STR0_TOTAL	0x0A0002D0UL	/* 2560 lines x 720 */
#define DD_BOOT_DSI_DMACTL	0x14000000UL	/* LP mode; clear BIT(26) for HS */
#define DD_BOOT_DSI_DMACTL_HS	0x10000000UL
#define DD_BOOT_DSI_STATUS	0x00000000UL
#define DD_BOOT_DSI_INTCTL	0x00000000UL

#define DD_BOOT_RGB_SIZE	0x0A0002D0UL	/* h=2560, w=720 (per link) */
#define DD_BOOT_RGB_FMT		0x0002243FUL	/* RGB888, LK's SSPP format */
#define DD_BOOT_RGB_ADDR	0x03400000UL
#define DD_BOOT_RGB_STRIDE	0x000010E0UL	/* 4320 = 1440*3 */
#define DD_BOOT_RGB2_ALL	0x00000000UL	/* kernel pipe is OFF in LK */
#define DD_BOOT_LM0_OUTSZ	0x0A0005A0UL	/* h=2560, w=1440 */
#define DD_BOOT_CTL_TOP		0x00021F20UL
#define DD_BOOT_CTL_FLUSH	0x40000000UL	/* PENDING IS NORMAL, not a failure */
#define DD_BOOT_CTL_LAYER	0x00001200UL
#define DD_BOOT_INTF1_PFMT	0x00002100UL
#define DD_BOOT_INTF2_PFMT	0x0000213FUL
#define DD_BOOT_INTF_TGEN	0x00000000UL	/* command mode: timing engine off */
#define DD_BOOT_PP0_TEAR	0x00000000UL
#define DD_BOOT_PP0_AUTORF	0x00000000UL
#define DD_BOOT_INTF_FRMC	0x00000000UL	/* frozen: no frames delivered */

/* Kernel working values (R), for comparison only -- NOT LK's handover. */
#define DD_KERNEL_RGB2_ADDR	0x00E30000UL	/* IOVA on MDP SMMU, not physical */
#define DD_KERNEL_RGB2_STRIDE	5760		/* 1440 x 4, ARGB8888 */
#define DD_KERNEL_TRIG_CTRL	0x80000004UL	/* te_sel set */
#define DD_KERNEL_FLUSH_IDLE	0x40000018UL	/* stable idle with pending bits */

/* ------------------------------------------------------------------ */
/* 4. DCS COMMANDS: WHAT WORKS AND WHAT DOES NOT                      */
/* ------------------------------------------------------------------ */
#define DD_DCS_SLEEP_OUT	0x11
#define DD_DCS_DISPLAY_ON	0x29
#define DD_DCS_DISPLAY_OFF	0x28		/* D6: screen went BLACK => link alive */
#define DD_DCS_SET_PIXFMT	0x3A
#define DD_DCS_COL_ADDR		0x2A
#define DD_DCS_PAGE_ADDR	0x2B
#define DD_DCS_WRITE_MEM	0x2C
#define DD_DCS_WRITE_CONT	0x3C
#define DD_DCS_READ_MEM		0x2E

/* Verified 2026-11-16:
 *  - 0x28 blanked the panel for 20 s: DCS commands REACH the panel.
 *  - 0x2C + 0x3C with a 720-column window filled the whole panel red,
 *    top-to-bottom, left-to-right: GRAM is WRITABLE over DSI.
 *  - 0x2A 0..639 with 640*3 bytes/row on a 720-wide panel displaced
 *    every row by 240 bytes: the deterministic "random pixels".
 *  - 0x2B SUB-WINDOWS ARE IGNORED BY THIS PANEL. D12 set 0x2B y..y+29
 *    before every 0x2C and ALL packets landed at row 0. Only a FULL
 *    0..2559 page window is honoured. Never use narrow page windows.
 *  - 0x3C continuation carries GRAM pointer state between packets; a
 *    lost/inserted byte shifts all following rows (row shear in D11).
 *  - CMD_DMA_DONE is HOST-SIDE ONLY: bit set means the DMA engine
 *    finished, NOT that the panel accepted the bytes. ACK_ERR_STATUS
 *    was 0 on both links in every successful run.
 */
#define DD_DCS_WINDOW_FULL_X1	719
#define DD_DCS_WINDOW_FULL_Y1	2559

/* ------------------------------------------------------------------ */
/* 5. MEASURED TIMING (pstore, 2026-11-16)                            */
/* ------------------------------------------------------------------ */
/* D11: 86 packets, 5,529,600 CPU-copied bytes -> visible progressive
 * fill over several seconds. BAND (D12): 20 packets, 1,296,000 bytes,
 * 292 ms total (~14.6 ms/packet) with both links polled. The per-packet
 * CPU byte copy into DD_DSI_CMD_BUF is the bottleneck, not the DSI lane. */
#define DD_T_FULL_PACKETS	86
#define DD_T_FULL_BYTES		5529600UL
#define DD_T_BAND_PACKETS	20
#define DD_T_BAND_BYTES		1296000UL
#define DD_T_BAND_MS		292UL
/* D13: one hardware MDP frame (whole 1440x2560 RGB888 framebuffer) needs
 * 12 ms from trigger to CMD_MDP_DONE on both links, with ZERO CPU bytes
 * copied per frame. 292 ms for 1.3 MB vs 12 ms for 11.1 MB. */
#define DD_T_HW_FRAME_MS	12UL

/* ------------------------------------------------------------------ */
/* 6. CPU PATH vs HARDWARE (MDP) PATH                                */
/* ------------------------------------------------------------------ */
/* CPU PATH (modes 4..12): the CPU copies every pixel byte from DRAM into
 * DD_DSI_CMD_BUF, then strobes DD_DSI_DMA_SW_TRIGGER. The DSI DMA engine
 * moves the bytes, but the CPU is on the critical path for EVERY byte and
 * for EVERY packet boundary. Slow, and any lost byte shears rows.
 *
 * HARDWARE PATH (mode 13): the CPU writes ONE frame into DD_FB_BASE in
 * the geometry LK already programmed (DD_FB_W x DD_FB_H, stride
 * DD_FB_STRIDE) and issues ONE DD_DSI_MDP_SW_TRIGGER. The MDP (Mobile
 * Display Processor, MDSS hardware) + the DSI MDP stream engine read the
 * framebuffer by DMA and push the frame with hardware-generated timing.
 * No CPU byte loop, no per-packet state, no shear, no progressive fill.
 *
 * NOTE ON "GPU": there is NO Adreno GPU driver in a bare-metal shim. The
 * engine that moves the frame is the MDP/DSI DMA (display hardware), not
 * the 3D GPU and not the CPU. Calling it "GPU" is a shorthand for
 * hardware scan-out. Adreno is not involved at any point.
 *
 * PROOF, not assertion. WRONG PROOF (tried 2026-11-16, D13 first run):
 * DD_INTF_FRAME_COUNT (0x0ac) stayed 0 even though the frame appeared
 * INSTANTLY. In command mode INTF_TIMING_ENGINE_EN = 0, so that counter
 * counts timing-engine (video-mode) frames only. It is NOT a valid
 * command-mode frame counter. Never use it as the verdict again.
 *
 * CORRECT PROOF: DSI_INTR_CTRL CMD_MDP_DONE = BIT(8)
 * (K/drivers/video/msm/mdss/dsi_host_v2.h:24-25). Clear BIT(8) on both
 * links, strobe MDP_SW_TRIGGER, poll BIT(8). Set => the DSI host's MDP
 * DMA engine completed a frame. That is the hardware, not the CPU.
 * CMD_DMA_DONE = BIT(0) is the CPU/DCS command path, a DIFFERENT bit.
 *
 *   MDP_DONE set on both links => verdict=HW, CPU bytes/frame = 0
 *   MDP_DONE never set          => verdict=CPU, DCS byte path required
 *
 * Supporting evidence: one MDP frame is instant; the CPU/DCS path needed
 * 292 ms for 1,296,000 bytes (20 packets) and several seconds for the
 * full 5,529,600-byte frame.
 *
 * MEASURED 2026-11-16 (D13, build_id 0x26ea8bdf):
 *   mode 0: MDP_SW_TRIGGER alone            -> MDP_DONE timeout 200 ms
 *   mode 1: CTL_FLUSH=0x40000000 + CTL_START=1 + MDP_SW_TRIGGER
 *                                          -> MDP_DONE=3 in 12 ms  <-- USE
 *   mode 2: full pipe flush + CTL_START     -> not needed
 *   every STEP and HELLO frame afterwards  -> MDP_DONE=3 in 12 ms
 *   CPU bytes per frame = 0
 *   user: "BOOT DIAGNOSTICS" appeared correctly and INSTANTLY
 * So the hardware path is CONFIRMED. The working kick is mode 1:
 * strobe CTL_FLUSH with the value LK left pending, set CTL_START, then
 * MDP_SW_TRIGGER on both links. Do not skip CTL_START.
 */
#define DD_GPU_KICK_MODE	1
#define DD_GPU_MDP_DONE_MS	12
#define DD_GPU_FRAME_COUNT_OFF	DD_INTF_FRAME_COUNT	/* informational only */
#define DD_GPU_ENGINE_NAME	"MDP/DSI DMA (MDSS hardware)"

/* ------------------------------------------------------------------ */
/* 7. SAFETY (never relax)                                           */
/* ------------------------------------------------------------------ */
/* Clock MMSS core BEFORE any MDSS/DSI register access. A write to a
 * controller with its clock off never completes and bricks the phone
 * until reflash (happened twice, 2026-11-09). Only `fastboot boot`,
 * never `fastboot flash boot`. All device writes through mmio_wr32()
 * allowlist (tools/check-mmio-writes.py). Watchdog armed before display
 * work; LK_RECOVERY_MODE set before reset. */
#define DD_SAFE_FASTBOOT_BOOT_ONLY	1
#define DD_SAFE_WDT_MS			60000UL
