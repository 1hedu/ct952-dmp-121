# JupiterSDK on CT952

A port of the portable core of the [Jupiter SDK](https://github.com/1hedu/jupiter-lichee-zero)
(bare-metal SDK for the Lichee Pi Zero / Allwinner V3s) to this CheerTek
CT952/CT909 DVD-player firmware (eCos RTOS, big-endian SPARC/LEON,
`sparc-rtems-gcc`).

The two platforms could hardly be more different — little-endian ARM
Cortex-A7 with NEON and a hardware compositor versus a big-endian SPARC
DVD SoC with a palette-indexed OSD overlay — so this is a port of the
*portable brains* of the SDK onto the CT952's native services, not a
line-for-line copy.

## What was ported

| Jupiter SDK | Here | Notes |
|---|---|---|
| `lib/nes.c` NES PPU renderer | `jnes.c/h` | Re-targeted from ARGB8888 dual-plane to **8bpp palette-indexed** single-plane output (the CT952 OSD region format). Sprite behind-BG priority via a 1bpp opacity bitmap. Tile/nametable/attribute/OAM formats unchanged. |
| `lib/gb.c` GB/GBC PPU renderer | `jgb.c/h` | Same treatment; GBC per-tile attribute map supported. Palettes become OSD palette indices. |
| `lib/snes.c` SNES renderer | `jsnes.c/h` | All BG modes 0–7 (2/4/8bpp tiles, per-tile-column offset in modes 2/4/6), 128 sprites (32/line). **Mode 7** affine ground (with twist/vortex) reimplemented in plain C, replacing the NEON scanline + NEON line-double. Single-buffer layering: bottom BG opaque, upper BGs front-to-back first-opaque-wins. |
| `lib/genesis.c` Genesis VDP renderer | `jgen.c/h` | Planes A/B, per-scanline hscroll, window plane (truly *replaces* Plane A in its rect, matching the DE2-overlay semantics), multi-tile column-major sprites. CRAM becomes 64 OSD palette indices. |
| `lib/audio.c` (portable half) | `jaudio.c/h` | PCM mixer w/ fractional resampling, NES/GB-style APU (2 pulse + wave + noise, envelopes), Genesis-style 6ch×4op FM + PSG. Restructured from a DMA ring-buffer to a **pull model** (`jaudio_render()` fills a mono int16 buffer). Integer-only (`-msoft-float` target). |
| `draw_rect` / `sprite_blit` (NEON) | `jdraw.h` | Plain-C 8bpp fill/clear/color-keyed blit with clipping. |
| platform layer (`video_/input_/timer_`) | `jshim.h` + `jshim_ct952.c` | See mapping below. |
| — | `jrgb2yuv.c/h` | New: ARGB8888 → `0x00YYUUVV` BT.601 (the OSD palette is **YUV, not RGB**). |
| `template/game.c` demo pattern | `japp.c/h` | Demo app on the superloop: color bars, NES scene (scroll + sprites + APU jingle), GB scene, Genesis scene (two-plane parallax + window HUD + sprites), SNES Mode 7 flight. |

## Platform mapping

| V3s / Jupiter SDK | CT952 firmware |
|---|---|
| DE2 mixer, VI0+UI0 ARGB layers | GDI OSD **region 0**, 8bpp indexed, 616×440, buffer at `DS_OSDFRAME_ST_MM` (pitch = width, 1 byte/px — verified against `gdi.c` addressing) |
| Hardware palette (implicit ARGB) | 256-entry OSD palette in **YUV** via `GDI_ChangePALEntry` (`jrgb2yuv` converts) |
| Codec + DMA ping-pong @48 kHz | HAL raw-PCM pipe @44.1 kHz (the `HAL_PlayTone` mechanism): copy PCM to `DS_AD0BUF_ST_MM`, `HAL_AM_AUDIO_TYPE=7`, `PLAY_COMMAND=1` |
| GPIO-bit-banged NES/SNES/Genesis/N64 pads | IR remote / front panel keys → `JBTN_*` masks (`jinp_map_key`) |
| `while(1)` game loop + vblank | Cooperative superloop: `JUPITER_ProcessKey` in the `FuncArray` dispatch table + `JUPITER_Trigger` per loop iteration (the `osdgame` pattern from `cc.c`), paced by `OS_GetSysTimer()` |

## Using it

Build with `SUPPORT_JUPITER = 1` (default) in the top-level `Makefile`;
it compiles `jupiter/*.c` into `OBJS/` and defines `-DSUPPORT_JUPITER`
for the firmware. Set to `0` for a byte-identical stock build.

On the player, press the **GAME** key (`KEY_OSDGAME` — already mapped on
several remotes in `ir.h`; free in this build because `SUPPORT_OSDGAME`
is compiled out) while no disc is playing. LEFT/RIGHT switch scenes,
ENTER replays the jingle, STOP or GAME exits back to the normal UI.

To write your own app: draw into `jvid_fb()` (616×440 bytes, palette
indices), load colors with `jvid_load_palette()`, render audio with
`jaudio_render()` + `jsnd_play()`, and follow `japp.c` for the
ProcessKey/Trigger integration. The renderers accept the same asset
formats as the Jupiter SDK originals, so CHR/nametable/OAM data moves
over unchanged.

## Verification

`test/run_tests.sh` (needs `gcc`, `sparc64-linux-gnu-gcc`, `qemu-user`):

1. Builds and runs renderer + audio tests **natively** (little-endian).
2. Builds them as static SPARC binaries and runs them under
   **qemu-sparc64 (big-endian)** — frame and audio CRC32s must match the
   native run bit-for-bit. They do, proving the ported code is
   endian-clean (the CT952 is big-endian; the SDK was written for
   little-endian ARM).
3. Compile-checks every firmware-facing file with
   `sparc64-linux-gnu-gcc -m32 -mcpu=v8 -msoft-float` (the CT952's ABI:
   ILP32, big-endian, soft float) against the **real firmware headers**,
   with minimal eCos stubs (`test/ecos_stub/`) standing in for the
   absent eCos install. All pass.

The tests also dump `out_nes.ppm` / `out_gb.ppm` (rendered frames) and
`out_audio.wav` (APU jingle, wave/noise, FM algorithms, PSG+PCM) for
inspection.

## What was NOT ported, and why

- **CedarVE H.264, `cedar_*` examples** — V3s hardware codec; no
  equivalent here.
- **NEON asm (`sprite_neon.S`, `mode7_neon.S`, `tiles_neon.S`,
  `genesis_asm.S`), `mmu.c`, `irq.c`, `mem.c`** — ARM-specific;
  functionality replaced by `jdraw.h`/plain C where it mattered.
- **Nuked-SC55, Munt MT-32** — C++, multi-MB ROMs, and far beyond this
  CPU's real-time budget (also empty submodules in the SDK checkout).
- **libvgm player + cycle-accurate cores (Nuked-OPN2 etc.)** — portable
  and endian-safe; a good follow-up, but the lightweight `jaudio` synths
  cover the demo, and Nuked-OPN2 is likely too heavy for this CPU anyway.
- **War1/Stratagus, guisan, Lua** — C++17 engine stack; out of scope.
- **GPIO controllers, MIDI UART, SDMMC/FatFs** — replaced by IR input
  and the firmware's own storage stack.

## Known limitations / unverified-on-hardware

Everything that can be verified without the real device has been (see
above). What has NOT been exercised on hardware:

- The OSD bring-up sequence in `jvid_open()` (region config, mix ratio,
  activation) follows the firmware's own idiom (`osdnd.c`/`osdmm.c`)
  but has not run on a CT952.
- `HAL_AM_ABUF0_ADR/LEN` opcode values (0x1F/0x20) are **inferred** from
  the unassigned gap in `hdecoder.h` — the authoritative values live in
  the eCos install headers that aren't in this tree. The defines are
  `#ifndef`-guarded so the real build's values win if present.
- Audio is one-shot (jingles/SFX) via the tone path; continuous
  streaming would need the PCM ring-buffer refill protocol, which is
  undocumented here.
- Frame rate: the NES scene renders 256×224 = 57K pixels/frame in
  software; on a ~100–200 MHz LEON expect the ~30 fps target to degrade
  gracefully (the pacing logic tolerates it).
- The full firmware still cannot be rebuilt in a stock environment (the
  `sparc-rtems` + eCos toolchain and several vendor headers are not in
  the repo; note also the tree's Windows-era case-insensitive
  `#include`s, e.g. `"winav.h"` vs `Winav.h`).
