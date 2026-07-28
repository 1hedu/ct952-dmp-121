#!/bin/sh
# Assemble apstub.S (the self-flashing AP body stub) to a flat big-endian SPARC V8
# binary and print its hex. The hex is embedded as APSTUB_BIN in ctkap.py; run this
# after editing apstub.S and paste the output back into that constant.
#
#   sh jupiter/tools/build_apstub.sh          # -> apstub.bin + hex on stdout
set -e
CROSS=${CROSS:-sparc64-linux-gnu-}
DIR=$(dirname "$0")
OUT=${1:-/tmp/apstub.bin}
"${CROSS}as" --32 -Av8 -o /tmp/apstub.o "$DIR/apstub.S"
"${CROSS}ld" -m elf32_sparc -Ttext=0x4009a000 -e _start -o /tmp/apstub.elf /tmp/apstub.o
"${CROSS}objcopy" -O binary -j .text /tmp/apstub.elf "$OUT"
echo "built $OUT ($(wc -c < "$OUT") bytes)"
python3 -c "print(open('$OUT','rb').read().hex())"
