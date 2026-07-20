#!/bin/bash
# Advance the boot snapshot by one segment: restore seg.snap, run STEP more
# instructions, snapshot back. Prints reached icount/pc, gate flags, milestones.
# Usage: bash seg.sh <step_instrs> [first]   ("first" = boot fresh, no restore)
cd /home/user/ct952-dmp-121/jupiter/emu
pkill -9 -f ct952emu 2>/dev/null
STEP=${1:-15000000}
SNAP=seg.snap
RESTORE="--restore $SNAP"
[ "$2" = "first" ] && RESTORE=""
for try in 1 2 3; do
  env CT952_TICK_MULT=64 CT952_DUMPFLAGS=1 CT952_UITRACE=1 CT952_DECODE_STACK=1 CT952_OSDUITRACE=1 \
    ./ct952emu dp700wd_ring.bin $RESTORE --run-to $STEP --snapshot ${SNAP}.new 2>seg.err >/dev/null
  rc=$?
  if [ $rc -eq 0 ] && [ -s ${SNAP}.new ]; then mv ${SNAP}.new $SNAP; break; fi
  echo "(retry $try rc=$rc)"; sleep 1
done
grep -aoE "reached icount=[0-9]+ pc=0x[0-9a-f]+" seg.err | tail -1
grep -aE "@4002|DUMPFLAGS" seg.err
echo "--- UI transitions this segment ---"
grep -aE "\[UITRACE\]" seg.err | tail -25
echo "--- milestones ---"
grep -aoE "starting usb stack|EHCI version|HCD: EHCI host controller added|usb no playable|no SD card|KEY_DOWN|POWERONMENU" seg.err | sort -u
