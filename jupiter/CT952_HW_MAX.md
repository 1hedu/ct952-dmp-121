# CT952 Hardware Maximization Report

*In the JupiterSDK tradition: what this silicon can actually do, divined
from the firmware source. Every claim cites `file:line` in this repo and
is tagged **[V]** (verified — explicit in source) or **[I]** (inferred —
deduced from strong evidence). Register-level drivers for some blocks
live in closed `.a` libraries, but the `ctkav_*.h` headers document the
registers themselves.*

**The chip:** CheerTek CT909P family (this board: CT952 / DMP_121 build,
`platform.h:27` sets `CT909P_IC_SYSTEM` [V]). A 2007-era DVD-player SoC
that turns out to contain: two SPARC CPU cores, a 2D blitter, a
four-plane display compositor with hardware scalers and raster
interrupts, hardware JPEG (progressive!) and MPEG-1/2 decode with a
memory-injection door, a JPU scale/rotate engine, and an audio DSP with
hot-swappable codec firmware and FFT readback.

---

## 1. CPU: dual-core LEON2-class SPARC V8

- **PROC1 (main CPU)**: SPARC V8, register windows, **133 MHz stock**
  (`CPU_133M`, internal.h:305), runtime-settable via
  `HAL_ClockSet(MODE_MPLL, ...)` from a menu of
  27/54/100/120/133/142/146/**162 MHz** (internal.h:296-303,
  spflash.c:102-165) [V]. Overclock to 162 MHz is a register write away
  [V]; thermal/stability headroom unknown [I].
- **PROC2 (audio DSP)**: a *second full SPARC core* with its own cache,
  IRQ bank, DSU, and boot registers (`PROC2_STARTADR`/`PROC2_SP` via
  GR22/GR21, hdecoder.c:702-703) [V]. Runs hot-swappable codec firmware
  from DRAM (§8). **It is reprogrammable**: the loader path
  (`HAL_ReloadAudioDecoder`, hdecoder.c:616) will boot any SPARC image
  placed at `DS_PROC2_STARTADDR` [V mechanism / I for custom code].
- Caches: **4-way 2 KB I + 2 KB D, 16-byte lines** (ctkav_platform.h:686)
  [V]. Uncached windows at `+0x80000000` (I/O) and `0xC0000000` (D-cache
  bypass) (ctkav_platform.h:682-683) [V].
- **Hardware integer MUL/DIV** (SPARC V8 UMUL/UDIV used inline,
  ctkav_platform.h:697-701); **no FPU** — everything is `-msoft-float`
  (Makefile:16) [V]. Fixed-point or die.
- GRMON-compatible **DSU debug unit** at `0x90000000`: full memory
  read/write, breakpoints, trace buffer, register file access over a
  dedicated DSU-UART (ctkav_platform.h:10,579-593) [V]. **This is the
  free hardware-hacking interface** — peek/poke/single-step any address
  with a serial cable, no exploit needed.
- Timers: LEON prescaler tuned to a **1 µs tick** (hsystem.c:42); 2 ms
  system tick; Timer3 readable for time-tagging; watchdog [V].
- Power management: per-block clock gates and resets for every
  peripheral (ctkav_platform.h:267-375); CPU can downshift to 27 MHz [V].

**CPU budget reality [I]:** at 133 MHz with 2 KB caches, expect roughly
1–2 cycles/px for cached tight loops but heavy DRAM-miss penalties on
big framebuffers. A full-software 256×224 renderer (like the jupiter/
NES port) is feasible at 20–60 fps; the point of this report is that
you should be *offloading* instead (§2, §4, §6).

## 2. The 2D GPU (hardware blitter)

Base `0x2880` (ctkav_gpu.h:21), physically part of the VPU [V].
Fixed-function block-mover with three primitive classes
(gdi.c:96-108) [V]:

| Op | Opcode | Notes |
|---|---|---|
| Solid rectangle fill | 6 (+ **7 = HP**) | into 2/4/8bpp OSD-format buffers |
| Bitmap blit | 4 (+ **5 = HP**) | **color-key transparency** (CTL0[8], key value CTL1[23:16]), **mirror** (CTL0[6]) |
| Glyph expansion | 0/1/2 | 1bpp/2bpp glyphs → 4-color mapped text, 14 glyphs/op FIFO |

- Max op size **1023×1023 px** (gdi.c:1294-1295) [V]; strides 4-byte
  aligned; sources/destinations are DRAM addresses — it will happily
  blit between *any* buffers in the OSD indexed formats [V].
- **Unused performance levers** [V-exists/I-gain]: the `_HP`
  ("higher-performance") opcode variants are never used by stock
  firmware (gdi.c uses 4/6 everywhere); DRAM burst thresholds are set to
  4 of max 7 (gdi.c:1369, fields at ctkav_gpu.h:53-54). Free speed on
  the table.
- Completion: IRQ exists (`GPU_INT_EN` → `INT_PROC1_2ND_VPU`) but stock
  driver **busy-waits** (gdi.c:3208-3234). A demo can go async:
  kick a blit, do CPU work, service the VPU interrupt [V mechanism].
- **Hardware beam-race gating**: `GPU_OP_JUDGE_EN` (CTL0[11]) +
  `REG_GPU_OP_THRE` makes the fill wait until the display scanline is
  outside the target region — tear-free drawing without vsync waits
  (gdi.c:1345-1369) [V].

## 3. Display engine: a four-plane hardware compositor

Base `0x1A00` (ctkav_disp.h:20). The compositing stack, all hardware
(disp.h:91-96, ctkav_spu.h) [V]:

```
  [4] SP2 subpicture   2bpp, 16-level per-color alpha, own scale/position
  [3] SP1 subpicture   2bpp, 16-level per-color alpha, own scale/position
  [2] OSD plane        2/4/8bpp, 256-color YUV palette, 64-level plane
                       alpha + per-color mix bits + chroma key
  [1] Main video       4× YCbCr 4:2:0 framebuffers, HW H/V scalers,
                       gamma RAM, page-flip by register
  [0] Background       solid color register (REG_DISP_MAIN_BG)
```

plus **2 hardware highlight rectangles per SP plane** (solid-color
boxes) and an **OSD brightness sub-window** ("spotlight" rect,
REG_DISP_BRK_OSD_*) [V].

Key display facts:
- **Hardware scalers**: independent V-scale, H-upscale, H-downscale on
  the video window, nearest or phase-interpolated
  (ctkav_disp.h:85-102) [V]. Zoom/letterbox/pan-scan are just crop+scale
  register programs (hal.c:2084-2191) [V].
- **Programmable timing generator**: H/V totals, sync widths, window
  position/size, per-line DRAM request count all RISC-writable
  (ctkav_disp.h) — custom raster timings possible [V].
- **Raster interrupts**: `REG_DISP_N_HSYNC_INT` (every-N-hsync IRQ),
  a fixed every-16-lines IRQ (`INT_16L`, interrupt.c:176-180), VSYNC/
  HSYNC/SCREEN_END/OSD_END IRQs, and a **readable current-scanline
  register** (`REG_DISP_MEM_LINE`) [V]. Split-screen, per-band palette
  swaps, and beam-raced effects are all reachable.
- **Gamma RAM** on Y and C with per-frame reprogramming
  (ctkav_disp.h:131-134) — hardware fades/flashes/color grading [V].
- `REG_DISP_N_LINE_REPEAT`: hardware "repeat after N lines" — cheap
  vertical stretch / kill-scroll effects [V].
- Progressive output: **480p/576p** (`DISP_PSCAN_EN`), plus a
  **VGA-timing progressive mode** (hdevice.c:890-942) [V]. No HD [V].

## 4. The OSD plane in detail

- One OSD plane live at a time; the 4-5 "regions" are software
  descriptors time-multiplexed onto it (gdi.c:669-685) [V]. Want more
  simultaneous layers? Use the SP planes (§5).
- Geometry: up to **616×440 (NTSC) / 616×540 (PAL)** at 8bpp
  (gdi.h:383-385); pitch = width, linear bytes (gdi.c:1109) [V].
- **Palette: 256 × YUV entries with a per-entry translucency bit**
  (`bMixEnable` → palette bit 24, gdi.c:897-898) layered under a
  **global 64-level OSD↔video mix ratio** (gdi.c:760-783) [V]. Entry 0 =
  transparent color key; an additional OSD chroma-key enable exists
  (`DISP_OSD_T_EN`, ctkav_disp.h:128) [V].
- Palette rewrites **latch at VSYNC** (gdi.c:914-943) — full-palette
  color cycling at field rate is safe [V].
- **OSD hardware pixel-doubling**: ×2 horizontal (`DISP_OSD_H2X`) and
  vertical scale (`DISP_OSD_VS_EN`) — a 308×220 buffer can fill the
  screen for kilobyte-cheap framebuffers (ctkav_disp.h:120-122) [V].
- Positioning at 1-px granularity via `GDI_MoveRegion` → hardware
  window position — **whole-plane smooth scrolling for free** [V].

## 5. Subpicture planes: two free overlay layers

The DVD subpicture unit (base `0x1900`) has a **bitmap mode that
bypasses RLE decode**: `SPU_BMP_EN` + `SPU_FREE_RUN` displays a raw
2bpp DRAM bitmap as a translucent overlay (ctkav_spu.h:126-128) [V].
The firmware already exploits this for DivX subtitles, pointing an SP
plane at a GDI-rendered buffer (char_subpict.c:1716-1729) [V]:

- 2 independent planes (SP1, SP2), each: 2bpp pixels, 16-entry YUV
  color RAM, **16-level per-color contrast (= alpha)**, hardware
  position/clip/**scale**, per-field control, pan offsets [V].
- Plus 2 solid-color highlight rectangles per plane [V].

**[I] Maximization:** SP1+SP2 as parallax/sprite/HUD layers over the
video-plane game canvas — with per-color alpha, that's hardware-blended
translucent clouds, fog, scanline gradients, dialogue boxes... The
compositing is free; the CPU only writes the 2bpp buffers.

## 6. Video/JPEG codec silicon

Unified VLD → DEQ/IDCT → MC datapath shared by MPEG and JPEG
(ctkav_vdec.h:83-120) [V], plus the **JPU** scale/rotate engine.

- **MPEG-1/2 decode to full D1** (720×480/576) into the tiled YUV
  framebuffers [V]. DivX/MPEG-4 exists in the family but is fused by
  IC part number (CT909G parts; ctkav_platform.h:868-872) — likely
  absent on this CT909P [I].
- **The FMV door [V]**: `HAL_FillVideoBuffer(mode, ptr, size)`
  (hdecoder.c:262-301) copies an arbitrary caller-supplied elementary
  stream into the video bitstream ring and points the hardware VLD at
  it — this is how the boot logo (an MPEG still) is decoded from
  memory. **A game can stream its own MPEG video from USB/SD through
  the hardware decoder as a cutscene/pre-rendered-background engine.**
- **JPEG decoder**: dedicated hardware on this part (REG_JPG VLD +
  DEQ + JPU; ctkav_jpg.h) [V]. Baseline, extended-sequential, and
  **progressive** JPEG; 420/422/444/440/400 chroma [V]. Oversized
  images use a **DC-only fast path** (1px per 8×8 block) for instant
  previews (jpegfmt.h:75-77) [V].
- **JPU** (base `0x2880`): hardware YUV420 **arbitrary scale**
  (16-bit fixed-point factors, interpolated or nearest), **8-way
  rotate/flip**, per-plane color fill, de-flicker, up to 4095×4095 per
  op (ctkav_jpu.h) [V]. Stock firmware uses it for thumbnails (15-20
  per page), 31 slideshow transition effects, photo zoom (25–200%),
  and the 9-frame video digest [V].
- **JPEG encoder**: software libjpeg-6b on PROC1 (encoder.a,
  jpegenc.c:13) [V] — quality ~90, writes to SPI flash (15 slots ×
  64 KB). It reads **arbitrary Y/UV framebuffer addresses**, and the
  digest feature proves live decoded-video frames can be captured and
  JPU-scaled (digest.c:75-131) [V] — so **video frame grab → JPEG** is
  a supported hardware path with a software back end [I for the combo].

**[I] Maximization:** treat JPEG as your texture/asset codec (store art
compressed in flash/USB, hardware-decode + JPU-scale at load or even
per-scene), and treat the MPEG decoder as a free FMV/background layer
under your OSD/SP game layers.

## 7. The video framebuffer as a game canvas

`GDI_FBDrawDot` (gdi.c:3532-3594) documents CPU access to the video
plane [V]:

- **Format: block-tiled YUV 4:2:0**, separate Y and C buffers —
  *not linear*. 909P swizzle:
  `Ybyte = (y>>4)*strip + (x>>3)*128 + (y%16)*8 + (x%8)`;
  chroma interleaved U/V at quarter res (gdi.c:3581-3582) [V].
- Practical canvas **704×480** (FBDrawDot clamp, gdi.c:3601-3602),
  buffers sized for 720×576 [V].
- **4 hardware framebuffers (F0-F3)** with page-flip by writing
  `REG_DISP_FnY/C_ADDR` + display-select — double/triple buffering is
  free (ctkav_disp.h:71-80) [V].
- Scan-out passes through the hardware scalers + gamma — render small,
  scale up for free [V].

**[I] Maximization:** a full-color (8-bit luma + 4:2:0 chroma) 704×480
double-buffered canvas — much richer than the 256-color OSD — at the
cost of doing the tile swizzle in your blitter (or rendering in
16×8-px tiles that map 1:1 onto the swizzle blocks, which is exactly
what console-style tile renderers do anyway).

## 8. Audio: a second CPU with a mailbox

- **PROC2 runs hot-swappable SPARC codec firmware** decompressed from
  flash ROM sections (`"MPPC"`/`"ACDT"`/`"WMA "`/`"AAC "`) into DRAM at
  `0x40002000`, booted via reset + start-address registers, commanded
  through the AM mailbox (`HAL_WriteAM`/`ReadAM`, 200 ms ACK,
  hdecoder.c:537-754,1728) [V].
- Codecs: MP3/MP2(+LSF), WMA(+L1), AC3, DTS, LPCM, PCM, HDCD, AAC [V].
- Output: **8–192 kHz** (15 rates), **16/20/24-bit**, **5.1 + stereo
  downmix**, SPDIF raw/PCM, 9 supported external DAC chips + internal
  DAC (hadac.c/h) [V].
- DSP post-processing, all controllable from PROC1: **Dolby Pro Logic
  II**, 5 surround presets, **16-band EQ**, DRC, bass management,
  **karaoke pitch shift ±7 semitones**, **9-level echo/reverb**, vocal
  cancel, per-channel gains [V].
- **Microphone input**: dedicated AIU mic ADC FIFO with DSP mixing and
  level detection (hdecoder.c:686,829) [V] — an *audio input device*
  (SKU-gated by `NO_MIC_INPUT`).
- **16-band spectrum readback**: the DSP publishes 16 × 4-bit live
  spectrum values through the EQ mailbox registers; the stock music UI
  draws a spectrum analyzer from them (osdmm.c:4335-4389) [V]. Free
  music-visualizer / beat-detection data.
- Buffering: ~108 KB compressed-audio ring + 52 KB PCM ring with
  threshold/credit refill (mm_play.c:3074-3156) [V].

**[I] Maximization tiers:**
1. *Use as shipped*: MP3/WMA music playback costs the main CPU almost
   nothing; layer game SFX via the raw-PCM path; read the spectrum for
   visuals; abuse pitch-shift/echo as live DSP effects on your music.
2. *The moonshot*: PROC2 is a SPARC core with a documented boot path —
   a custom DSP image (e.g. the jupiter jaudio mixer + streaming) could
   run there, freeing PROC1 entirely. Needs the DSP-side AIU/FIFO
   contract reverse-engineered from the `.sym` files (symbols like
   `int_mcu_pcmbuf_empty`, `send_to_pcm_0/1/2` map the surface) [I].

## 9. TV encoder & outputs

- Outputs: **CVBS, S-Video, YPbPr component, RGB (SCART), VGA-timed
  progressive**; NTSC/PAL/PAL-M/PAL-N; 7.5/0 IRE (hdevice.c:511-942)
  [V]. 480i/480p/576i/576p only — no HD [V].
- Hardware **closed-caption and WSS (line-21/VBI) insertion** with
  per-field data registers (ctkav_tve.h:22-38) [V] — you can inject
  arbitrary VBI data. Macrovision registers present [V].

## 10. I/O, storage, connectivity

| Interface | Capability | Evidence |
|---|---|---|
| USB | **Dual-role host + device**, OHCI (USB 1.1 full-speed [I]), mass-storage both directions | usbsrc.c:123,502-1355 [V] |
| SD/MMC | Hardware controller, **DMA**, up to 8-bit MMC bus | ctkav_sdc.h:16-64 [V] |
| Memory Stick | Hardware controller + DMA | ctkav_msc.h:60-69 [V] |
| NAND | Controller with **ECC** + IRQ | ctkav_nfc.h:9-20 [V] |
| SPI NOR flash | 4–32 Mbit, XIP, standard command set | spflash.c:429-525 [V] |
| GPIO | Up to 9 ports (A–H + expander), set/clear/dir regs, 3 GPIO IRQs | pio.h:19-30 [V] |
| UARTs | 2 × LEON UART + 2 × DSU debug UARTs | ctkav_platform.h:40-97 [V] |
| IR | Hardware timer-capture decoder (HW-NEC) + SW protocols, own IRQ + 27 MHz clock | input.c:14-85 [V] |
| I²C | Bit-banged over GPIO (tuner, RF, RTC, DACs) | radiodrv.c:189-216 [V] |
| FM radio | **Si4703 tuner with RDS** | radiodrv.c:237-425 [V] |
| RTC | External battery-backed serial RTC; alarm/auto-power scheduling | rtcdrv.c:15-117 [V] |
| Front panel | VFD controller (3-wire), LEDs, key-scan readback | vfd_ctrl.h:5-52 [V] |
| ATAPI | Full optical-drive command set incl. CSS key exchange (compiled out in this DMP build) | atapi.h:36-47 [V] |

Memory in *this* build: **2 MB DRAM** (`DRAM_SIZE_16` = 16 Mbit,
span 0x200000, dvd_dram_16m.h:22) + **2 MB SPI flash** (16 Mbit,
internal.h:86); family supports 4/8 MB DRAM and up to 4 MB flash [V].
Code XIP from flash with a DRAM overlay for ISRs/flash-writers
(DVD909.ld:25-49) [V].

## 11. The maximization playbook

Ranked by return-on-effort, combining everything above:

1. **Claim all four planes.** Game world on the video plane (704×480
   full-color YUV, page-flipped, hardware-scaled), HUD/text on the OSD
   (256-color, per-color translucency), parallax/effects on SP1+SP2
   (2bpp, 16-level alpha, hardware scale/position). The compositor is
   free; today's jupiter port uses one plane. [V planes / I design]
2. **Move fills/blits to the GPU.** `jdraw`-style rect fills and
   color-keyed sprite blits are exactly opcodes 6/4 — with the HP
   variants and burst-threshold 7 that stock firmware never uses, plus
   async completion via the VPU IRQ and beam-race gating for tear-free
   single-buffer rendering. [V]
3. **Raster tricks.** Every-N-hsync IRQ + VSYNC-latched full-palette
   rewrites + readable beam position + gamma RAM = split screens,
   >256-colors-per-frame, sky gradients, hardware fades — the classic
   demo toolkit, all present. [V]
4. **FMV and JPEG assets.** Stream MPEG elementary streams from USB/SD
   into `HAL_FillVideoBuffer` for hardware-decoded cutscenes and
   backgrounds under your overlay layers; store textures/screens as
   (even progressive) JPEG and hardware-decode + JPU-scale/rotate them
   on load. [V doors / I integration]
5. **Let PROC2 be the sound chip.** Zero-CPU music via the shipped
   codecs + spectrum readback for reactive visuals + pitch/echo as
   live effects; long-term, boot custom SPARC DSP code on it. [V/I]
6. **JPU as second blitter.** Arbitrary-ratio scaling and 8-way
   rotation of YUV tiles — sprite scaling/rotation the 2D GPU can't
   do, proven by the digest/effects code. [V block / I use]
7. **Inputs beyond the remote**: mic (audio-reactive/karaoke games),
   front-panel keys, GPIO headers for custom controllers (9 ports —
   the JupiterSDK NES/SNES/Genesis pad bit-bang protocols would port
   directly), FM/RDS data as an entropy/companion channel. [V/I]
8. **162 MHz.** `HAL_ClockSet` one call. Validate stability first. [V/I]
9. **Develop over GRMON.** The DSU gives load/peek/poke/breakpoints
   over serial — a real debug workflow without reflashing. [V]

## 12. Honest limits

- **2 MB DRAM in this build** is the tightest constraint — the AV
  buffer map is already carefully partitioned; a game borrowing the
  decode buffers cannot simultaneously run full MPEG playback in
  anything but carefully budgeted slices [V map / I implication].
- No alpha *blitting* in the 2D GPU (color-key only); all blending
  happens at scan-out between planes [V].
- One OSD plane; extra layers must come from SP planes (2bpp) or the
  video plane [V].
- SD-only output; interlace unless PSCAN/VGA modes are engaged [V].
- No FPU; soft-float is poison — stay integer/fixed-point [V].
- USB is full-speed (OHCI): ~1 MB/s practical streaming ceiling [I].
- Closed blobs: display.a/tve.a/decoder.a register drivers and the
  PROC2 codec images are binary-only; register headers + .sym symbol
  maps are the documentation [V].
- Everything here is source-divined; none of it has been re-validated
  on powered hardware in this effort [V].
