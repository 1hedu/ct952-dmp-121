# ct952emu — bring-up journal

A from-scratch full-machine emulator for the CT909P/CT952 SoC, in the
spirit of the Jupiter SDK's `licheeEmu`: not just the CPU but the
device fabric, so the stock ROM can eventually run on a host PC.

This is honest, iterative hardware bring-up. Each entry records what
works, what's blocked, and exactly what the next step is — the
emulator's own I/O-access inventory is the worklist generator.

## Status

| Layer | State |
|---|---|
| SPARC V8 CPU core | **Verified** — full integer ISA (windows, traps, delay slots, mul/div, atomics, ASI), result bit-identical to native GCC on a stress payload (`test_sparc`) |
| Memory map | flash XIP @ 0, DRAM @ 0x40000000 (+0xC0000000 alias), I/O @ 0x80000000, DSU/FCR stubs |
| Core devices | timers 1/2/3 + prescaler + watchdog, UART1/2/DSU (tx→host), LEON interrupt controller, PROC2 mailbox + boot-handshake auto-ACK |
| Unmodeled I/O | store/readback register file with an access log (the worklist) |
| **ROM section loader** | **Working** (`--rom-load`) — parses the section table, copies raw sections, and **decompresses zipped sections by running the firmware's own codec in-emulator** (`machine_call`). The "closed `gz909`/`UNZIP2006` codec" is no longer a blocker. |
| **Stock firmware boot** | **Boots into the main superloop** — a real device dump (`dp700wd.bin`) unpacks all 5 zipped sections, runs eCos HAL init, starts the PROC2 audio DSP, and reaches its watchdog-petting main loop. Parks in a table-driven register-config routine awaiting un-staged config data (see frontier below). |
| **SDK-on-emulator demo** | **Renders** (`make demo`) — real Jupiter code (`jnes` + draw + palette) cross-compiled to SPARC V8 and run on the emulated CPU draws a splash + live NES scene into the CT952 8bpp OSD plane; snapshotted from DRAM to `demo_splash.png`. |

## Breakthrough: the compression codec, cracked by execution

The one hard gate — the custom `UNZIP2006`/`gz909` codec that packs the
boot vectors, DRAM overlay, and data — is **present as plaintext SPARC
in the device flash** (the `UZIP` section, flash `0x2000`). Rather than
reverse-engineer the format, `ct952emu` **runs the firmware's own
decompressor inside the emulated CPU** via `machine_call()` (set
`%o0..%o2` = src/dst/workmem, run until it returns). The public entry is
the wrapper at `UZIP+0xc50`; it XOR-deobfuscates a 16-byte header with
`0x5a5a5a5a` (hence the `0x5A`-heavy compressed blobs), builds a
descriptor, and calls the core at `UZIP+0xb4`.

Verification: decompressing `ROMV` yields a textbook SPARC trap table
(`b reset; nop`; per-slot `rd %tbr; rd %psr; b handler`), and — the
stronger proof — the firmware then **executes** the decompressed vectors
and `.text_dram` overlay for tens of thousands of instructions,
correctly programming the eCos system-tick timer. Corrupt output could
not do that. The codec is effectively solved for all sections (same
algorithm).

`--rom-load` does the on-chip mask ROM's job: decompress `ROMV`→
`0x40000000`, `TEXT`→`0x4001d000`, `DATA`, `SFAT`, `ENGL`, seed the boot
trampoline registers (GR21/22) with the reset vector + a stack, and run.

## A display window: the SDK, running on the emulated CT952

`make demo` cross-compiles real JupiterSDK code (`tests/demo_splash.c` +
`jnes.c` + `jrgb2yuv.c`) for SPARC V8 and runs it *inside* ct952emu --
the same CPU + machine model that boots the stock ROM -- then snapshots
the OSD framebuffer out of emulated DRAM to a PPM (`demo_run.c`). The
result (`demo_splash.png`) is a genuine picture drawn by the ported SDK
executing on the emulated hardware: a colour-bar test card, a title band
in a built-in 5x7 font, a 64-step grey ramp, and the ported `jnes`
background renderer drawing a live 256x224 scene in true NES colours into
the CT952's native 8bpp palette-indexed OSD plane. No firmware, no libc
-- `start.S` sets the trap table + stack and calls `testmain()`; every
pixel is produced by SPARC instructions the interpreter executed.

This closes the arc: port the SDK -> build the emulator -> run the SDK on
the emulator -> see a display window.

## The display engine is modeled (real scanout)

`machine_scanout()` reproduces the CT952 OSD read-channel from the actual
registers any code programs -- `REG_MCU_VCR20` (0x80000D80, OSD base),
`REG_DISP_OSD_SIZE` (0x80001A54, bit28 enable), and the `GAM_OSD` palette
RAM (0x80001C00, 256 entries of `[23:0]=YCbCr`, BT.601 studio-swing) --
resolving the 8bpp plane through the palette and converting YCbCr->RGB
the way the hardware would. The demos now program those registers via
`tests/demo_de.h` (`de_program`, RGB->YCbCr into GAM_OSD), and `demo_run`
scans out through the DE instead of reading a side-channel palette. The
same path will render the *firmware's* screen the moment it reaches its
own OSD init -- no demo-specific assumptions.

## BREAKTHROUGH: correct chip ID -> the firmware self-boots

The stock first-stage boot (flash `0x310`) reads **`PSR[31:24]`** and only
runs its real clock/DRAM/PROM/section-load path when it sees **`0xA0`**
(the CT952 chip ID); any other value drops it to a debugger-style path.
The emulator had been reporting a LEON-ish `0xF3...`, so the true boot
never ran -- which is *why* `--rom-load` was needed and why the config
tables at `0x40042000`/`0x40046800` were always empty (the descriptor
builder is part of the real boot). This root cause was found by the
parallel `coby-dp722` effort; independently reproduced here.

Setting `sparc_reset` PSR to `0xA0000000` (impl=0xA, ver=0) makes the
firmware **boot itself with no `--rom-load`**: it programs the PROM
controller, decompresses every section, builds the config descriptors,
and runs the eCos app -- now programming the **DISP** timing/OSD
registers (`0x80001A38..A64`), the **Vipor scaler/TCON** panel block
(`0x80003xxx`), the **2-D GPU** (`0x80002880`, dest `0x4009E600`), and
reading **SAR-ADC** keys (`0x8000407C`). Backing `0xB0000000` as real
VDEC/USB scratch SRAM (not a 0/FF stub) lets the working-memory writes
there stick. CPU stays bit-exact.

The OSD plane stays blank for now only because the on-screen "Loading ."
is drawn by the **2-D GPU font path**, which this emulator does not model
yet (the parallel branch does) -- the pixels are issued, they just have
no engine to land in. Next: model the GPU blit/font engine so those ops
render into the OSD plane, then the modelled DISP scan-out shows them.

## Stock-firmware boot frontier

`./ct952emu dp700wd.bin --rom-load` now boots far past the old
`0x9210a308` wild jump. Fixes that moved the frontier this pass:

1. **SFAT/DATA overlap** -- DATA now loads last so its VSR dispatch table
   at `0x40020878` isn't clobbered by SFAT (was the `0x9210a308` crash).
2. **Hardware IIC/EEPROM master** (`0x80004200`, undocumented in the
   headers) -- the boot config read (flash `0x62538`) writes a command to
   `0x4210` and spins on the bit2 "busy" flag. The store/readback file
   left it stuck set; we self-clear it on read (transaction completes
   instantly), matching self-clearing trigger silicon.
3. **PROC2 audio-DSP boot ACK** (`0x800007e4`) -- the firmware writes
   `0x10003` and spins until bits [31:16] read **0** (hdecoder.c:724).
   The stand-in DSP now clears the high half instead of setting it.

With those, the firmware decompresses all sections, runs eCos HAL init,
starts the audio DSP, and reaches its **main superloop** (pets the
watchdog at `0x8000004c`, reads `SYSTEM_CONFIGURATION1` at `0x8000031c`
each pass). It currently parks in a table-driven register-config routine
(SFAT region, tight loop `0x4002050c..0x40020638`): a nested walk over
halfword count tables at `%i0/%i4/%l5` that programs engine registers via
a pointer table at `0x40046800`. The inner exit compares
`(%l4 & 0xff) < *(u16*)%i0`; with the current table contents that outer
count exceeds 255, so it can't terminate -- one of the config tables it
walks still holds a value that depends on state we haven't staged (a real
EEPROM image / earlier device init). The display engine (`0x80001A00`) is
not touched yet, so the firmware hasn't reached its own display init.

Pulling that thread further (this pass): the spin is the leaf function at
`0x400203e8` (called from flash `0x3d5ac`). It is a *leaf* -- no `save`,
so it runs in its caller's register window -- that walks a device table
and programs SPI-flash-controller shadow registers (`0x40046800[...]` and
`0x80002a28`/`0x80002a34` in the `0x80002a00` PROM/SPI block) per entry.
The inner walk indexes a runtime table at `0x40042000`; a DRAM write-watch
(`--watch`) shows that table is **only ever memset to zero** (by
`0x4001d1f0`) and never populated. With it empty, the per-entry "apply"
work is always skipped and the outer counter spins against a bogus bound.
The table lives in the DATA->ENGL BSS gap, so it is meant to be filled at
runtime -- by storage/EEPROM probing over the `0xA0000000` FCR/SDC/NFC
controller, which the model currently stubs to 0 (no media). So the next
real fix is to model that controller's media-detect (or reverse the three
caller frames that build the descriptor list) so the device table gets
populated. Tooling that landed to get here: `--dump-dram`, `--watch ADDR`
(DRAM write-watch with the issuing PC), the jmpl trace ring, a 64-deep
per-instruction PC ring, and a full window+global register dump at halt.

### Deeper trace (SPI flash modeled; spin root-caused to bad window inputs)

The serial-flash controller is now modeled (device-ID + status handshake:
0x90 read-ID returns 0xC214 = MX25L1605, matching the 2 MB dump; status
reads 0 = ready), so the firmware can detect its flash. That did **not**
move the spin -- flash detection runs elsewhere.

Capturing the spin leaf's registers on first entry (`--regs-at
0x400203e8`) pinned the real problem: the leaf reads `%i0 = 0x126` and
dereferences it (`lduh [%i0]`), but `*(u16*)0x126` is the *CLCK section's
size field* inside the flash section table (0x56b7) -- a stray pointer,
not a count. `%i4 = 0` (also dereferenced), and `%l4..%l7` are all
0x40042000. The call chain is 0x33ca4 -> 0x3d564 -> leaf(0x400203e8); the
leaf takes no `save`, so it runs in 0x3d564's window and consumes
registers (%i4, %l4-7) that 0x3d564 never initialises. The machine's
globals are otherwise sane (%g1/%g3/%g4 point at real DRAM structs), so
this is narrowly stale/uninitialised *window* state feeding the leaf --
either an upstream init that never ran, or a register-window-preservation
bug in the interpreter that the canonical-handler CPU test doesn't
exercise. Next: a targeted window-preservation stress test (does a callee
correctly see an outer frame's %i/%l across a window-trap-triggering
chain?) to rule the interpreter in or out before more RE.
Tool added: `--regs-at ADDR` (one-shot register capture on first PC hit).

### Definitive root cause: register-window thread context (needs a HW trace)

Walking the spin's saved-frame chain gives the full stack:
`0x4001ea40/60` (RTOS overlay) -> `0xadbc` -> `0x4176c` -> `0x42218` ->
`0xea74` -> `0x1b3d0` -> `0x33ca4` -> `0x3d564` -> leaf `0x400203e8`.

At `0x1b3cc` the argument is a **literal**: `mov 0x126, %o0` -- so `%i0 =
0x126` is a deliberate ID/index, not a stray pointer. And `0x400203e8` is
not a function entry: it is the **branch-delay slot** of a `be` inside
one large `.text_dram` routine (0x40020340..0x40020658). The call at
`0x3d5ac` jumps into the *middle* of that routine, which immediately
overwrites its passed argument (`lduh [%i4], %o0`) and runs on inherited
window registers -- `%i4/%i5/%l5..%l7` are the **current thread's
register-window context**, expected to be pre-established by whatever last
owned that physical window.

So the `.text_dram` overlay is a register-window-based RTOS, and the stall
is a *symptom*: the inherited context is wrong (`%i4 = 0` where a table
pointer belongs), which means an earlier execution-history divergence --
one device read, interrupt, or scheduling decision that branched
differently than real silicon -- left the thread's window state wrong.
The machine's globals are all sane and the CPU is bit-exact (windows
included), so this is not a local missing-fill or a decode bug; it is the
accumulated product of the whole boot.

Pinning the *first* divergence is not something static analysis can do
from here -- the state is a function of the entire run. The definitive
next step needs a **hardware reference**: a GRMON/DSU register+PC trace
from the real board at a known checkpoint, diffed against the emulator to
find the first instruction where they part. Everything up to that point
is proven (bit-exact CPU, eCos HAL, PROC2 audio DSP boot, SPI-flash ID,
200M instructions with no fault), and the display engine is already wired
to render the firmware's OSD the moment it is programmed.

### The older status (pre-dump), kept for context

## The CPU is proven

`test_sparc` loads a cross-compiled SPARC image (`tests/testprog.c`,
linked with a real trap table + canonical window overflow/underflow
handlers in `tests/start.S`) into the emulated machine and runs it. The
payload — deep recursion (forces window traps), umul/smul/udiv/sdiv +
remainders, every shift, signed/unsigned compares, annulled branches,
byte/half/word/double loads and stores, switch tables, 64-bit math —
returns a single 32-bit value. The **same C compiled natively** into the
harness must return the same value. It does: `0x77a79387`. One number,
two compilers, two architectures — any mis-executed instruction would
diverge it.

## Booting DVD909.rom: what happens

Running the stock ROM (`make boot`) executes cleanly — tens of millions
of instructions, **zero illegal instructions, no crashes** (`tbr=0`) —
then settles into a tight loop. The I/O inventory names the exact three
registers involved:

```
0x80000014   (CACHE_CONTROL)  written  N times  = 0x0063000f
0x800007d4   (PROC2_SP/GR21)  read     N times  = 0
0x800007d8   (PROC2_START/GR22) read   N times  = 0
```

Hand-decoding the loop (flash 0x46c–0x4b0) shows it is the **boot
trampoline**:

```
or   %g3, 0x3d4, %g2      ; %g2 = 0x800007d4  (PROC2_SP  / GR21)
ld   [%g2], %o2           ; %o2 = staged stack pointer
or   %g3, 0x3d8, %g3      ; %g3 = 0x800007d8  (PROC2_START / GR22)
ld   [%g3], %o1           ; %o1 = staged entry point
... write 0x0063000f to CACHE_CONTROL (enable I+D cache) ...
mov  %o2, %g1             ; g1 = SP
mov  %o1, %g2             ; g2 = entry
jmp  %g2                  ; jump to staged entry...
mov  %g1, %sp             ; ...with staged stack   (delay slot)
```

It reads the firmware's real entry point and stack **out of two GR
registers** and jumps there. The I/O log proves those registers are
only ever *read*, never *written* — so an **external agent stages them**
before this code runs. On real hardware that agent is the earlier
`dsu_boot` stage (`dsu_boot.rom` in the tree). With the emulator
returning 0, the trampoline jumps to 0 and re-runs itself forever.

Seeding the registers (`--seed-entry`/`--seed-sp`, mimicking dsu_boot)
advances past the trampoline immediately — proving the diagnosis — and
surfaces the *next* blocker: the code at flash `0x2000` (the `.text`
section) faults, because it references the vectors and the DRAM-code
overlay that live in **loaded** ROM sections not yet present.

## Why the ROM can't flat-boot: it's a packed image

`DVD909.rom` is not flat code. Offset 0x10 holds a **section table**
(`romld.h`: `ROMLD_SECTION_TABLE_ADDR = 0x10`, entries of
`{char name[4]; DWORD dwLMA; DWORD dwRMA; DWORD dwSize; ...}`). From the
header and `romcfg.txt` the sections are:

| Section | Name | Run addr | Zipped? | Notes |
|---|---|---|---|---|
| `.rom_vectors` | `ROMV` | 0x40000000 | **yes** | **program entry** (reset_vector) |
| `.text` | `TEX2` | flash 0x2000 | no | executed in place |
| `.text_dram` | `TEXT` | 0x4001D000 | **yes** | ISR/flash-writer overlay |
| `.rodata` | `RODA` | flash | no | |
| `.data` | `DATA` | ram | **yes** | |
| `.osdstr` | `Engl` | ram | **yes** | strings |

The earlier boot stage parses this table, **decompresses** the zipped
sections to their run addresses, seeds the trampoline registers with the
entry (`0x40000000`, the vectors) and a stack, and jumps. `dsu_boot`
does this on hardware; a debugger (GRMON) does it during development.

## Next steps (the remaining work, honestly)

1. **ROM section loader** — parse the table at 0x10, copy each section
   to its run address. Straightforward for the *unzipped* sections
   (`TEX2`, `RODA`).
2. **The decompressor is the hard blocker.** The entry vectors (`ROMV`),
   the DRAM overlay (`TEXT`) and `DATA` are all zip-flagged, compressed
   with CheerTek's custom `gz909`/`ZIP2006` codec. That algorithm is not
   in this source tree (it lives in the build tool and the closed
   `dsu_boot`), so those sections can't be unpacked from the C sources
   alone. Options, in order of tractability:
   - run `dsu_boot.rom` itself in the emulator (it does the unpack) —
     but it in turn expects its own pre-staged clock/DRAM state;
   - reverse-engineer `gz909` from a compressed/plaintext section pair
     (the tree has both build inputs and the packed output for some
     sections);
   - dump the post-unpack DRAM image once from real hardware over GRMON
     and load *that* — turning the emulator into a faithful runner even
     without the codec.
3. **More device models**, driven by the I/O inventory as each new
   blocker appears: the display engine (so a booted firmware renders to
   a host window), the clock/PLL status bits, GPIO, the front panel.

The point of this milestone: the CPU is trustworthy, the machine runs
real firmware without miscompiling a single instruction, and every
remaining blocker is now *named and located* rather than mysterious.

## Usage

```
cd jupiter/emu
make            # builds ct952emu, test_sparc, and the SPARC test image
make check      # CPU verification (bit-exact vs native)
make demo       # run the SDK splash on the emulator -> demo_splash.ppm
make boot       # run the stock ../../DVD909.rom, capture UART + I/O log

./ct952emu <flash.rom> [--instr N] [--uart FILE] [--iolog FILE]
           [--seed-entry ADDR] [--seed-sp ADDR] [--quiet]
```

Requires `gcc` for the host build and `sparc64-linux-gnu-gcc` (with
`-m32`) for the CPU test image.
