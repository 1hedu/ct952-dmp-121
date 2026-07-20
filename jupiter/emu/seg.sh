#!/bin/bash
# Advance the boot snapshot by one segment: restore seg.snap, run STEP more
# instructions, snapshot back. Prints the reached icount/pc + any milestone.
# Usage: bash seg.sh <step_instrs>
cd /home/user/ct952-dmp-121/jupiter/emu
pkill -9 -f ct952emu 2>/dev/null
STEP=${1:-15000000}
SNAP=seg.snap
for try in 1 2 3; do
  env CT952_TICK_MULT=64 CT952_PCSAMP=500000 CT952_PCSAMP_WIN=8000000 \
    ./ct952emu dp700wd_ring.bin --restore $SNAP --run-to $STEP --snapshot ${SNAP}.new 2>seg.err >/dev/null
  rc=$?
  if [ $rc -eq 0 ] && [ -s ${SNAP}.new ]; then
    mv ${SNAP}.new $SNAP
    break
  fi
  echo "(retry $try rc=$rc)"; sleep 1
done
grep -aoE "reached icount=[0-9]+ pc=0x[0-9a-f]+" seg.err | tail -1
echo "--- milestones this segment ---"
grep -aoE "starting usb stack|reaper thread|Blk dev: Init|EHCI version|new device port=[0-9]+|HCD: EHCI host controller added|uhub[0-9]|POWERONMENU|OSDSS_[A-Za-z]+|ShowJPEG|no media|MEDIA_[A-Za-z]+" seg.err | sort -u
echo "--- last PCSAMP window ---"
grep -aA9 "PCSAMP @" seg.err | tail -11
