#!/usr/bin/env python3
# Make a debug-enabled build of the retail firmware.
#
# ONE-BYTE patch: in UTL_Config_DebugMode (flash 0x11500), the UTL_DBG_INIT
# path sets _dwDBGMode = 0x11 (DSU1 only -> no UART DBG_Init -> silent).
# Patching the immediate to 0x111 adds the UART1_TX nibble, so the function
# calls DBG_Init(HAL_UART1_TX) at boot (initial.c:465), which sets
# _bDBGEnable=TRUE, allocates the DRAM debug ring (@0x4004c000), and routes
# DBG_Printf to UART1. The firmware then narrates its own boot.
#
#   0x11518: 90 10 20 11  mov 0x11,  %o0   ->  90 10 21 11  mov 0x111, %o0
#   i.e. byte @ 0x1151a: 0x20 -> 0x21
#
# Read the ring straight from DRAM (probe reads _pDBG_Header1 @0x40032508,
# 96-byte descriptors) since the ring's UART drain (DBG_Polling/DBG_INT) only
# runs in CC_DVD_MainLoop, which the long init hasn't reached yet.
# SECOND patch (optional, --live): _PutCHAR (0x13dac) buffers normal-priority
# chars into the DRAM ring and only DMAs PRIORITY_HIGH chars straight to UART;
# the ring's drain (DBG_Polling/DBG_INT) only runs once CC_DVD_MainLoop or the
# MONITOR_INTERRUPT alarm is up, which the long init hasn't reached -- so the
# UART stays silent even though messages accumulate. NOPing the branch at
# 0x13db8 (be 0x13dd4) makes EVERY char go direct to UART1 -> continuous live
# narration to the log with a plain run, no gdb, no ring, no drain needed.
#   0x13db8: 02 80 00 07  be 0x13dd4   ->  01 00 00 00  nop
import sys, shutil
src = sys.argv[1] if len(sys.argv) > 1 else 'dp700wd.bin'
dst = sys.argv[2] if len(sys.argv) > 2 else 'dp700wd_dbg.bin'
live = '--live' in sys.argv          # also stream every char straight to UART
shutil.copyfile(src, dst)
with open(dst, 'r+b') as f:
    f.seek(0x11518); before = f.read(4)
    assert before == bytes.fromhex('90102011'), 'unexpected bytes @0x11518: %s' % before.hex()
    f.seek(0x1151a); f.write(bytes([0x21]))
    f.seek(0x11518); after = f.read(4)
    print('%s: 0x11518 %s -> %s  (mov 0x11 -> mov 0x111, UART1_TX debug on)' % (dst, before.hex(), after.hex()))
    if live:
        f.seek(0x13db8); b2 = f.read(4)
        assert b2 == bytes.fromhex('02800007'), 'unexpected bytes @0x13db8: %s' % b2.hex()
        f.seek(0x13db8); f.write(bytes.fromhex('01000000'))
        print('       0x13db8 %s -> 01000000  (_PutCHAR be->nop, all chars direct to UART)' % b2.hex())
