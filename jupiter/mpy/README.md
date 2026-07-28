# MicroPython on the CT952 (big-endian SPARC V8)

A bare-metal [MicroPython](https://github.com/micropython/micropython) port
for the CheerTek CT952/CT909 DVD-player SoC, running under the `ct952emu`
emulator. Python executing on a DVD player.

```
[ct952] MicroPython booting...
hello from MicroPython on a CT952 DVD player!
2**32 - 1 = 4294967295
squares: [1, 4, 9, 16, 25, 36, 49, 64, 81, 100]
sum(1..100) = 5050
big-endian bytes of 0x01020304: b'\x01\x02\x03\x04'
[ct952] done.
```

## What this is

The full MicroPython lexer → parser → compiler → bytecode VM, GC heap, and
MPZ arbitrary-precision integers, cross-compiled for the CT952's native ABI:
**ILP32, big-endian, soft-float SPARC V8**, freestanding (no OS, no libc).
It boots from the flash reset vector, sets up register windows, copies `.data`
into DRAM, and runs a Python script, printing over UART1.

## Layout

| File | Role |
|---|---|
| `micropython/` | MicroPython core, as a git submodule pinned to a tested commit |
| `main.c` | `mpy_main()`: GC heap in DRAM, `mp_init`, run the demo script |
| `uart_core.c` | `mp_hal_stdout_tx_strn` / `mp_hal_stdin_rx_chr` on UART1 (`0x80000070`) |
| `modct952.c` | `ct952` hardware module: OSD draw (`init`, `palette`, `pixel`, `fill`, `rect`), the **GPU 2-D engine** (`gpu_fill`, `text`), the **JPU decoder** (`decode_jpeg`), the **SD card** (`sd_present`, `sd_init`, `sd_read`), the **IR remote** (`ir_poll`), the **panel keys** (`panel_adc`), and raw register access (`peek32`, `poke32`, `poke_bytes`) |
| `modusb_kbd.c` | `usb_kbd` module: a bare-metal EHCI + USB HID boot-keyboard driver (`init`, `poll`, `getchar`) — enumerate and read a USB keyboard from Python |
| `mpconfigport.h` | Feature config (minimal + compiler + GC + MPZ long ints; `MICROPY_NLR_SETJMP`) |
| `setjmp.h` | `setjmp`/`longjmp` → gcc builtins (handle SPARC register windows, no libc) |
| `start.S` | Reset trap table, window overflow/underflow handlers, `.data` copy, `.bss` zero, stack, `mpy_main` |
| `ct952.ld` | Flash (XIP @ 0) for code + rodata + `.data` init image; DRAM @ `0x40000000` for `.data`/`.bss`/stack |
| `sysinc/gnu/stubs-32.h` | Empty stub — the sparc64 cross toolchain lacks the 32-bit multilib header (we build `-nostdlib`) |

## Building

Needs `sparc64-linux-gnu-gcc` (Ubuntu package `gcc-sparc64-linux-gnu`) and a
host Python for MicroPython's qstr generation.

```sh
git submodule update --init jupiter/mpy/micropython   # first time
cd jupiter/mpy
make -j4                              # standalone -> build/firmware.bin
make APP=1 BUILD=build-app -j4        # embedded "app" -> build-app/firmware.bin
```

The **standalone** build boots from reset and replaces the whole firmware. The
**app** build is a DRAM-resident payload injected into a *booted* firmware and
launched in place of a firmware app -- see "Python as a firmware app" below.

## Running

`ct952emu` loads the `.bin` as the flash image and resets to address 0. The
firmware boots to an **interactive MicroPython REPL** reading from a USB
keyboard and UART1:

```sh
cd ../emu && make ct952emu
./ct952emu ../mpy/build/firmware.bin --instr 200000000
```

UART1 output is echoed to stdout.

### Interactive REPL over a USB keyboard

After a one-time startup (`import ct952; ct952.init()`) the port drops into
MicroPython's friendly REPL. Input comes from a USB HID keyboard — enumerated
and polled by the bare-metal EHCI driver (`modusb_kbd.c`) — and/or UART1 RX.
Feed a keyboard "session" with `CT952_USB_KEYS` (`\n` = Enter, shift handled):

```sh
printf -v KEYS 'print(2+2)\nimport ct952\nct952.fill(11)\n2**16\n'
CT952_USB_KEYS="$KEYS" ./ct952emu ../mpy/build/firmware.bin --instr 200000000
```

```
usb_kbd: device VID=ceeb PID=0952 class=0 MPS0=64
usb_kbd: configured, polling ep 0x81
[ct952] USB keyboard ready -- type Python below
MicroPython v1.29.0-preview on ...; ct952 with sparc-v8-be
>>> print(2+2)
4
>>> import ct952
>>> ct952.fill(11)
>>> 2**16
65536
```

Every character arrives as an 8-byte USB HID boot report through the modeled
EHCI controller: `usb_kbd_bringup()` resets the controller, resets the root-hub
port, and enumerates the device over the async schedule (GET_DESCRIPTOR /
SET_ADDRESS / SET_CONFIGURATION / HID SET_PROTOCOL); the REPL's stdin
(`mp_hal_stdin_rx_chr`) then polls the interrupt-IN endpoint and UART1 RX. With
no `CT952_USB_KEYS`, the root hub is empty and the REPL reads UART1 only
(`--uart-in`). The `usb_kbd` module (`init`, `poll`, `getchar`) exposes the same
driver to Python.

### Driving the hardware from Python

The `ct952` module binds the emulator's modeled peripherals, so the REPL drives
the real blocks, not just the framebuffer:

```python
ct952.gpu_fill(20, 20, 220, 60, 1)        # 2-D engine fill-rectangle
ct952.text(30, 100, 'HELLO CT952', 9, 10) # GPU 1-bit font engine (built-in 8x8 font)
ct952.decode_jpeg(0x41000000)             # kick the JPU JPEG decoder -> video plane
ct952.sd_init(); blk = ct952.sd_read(0, 1)  # SD host controller: DMA block read
ct952.ir_poll()                           # read the IR remote (feed with CT952_IRKEY)
ct952.panel_adc(0x84)                     # read a panel key-ladder ADC line
ct952.poke32(0x80001a54, 0x10f001e0)      # any register
v = ct952.peek32(0x80001a54)              # ...read it back
ct952.poke_bytes(0x41000000, jpeg_data)   # stage bytes in DRAM (e.g. before decode_jpeg)
```

`gpu_fill` and `text` program the GPU's shared JPU/GPU block (`0x80002880`) and
kick the engine; the emulator's `gpu_exec()` writes the pixels into the 8bpp OSD
plane. `decode_jpeg` points the MCU-BIU bitstream source at a DRAM address and
runs a JPU op, so the functional picojpeg decoder reconstructs the frame onto
the video plane the display composites under the OSD. (Set `CT952_OSD_FULL=1` to
composite the whole 240-row OSD in `--fb-out`; the default clamps to ~51 rows.)

## Python as a firmware app (on-device live debugger)

The `app` build runs MicroPython **inside a booted CT952 firmware**, in place of
a firmware app, so Python is an interactive console over the *live* firmware
state. The payload links into the runtime-free DRAM window `0x40500000` (all-zero
after boot), installs its own trap table, and runs with interrupts masked so it
owns PROC1 without disturbing eCos's dormant handlers.

```sh
# boot the real firmware, inject the app, seize PROC1 at 40M instructions:
CT952_PYAPP=../mpy/build-app/firmware.bin CT952_PYAPP_AT=40000000 \
CT952_USB_KEYS=$'print(hex(ct952.peek32(0x40000000)))\n' \
  ./ct952emu dp700wd.bin --instr 200000000
```

`CT952_PYAPP_HOOK=<pc>` instead seizes when the firmware first reaches a specific
app-entry PC (the "replaced app"). Inside the REPL the firmware is live:

```python
ct952.peek32(0x40039074)              # read a firmware variable (__bISRKey)
ct952.poke32(0x40023a10, 1)           # flip a firmware gate
ct952.call(0xd3900, dst, src, n)      # call a firmware function (here memcpy)
```

`ct952.call(addr, a0..a3)` invokes firmware code directly (args in `%o0..%o3`,
result from `%o0`) -- verified by driving the firmware's own `memcpy`. This turns
the emulator into an interactive firmware lab: seize, then peek/poke/call and
observe, with no rebuild between experiments.

## SPARC bring-up notes

- **Register windows.** The parser/compiler/VM recurse deeply; `start.S`
  provides the canonical SPARC V8 window overflow (tt 5) / underflow (tt 6)
  handlers (NWIN = 8) so windows spill/reload transparently.
- **Exceptions (NLR) + `ta 3`.** MicroPython has no dedicated SPARC NLR, so we
  use `MICROPY_NLR_SETJMP`; `setjmp.h` maps `setjmp`/`longjmp` to
  `__builtin_setjmp`/`__builtin_longjmp`. On SPARC `__builtin_longjmp` emits
  `ta 3` (`ST_FLUSH_WINDOWS`) to spill the register windows before the jump, so
  `start.S` implements the tt 0x83 flush-windows handler (spills 7 windows to
  their stacks, returns past the `ta`). Without it, the first raised exception
  traps.
- **Big-endian.** Verified live: `(0x01020304).to_bytes(4, 'big')` yields
  `b'\x01\x02\x03\x04'`, and MPZ long ints (`2**32 - 1`) are exact.
- **No 32-bit libgcc / libc.** The link is `-nostdlib`; `shared/libc/string0.c`
  supplies `mem*`/`str*` and `shared/libc/printf.c` supplies `snprintf` (for
  readline), and no libgcc runtime helpers turned out to be needed at `-Os`.
  `-U_FORTIFY_SOURCE` stops the toolchain headers redirecting `snprintf` to the
  glibc `__snprintf_chk` fortify helper.

## Next

- A frozen `boot.py`/`main.py` in flash so scripts run without a keyboard.
- Bind more of the modeled hardware to `ct952` (GPU 2-D engine, IR/panel keys,
  the SD card) so Python can drive the whole player.
