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
| **DISP display engine** | **Modelled** (`make disp`) — the DISP OSD scan-out is now a real device: writes to the GAM_OSD palette RAM (`0x80001C00`) and `REG_DISP_OSD_SIZE` are honoured, and `machine_disp_scanout` composites the 8bpp OSD plane through the BT.601 palette to an RGB PPM (`--fb-out`). A bare-metal SPARC test drives it *through the hardware registers* and renders a colour-bar test card (`disp_test.png`). This is the interface a booted firmware / MicroPython HAL uses to put pixels on the panel. |

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

## The DISP engine is now a modelled device (not a DRAM peek)

The demo above snapshots a framebuffer the SDK wrote to a *known* DRAM
address. The next step makes the **display hardware itself** real: the
emulator now honours the DISP OSD registers, so any code that programs
them the way the firmware (or a future MicroPython HAL) does gets
scanned out.

`machine_disp_scanout()` (`machine.c`) reads the modelled DISP state and
composites the OSD plane:

- **Palette** from the DISP `GAM_OSD` RAM at `0x80001C00` — 256 words of
  `0x00YYUUVV`, BT.601 studio range, the exact format the firmware's
  `GDI_ChangePALEntry` writes (`jrgb2yuv.c`). The scan-out inverts
  BT.601 back to RGB, so a colour loaded into the palette comes back out
  as itself.
- **Enable** from `DISP_OSD_EN` (bit 28 of `REG_DISP_OSD_SIZE`,
  `0x80001A54`) — a disabled OSD scans out black.
- **Pixels** from the 8bpp plane at `DS_OSDFRAME_ST` (`0x4005F000`, the
  firmware OSD region), linear, palette-indexed.

`make disp` proves it end-to-end: `tests/disp_test.c` (bare-metal SPARC,
no host help) loads a palette into `GAM_OSD`, sets `DISP_OSD_EN`, and
paints an 8bpp test card into the OSD plane — **only** through hardware
register / DRAM writes. The emulator's scan-out renders a clean
SMPTE-style colour-bar + grey-ramp card (`disp_test.png`), each bar's
RGB round-tripping through the YUV palette to its true colour. `--fb-out`
also snapshots a booted firmware's OSD plane once the firmware reaches
its own display init.

Register facts (bases, the `TFT_Init` panel-on order, the active-low
backlight, the OSD framebuffer address) are cited in the repo's
`DP700WD_HW_REFERENCE.md`; this device model is the executable half of
that reference.

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

### Frontier, sharpened with register + memory evidence

Booting the real device image (`dp700wd.bin`, 2 MB / 16 Mbit: sections
`SETD UZIP TEX2 RODA ROMV TEXT DATA ENGL SFAT LOGO CUST CLCK`, all five
zip sections unpack) and reading the CPU state at the stall pins the
cause down exactly. At the spin (`pc=0x40020624`, `cwp=7`):

```
g2=00000001  l5=40042000  i0=00000126  i4=00000000  i1=40032616
[l5=0x40042000] : 00 00 00 00 00 00 00 00 ...   (all zero)
ptbl 0x40046b20 / 0x40046b28 : 00000000 / 00000000
```

The loop's guard is `lduh [%l5]; cmp %g2, [%l5]; bcc` -- it exits only
when the counter `%g2` reaches the halfword count at `%l5`. That count
table at **`0x40042000` is all zeros**, so `1 >= 0` is always true, the
branch is always taken, and it loops forever. The companion register-
pointer table at **`0x40046800`** (used as `ld [0x40046b20]`) is zero
too. Both live in the **bss gap** (between `SFAT`/`DATA` end ~`0x40024e68`
and `ENGL` at `0x40049900`).

A DRAM write-watch over the whole boot shows `0x40042000..40` is written
**only once, with zeros**, by a bss-clear at `pc=0x4001d1f0` -- the
routine that should *populate* these config tables never runs. So this is
not a stuck device bit: an earlier init step that decodes the panel /
customer config into `0x40042000` + `0x40046800` (likely from the
un-loaded `CUST` resource at flash `0x10ff80`, or gated behind a device
read we stub to 0) is being skipped, and the register-programming loop
then consumes the empty table and spins.

**Next step:** find what fills `0x40042000` / `0x40046800`. It is
referenced by many `.text_dram` routines (`sethi %hi(0x40042000)` at
`0x4001d1cc, 0x4001d864, 0x4001d938, ...`). (Note the `0x4001d560 ->
0x4002050c` entry in the jmpl ring is a red herring: `0x4001d560` is a
`jmp %l1; rett %l2` trap-return -- the window-underflow / tick-timer
handler returning *into* the spin, not the loop's caller. The real caller
entered via a pc-relative `call` that has scrolled out of the ring.)
Trace back to the config-decode routine and either run it (stage its
`CUST`/EEPROM input) or seed the two tables directly, then the firmware
should proceed to its own display init -- at which point the modelled
DISP scan-out (`--fb-out`) renders whatever it draws (boot logo from the
`LOGO` section first).

Tooling used to pin this: `--dump-dram`, the jmpl trace ring, the 64-deep
PC ring, and an ad-hoc windowed-register + DRAM-write watch.

### The loop, fully decoded

Disassembling `0x400204f0..0x4002063c` and walking the register windows
resolves it completely. The register-config routine is a doubly-nested
walk; the terminating outer loop is:

```
40020624  inc  %l4                 ; outer index++
40020628  lduh [%i0], %o1          ; outer count = *(u16*)%i0
4002062c  and  %l4, 0xff, %o0      ; index masked to 8 bits
40020630  cmp  %o0, %o1
40020634  bcs,a 0x4002050c         ; loop while (%l4 & 0xff) < count
```

Both inputs are garbage:
- **`%i0 = 0x126`** — a flash address in the boot header, not a table.
  `*(u16*)0x126 = 0x56b7` (25783). Masked to 8 bits, `%l4` tops out at
  255 and can never reach it, so the outer loop cannot terminate.
- **`%l5 = 0x40042000`** — the inner count `*(u16*)%l5 = 0`, so the inner
  guard `cmp %g2, [%l5]` (`%g2=1`) always skips the body: no engine
  registers are ever written.

Call stack at the stall (window-chain unwind):
`0x4001ea40 -> 0x4001ea60 -> flash 0xadbc -> 0x4176c -> 0x42218 ->
0xea74 -> 0x1b3d0 -> 0x33cc0 -> [stall]`. `%i0`/`%l5` are handed in by
this chain from config state that was never built.

`0x40042000` is a shared bss arena (e.g. an IRQ/callback table lives at
`+0x3a0`, managed by `0x4001d918`); the **display-config sub-table at
offset 0 is the part never populated**.

### Root cause, pinpointed

The `%i0 = 0x126` isn't a stray pointer -- it's an **offset with a null
base**. The call at flash `0x33cc0` targets a thunk `0x3d564`, which does
`base = *(desc + 0x10); arg0 = 0x126 + base` before tail-calling the
SFAT routine `0x400203e8` (the one holding the loop). The descriptor is
`desc = 0x4002f75c` (formed as `sethi %hi(0x4002f400); or 0x35c`). So the
stall's `%i0 = 0x126` means **`*(0x4002f76c) = 0`** -- the register-config
**table-base pointer is null**.

A write-watch on `0x4002f75c..0x4002f784` over the whole boot shows the
descriptor is, again, **only ever zeroed** (bss-clear at `0x4001d1f0`) and
never populated. So one specific config-init phase -- the one that fills
`desc+0x10` (table base) and `desc+0x14` (count), plus `0x40042000[0]`
and the `0x40046800` pointer table -- is skipped entirely.

**Exact next step:** find the writer of `0x4002f76c` (`desc+0x10`) -- the
routine that builds the display-register config table -- and determine
what gates it (a mode flag, a panel-ID/`CUST` read, or an ordering
dependency). Run it or stage its output (the table base + count), and the
`0x400203e8` walk gets a valid pointer and a bounded count, terminates,
and boot proceeds to display init -- at which point `--fb-out` renders
the firmware's own screen (boot logo from `LOGO` first).

### Modelling the real DP700WD target (not a generic CT952)

The stall is a *boot-config* subsystem (the strings around it -- "AP code
area", "Err: Unknown DRAM type", "APPacker Version too old" -- and
`0x40260` reading `SYSTEM_CONFIGURATION1` at `0x8000031c` -- show it is
the AP/auto-upgrade + per-panel config path). `0x40260` decodes the
DRAM/flash **hardware strapping** from `SYSTEM_CONFIGURATION1` (bits[4:0],
forced `0b11xxx`) via a jump table at flash `0x40300`; with that register
unmodelled (reading 0) it returns the `0x50000000` "unknown DRAM"
sentinel and config-init aborts.

The real strapping is embedded in the device image's own boot-config
block (`dp700wd.bin` `0xf8c..0xfb0`), which decodes to the actual board:

```
unzip_buff 0x40002000   sp1 0x40012000   code_protect 3
mclk_config 0x00000085  -> 133 MHz
dram_config 0x0108011b  -> 16 Mbit = 2 MB DRAM, 909P   (low byte 0x1b)
prom_config 0x20541010  -> serial flash, fast-read
```

So the DP700WD is a **2 MB-DRAM / 133 MHz / 16 Mbit-flash** board, strap
`0x1b`. The emulator now models `SYSTEM_CONFIGURATION1 = 0x1b` by default
(taken from this image; overridable via `CT952_SYSCFG1`), so `0x40260`
returns a valid type (`0x40200000`, top of 2 MB) instead of the error
sentinel. (The DRAM *allocation* stays 8 MB -- a harmless superset; the
firmware's notion of size comes from the strap. Shrinking the alloc to a
true 2 MB would need `rom_load`'s decompression scratch re-placed, a
separate change, and it does not affect this stall.)

Modelling the strap alone does not fill the config arena -- that init
phase is still skipped -- so a labelled, opt-in bring-up aid,
`--skip-panelcfg`, forces the config thunk's own "no override" path
(`desc+0x14 = -1`, flash `0x3d564`). With it, the real firmware advances
**past the config stall into display init**: it programs the DISP timing
generator (`REG_DISP_TGEN_TOTAL 0x80001a38 = 0x120d035a`), screen size
(`0x80001a44 = 0x00f002d0`, 720x240) and the TVE, then hits the **next
frontier** -- a compute loop at flash `0x32e34` (a bounded sum over a
halfword table left unpopulated by the skipped config). The aid is a
stand-in until the config-arena init is modelled faithfully; it confirms
the display path is reachable and that the DISP model will render the
firmware's own output once boot completes.

### Faithful trace: the descriptor is consumed before it is produced

Chasing this faithfully (no skip): a fetch trace shows the config
*consumer* (thunk `0x3d564`) is reached but the *producer* is not. The
descriptor at `0x4002f75c` is written only by the bss-clear -- confirmed
by a full-boot write-watch on `0x4002f75c..0x4002f784`.

The real producer is **`0x3ce60`**: it initialises the descriptor by
looking up the **`SETD`** (settings/NVRAM sector) and **`SAV1`** (save
area) flash sections via `0x621f0`/`0x3cf24`, storing their addresses
into `desc+0x8/+0x14` and sizes into `desc+0xc/+0x28/+0x30`. `0x3ce60` is
called only from `0x41774`, inside function `0x416f4`.

The ordering is the problem. Disassembling `0x416f4`:

```
416f8  btst 4,%i0 ; be 0x4181c     ; full path only if arg bit2 set
...
4176c  call 0x420d4                ; -> ... -> 0x33cc0 -> thunk 0x3d564  (STALLS)
41774  call 0x3ce60                ; <- descriptor init, never reached
4178c  call 0x33ca4                ; second thunk caller, after init
```

`0x416f4` calls `0x420d4` (which chains through `0x42218 -> 0xea74 ->
0x1b3d0 -> 0x33cc0` to the config thunk) **before** it calls the
descriptor initialiser `0x3ce60` one instruction later. A single-step
trace confirms it: `0xadbc -> 0x416f4` (instr 150789) reaches the thunk
`0x3d564` (instr 153067) and spins; `0x3ce60`/`0x621f0`/`0x3cf24` are
never fetched.

So on real hardware the descriptor must already be valid at `0x4176c` --
initialised by an *earlier* pass that the emulator's `rom_load` does not
reproduce (the section-name lookup table `0x621f0` searches, and/or a
prior call to `0x416f4`/`0xa798` that stages `SETD`/`SAV1`, must be set
up by the real `dsu_boot`/section loader). The faithful fix is to model
that earlier staging -- specifically, make the `SETD`/`SAV1` section
lookup resolve so `0x3ce60` (when it does run) yields a valid `desc+0x10`
base -- rather than papering over it with `--skip-panelcfg`. This is the
next concrete target; it is one more layer of the same boot-config
subsystem, not a new mystery.

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
