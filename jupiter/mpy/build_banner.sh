#!/bin/sh
# Build the minimal on-screen banner AP (no MicroPython, no keyboard) -- the
# display bring-up isolation vehicle. Tiny (~6 KB) so each real-device test is
# seconds. NOTE: --build-id=none is REQUIRED: a .note.gnu.build-id section at
# offset 0 pushes _tt off 0x400c0000 and the loader jumps into the note bytes.
set -e
CROSS=sparc64-linux-gnu-
F="-m32 -mcpu=v8 -msoft-float -mno-app-regs -nostdlib -ffreestanding -fno-common"
cd "$(dirname "$0")"
${CROSS}gcc $F -c start_app.S -o /tmp/bstart.o
${CROSS}gcc $F -Os -std=c99 -c banner.c -o /tmp/banner.o
${CROSS}gcc -m32 -mcpu=v8 -nostdlib -Wl,-N,--build-id=none,-T,ct952_app.ld \
    /tmp/bstart.o /tmp/banner.o -o /tmp/banner.elf
${CROSS}objcopy -O binary /tmp/banner.elf /tmp/banner.bin
python3 ../tools/ctkap.py mkrunap /tmp/banner.AP /tmp/banner.bin
echo "built /tmp/banner.AP"
