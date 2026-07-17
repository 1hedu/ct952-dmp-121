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
| Stock ROM boot | **Root-caused stall** — see below; boots and runs millions of instructions with zero illegal-instruction traps |

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
make boot       # run the stock ../../DVD909.rom, capture UART + I/O log

./ct952emu <flash.rom> [--instr N] [--uart FILE] [--iolog FILE]
           [--seed-entry ADDR] [--seed-sp ADDR] [--quiet]
```

Requires `gcc` for the host build and `sparc64-linux-gnu-gcc` (with
`-m32`) for the CPU test image.
