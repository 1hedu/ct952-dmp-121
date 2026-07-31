#!/bin/sh
# Build the card-dump AP -- no MicroPython, no display, no keyboard. Reuses
# the banner AP's trap-table entry stub and DRAM linker script unchanged
# (see start_banner.S, ct952_app.ld): both are generic to any freestanding
# app in the 0x400c0000 payload window, not banner-specific.
set -e
CROSS=sparc64-linux-gnu-
F="-m32 -mcpu=v8 -msoft-float -mno-app-regs -nostdlib -ffreestanding -fno-common -fno-builtin -fno-tree-loop-distribute-patterns"
cd "$(dirname "$0")"
${CROSS}gcc $F -c start_banner.S -o /tmp/cdstart.o
${CROSS}gcc $F -Os -std=c99 -c carddump.c -o /tmp/carddump.o
${CROSS}gcc -m32 -mcpu=v8 -nostdlib -Wl,-N,--build-id=none,-T,ct952_app.ld \
    /tmp/cdstart.o /tmp/carddump.o -o /tmp/carddump.elf
${CROSS}objcopy -O binary /tmp/carddump.elf /tmp/carddump.bin
python3 ../tools/ctkap.py mkrunap /tmp/carddump.AP /tmp/carddump.bin
echo "built /tmp/carddump.AP"
