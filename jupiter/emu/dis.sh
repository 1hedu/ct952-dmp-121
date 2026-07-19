#!/bin/bash
# dis.sh <start_hex> <len_hex>  -- disassemble retail dp700wd.bin (XIP vaddr==flash off)
BIN="$(dirname "$0")/dp700wd.bin"
s=$1; n=${2:-0x200}
dd if="$BIN" of=/tmp/_dis.bin bs=1 skip=$((s)) count=$((n)) 2>/dev/null
sparc64-linux-gnu-objdump -b binary -m sparc -EB -D /tmp/_dis.bin --adjust-vma=$((s)) 2>/dev/null | sed -n '5,400p'
