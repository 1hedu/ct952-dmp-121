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
| `modct952.c` | `ct952` display module: draw to the OSD plane from Python (`init`, `palette`, `pixel`, `fill`, `rect`, `WIDTH`, `HEIGHT`) |
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
make -j4                       # -> build/firmware.bin
```

## Running

`ct952emu` loads the `.bin` as the flash image and resets to address 0:

```sh
cd ../emu && make ct952emu
./ct952emu ../mpy/build/firmware.bin --instr 20000000
```

UART1 output is echoed to stdout.

### USB keyboard

The demo also enumerates and polls a USB HID keyboard through the `usb_kbd`
module — a bare-metal EHCI driver written in C, driven from Python. Attach a
keyboard and feed keystrokes with `CT952_USB_KEYS`:

```sh
CT952_USB_KEYS="hello world" ./ct952emu ../mpy/build/firmware.bin --instr 150000000
# ...
# usb_kbd: device VID=ceeb PID=0952 class=0 MPS0=64
# usb_kbd: configured, polling ep 0x81
# usb_kbd typed: hello world
```

`usb_kbd.init()` resets the controller, resets the root-hub port, enumerates
the device over the async schedule (GET_DESCRIPTOR / SET_ADDRESS /
SET_CONFIGURATION / HID SET_PROTOCOL), then `usb_kbd.getchar()` polls the
interrupt-IN endpoint for 8-byte HID boot reports and maps them to characters.
With no `CT952_USB_KEYS`, the root hub is empty and `init()` returns `False`.

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
  supplies `mem*`/`str*`, and no libgcc runtime helpers turned out to be
  needed at `-Os`.

## Next

- Interactive REPL over UART RX (`uart_core.c` already implements
  `mp_hal_stdin_rx_chr`; feed input with `ct952emu --uart-in`).
- A `ct952` HAL module binding the modeled display/GPU/input so Python can
  drive the panel (see `jupiter/` for the C-side hardware bring-up).
