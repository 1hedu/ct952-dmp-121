#!/bin/sh
# Assemble a flasher stub to a flat big-endian SPARC V8 binary and print its hex.
# The hex is embedded in ctkap.py (APSTUB_BIN / APSTUB_SEC_BIN); run this after
# editing the .S and paste the output back into that constant.
#
#   sh jupiter/tools/build_apstub.sh                       # apstub.S     @0x4009a000 -> APSTUB_BIN
#   sh jupiter/tools/build_apstub.sh apstub_sec.S 0x40500000  # apstub_sec.S @LMA     -> APSTUB_SEC_BIN
set -e
CROSS=${CROSS:-sparc64-linux-gnu-}
DIR=$(dirname "$0")
SRC=${1:-apstub.S}
TEXT=${2:-0x4009a000}
OUT=/tmp/$(basename "$SRC" .S).bin
"${CROSS}as" --32 -Av8 -o /tmp/_apstub.o "$DIR/$SRC"
"${CROSS}ld" -m elf32_sparc -Ttext="$TEXT" -e _start -o /tmp/_apstub.elf /tmp/_apstub.o
"${CROSS}objcopy" -O binary -j .text /tmp/_apstub.elf "$OUT"
echo "built $OUT from $SRC @ $TEXT ($(wc -c < "$OUT") bytes)"
python3 -c "print(open('$OUT','rb').read().hex())"
