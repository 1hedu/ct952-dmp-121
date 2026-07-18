# ct952emu — bring-up journal

A from-scratch full-machine emulator for the CT909P/CT952 SoC, in the
spirit of the Jupiter SDK's `licheeEmu`: not just the CPU but the
device fabric, so the stock ROM can eventually run on a host PC.

This is honest, iterative hardware bring-up. Each entry records what
works, what's blocked, and exactly what the next step is — the
emulator's own I/O-access inventory is the worklist generator.

## IT RENDERS: the firmware draws "Loading ." on screen

With the real boot running (below), the firmware's application draws its UI
through the **CT952 2-D GPU engine** (`0x80002880`, `gdi.c` programming) --
overwhelmingly the **1-bit font** path (133k+ font ops, ~0 fills). The
emulator now models that engine (`gpu_exec` in `machine.c`): on a
`REG_GPU_CTL0` write with `GPU_START` it executes the op and clears the
busy bit. FillRectangle fills the 8bpp OSD plane; the font op expands each
queued glyph (indices written to `REG_GPU_FONT_RAM_INDEX`) from the DRAM
glyph table (`FONT_ADDR`, 64-byte 1-bit glyphs, MSB-first) into the plane
with the `COL_NDX` foreground/background indices, advancing per glyph.

Result: `make bootreal` boots `dp700wd.bin` from reset and the OSD plane at
`0x4005f000` (480-wide, the DP700WD panel) shows the firmware's real boot
message -- **"Loading ."** -- rendered pixel-for-pixel by the device's own
font engine. `--fb-out` snapshots it (rendering the plane even before the
firmware flips the OSD-enable bit; a fallback index ramp keeps it legible
if the OSD palette RAM isn't loaded yet). The full arc is closed: correct
chip ID -> firmware self-boots -> eCos app runs -> 2-D engine draws -> we
read the screen.

## BREAKTHROUGH: the real first-stage boot runs (correct chip ID)

The whole "config-arena / rom_load" saga had one root cause: **the emulated
CPU reported the wrong PSR chip ID.** The stock boot at flash `0x310` reads
`PSR[31:24]` and only takes its full clock/DRAM/section-load path when it
reads **`0xa0`** (the real CT952); anything else falls to a debugger-style
trampoline that expects pre-staged entry/SP -- which is exactly why the
emulator needed `--rom-load` and never ran the true boot. Setting
`sparc_reset` PSR to `0xA0000000` (impl=0xA, ver=0) makes the firmware's
**own** first-stage boot run from reset, and it prints its real UART log:

```
<P1 Booting>
SP1=40012000  DRAM_Config=0108011b [Size=16MBit]  MCLK=133MHz  PROM_Config=20541010
Set PROM Controller...Ok   Code_Protect=3
Load Sec[ROMV]:40000000...Ok   Load Sec[TEXT]:4001d000...Ok
Load Sec[DATA]:40020878...Ok   Load Sec[ENGL]:40049900...Ok
Jump Sec[ROMV]:40000000
```

Run it faithfully with **`ct952emu dp700wd.bin`** (no `--rom-load`, no
aids): the firmware sets up the PROM controller, decompresses every
section with its own codec, jumps to the main app, and reaches the eCos
scheduler/idle loop. It programs the DISP timing generator (720x240) on
its own. CPU verification stays bit-exact. The `--rom-load` path and the
panel-config aids remain for diagnostics but are no longer needed for a
real boot.

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

### Deeper: it is a two-phase init and only phase 2 runs

`0x420d4` (called at `0x4176c`) is the **display-config apply**: it stages
real panel timing into `0x40033744` (`0x2d0`=720, `0x1e0`=480, `0xf0`=240)
and then, at `0x42218`, calls `0xea18` -> ... -> the config thunk to push
that config through the descriptor's register table -- which is empty, so
it stalls. The table's source, the **`SETD`** settings sector, *does*
contain real data in this image (`flash 0x1000`: `43 00 01 0d 03 01 05 7d
...`, 204 non-trivial bytes), so the config is present on the device; the
problem is purely that the descriptor pointing at it is never built.

The descriptor's *correct uninitialised state is `-1`* (skip): `0x3cf60`
sets `desc+0x14 = -1` based on a MODE byte at `0x40033723` (which has no
explicit writer -- it stays 0). And `0x416f4` is two-phase: its entry
`btst 4,%i0 ; be 0x4181c` splits an early default-init path (`0x4181c`,
arg bit2 clear) from the apply path (bit2 set, the `0x4176c`/`0x41774`
body). The emulator only ever runs the **apply** phase (via `0xadbc`,
bit2 set); the early default-init call (via `0xa798`->`0xa8c0`, which
would run with bit2 clear and set the descriptor to its `-1`/SETD-derived
default) is never reached. So phase 2 runs before phase 1 -- the missing
early pass is why the descriptor is raw zero instead of `-1`-or-valid.
Next: find why `0xa798` (the phase-1 caller) is skipped in this boot.

### Root: the display-init phase is event/callback-gated

Both `0x416f4` call sites confirm the split -- `0xadbc` passes `0x14`
(bit2 set = apply), `0xa8c0` passes `0x11` (bit2 clear = default-init).
The default-init orchestrator is fn `0xa798`, and it is **event-gated**:
a dispatcher at `0x4860` calls it only when
`*(0x4002fb58) != 0` **or** `*(state+0x1f4) == 2` (an init/mode event).
Those flags are 0 in the emulator, so `0xa798` -- and the phase-1
descriptor default-init it drives -- never runs.

The apply side is worse-coupled: fn `0xadb0` (which contains the stalling
`0xadbc` apply) has **no direct flash callers** -- it is invoked through a
registered function pointer (a callback / message handler). So the apply
fires from the firmware's event system *before* the default-init event is
posted, and stalls on the still-raw descriptor.

**Conclusion.** This is no longer a single missing register or table --
it is the firmware's **boot event/callback sequencing**. On real hardware
a boot init step posts the display-init event (`*(0x4002fb58)` /
`state+0x1f4`), `0xa798` runs, `0x416f4`'s default path + `0x3ce60` build
the descriptor from the real `SETD` settings (which are present in this
image), and only then does the apply callback fire. The emulator's
`rom_load` stages code+data but does not reproduce this event posting, so
the ordering inverts. A faithful fix means modelling that boot event
sequence (or running the real `dsu_boot`, which posts it) -- a deeper
undertaking than a device stub. The `--skip-panelcfg` aid short-circuits
exactly this gap (forcing the apply's descriptor to the `-1` "no-override"
skip the firmware itself uses), and demonstrably carries boot into display
init; it stands in until the event layer is modelled.

### Toward faithful: run the firmware's own builder on the real SETD

Rather than skip, `--build-panelcfg` reproduces the missing default-init
*faithfully*: at the first fetch of the config thunk (`0x3d564`) it saves
the boot CPU context, invokes the firmware's own descriptor builder
`0x3ce60` via `machine_call` (the same in-emulator-execution trick that
cracked the decompressor), then restores context. `0x3ce60` runs cleanly
(`rc=0`) and **builds the descriptor from the real `SETD` sector**:
`desc+0x14 = 0x1000` (SETD flash address), `desc+0x10 = 0x1c00`. So the
firmware's own code, on the device's own settings, produces a live
descriptor -- the faithful path works.

It is **not yet sufficient alone**, and the trace shows why: the config
walk also reads an *inner* register table at DRAM `0x40042000`, which is
still empty -- that table is populated by the apply pass itself (from the
descriptor), and `desc+0x10 = 0x1c00` is a flash offset the walk consumes
differently than a ready DRAM table. So one more layer remains: either
run the descriptor build at the point where the apply expects it (so the
apply fills `0x40042000`), or model the apply's table-population. The
mechanism and the proof that the builder works on real SETD data are the
groundwork; `--build-panelcfg` is committed as the in-progress faithful
path, `--skip-panelcfg` remains the working bridge to display init.

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

---

## Update: from "Loading . . ." into photo-display mode

Two device-model gaps were keeping the stock DP700WD firmware parked on the
`Loading . . .` screen, spinning ~38M times per run polling the video decoder
for progress that never came. Both are now modeled (`machine.c`), and the
firmware advances through decoder init into slideshow/photo mode -- it clears
the loading OSD to expose the video plane.

### 1. Display VSYNC interrupt (the missing heartbeat)

The panel's field tick drives the firmware's display/slideshow state machine
(`interrupt.c`: `INT_Proc1_1st_isr` -> `ISR_DISPSaveClearStatus`). It arrives
on a *secondary* interrupt controller -- "PROC1 1st" at `0x800000b0..bc`
(`ctkav_platform.h`) -- that cascades into LEON interrupt line 13
(`INT_NO_PROC1_1ST`). The firmware unmasks line 13 and enables all secondary
sources (`REG_PLAT_PROC1_1ST_INT_MASK_ENABLE = 0xffffffff`), then waits.

Modeled:
* the secondary controller registers -- `MASK_ENABLE` (0xb0, direct RW mask),
  `PENDING` (0xb4, RW; VSYNC source sets bit0), `STATUS`/`CLEAR` (0xb8, R /
  W1C), `MASK_DISABLE` (0xbc, W1C into the mask);
* a **level-triggered cascade**: LEON line 13 asserts whenever
  `(PENDING & MASK_ENABLE) != 0`, computed live in `bus_irq_level`;
* a **field-rate VSYNC source** in `machine_cycle` (env-tunable divider
  `CT952_VSYNC_DIV`, default 200000 cycles).

With this the display ISR runs and the firmware programs the video window
(`REG_DISP_VIDEO_POS` becomes `0x00150065`).

### 2. PROC2 vdec command handshake

PROC1 commands the video decoder by writing `REG_SRAM_PLAYMODE` (`0xb0000190`,
in the vdec SRAM `ctkav_vdec.h`); the decoder microcode on PROC2 overwrites it
with a **completion state** (`comdec.h` `EN_VDEC_CMD`) that PROC1's wait loops
poll for -- e.g. `_Wait_Decoder_Stop_CMD_ACK` (`hal.c`) waits for
`MODE_STOPPED`. Command values observed from the firmware: `0x10` MODE_STOP,
`0x11` MODE_STOPPED, `0x80` MODE_PREDECODE, `0x86` MODE_RELEASE_MODE, `0x00`
NONE/reset.

We don't execute the PROC2 microcode, so a stand-in delivers the ack after a
short poll latency (`proc2_ack_of`): `STOP->STOPPED`, `SCAN->SCAN_DONE`,
`PREDECODE->PREDEC_DONE`, `reset/NONE->STOP`. This carries the firmware through
several decoder-reset/command cycles it otherwise dead-locked on.

### The built-in demo photos

The frame ships five 640x360 demo images -- the Windows sample-picture set
(zebra-longwing butterfly, Grand Teton barn, chrysanthemum, Golden Gate, ...) --
as ordinary JFIF/EXIF JPEGs at flash `0x160000/0x170000/0x180000/0x190000/
0x1a0000` (one per 64 KiB slot, each followed by two thumbnails).
`tools/extract_photos.py` pulls them out and renders them at the 480x234 panel
geometry (what the LCD actually shows).

### Remaining path to a firmware-rendered photo on screen

The firmware now reaches photo mode but does not yet complete an actual frame
decode (the DISP video frame-buffer registers `REG_DISP_F0Y/F0C_ADDR` and the
BIU bitstream source stay 0). To close the loop:

1. Drive the firmware's still-JPEG decode far enough that it programs the video
   frame buffers and the BIU bitstream-read channel (the JPEG source in DRAM).
2. Intercept the decode kick; decode the JPEG host-side (or model the JPU) and
   write the result into the destination frame buffer in the hardware's tiled
   YUV 4:2:0 layout (`jfb.h`).
3. Add a DISP **video-plane** scan-out path (currently only the 8bpp OSD plane
   is composited by `machine_disp_scanout`); composite video + OSD to a PPM.
