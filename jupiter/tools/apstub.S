/*
 * Self-flashing AP body stub for the CheerTek CT952A (big-endian SPARC V8).
 *
 * The firmware AP loader (flash 0x3e48) validates a UPG952A.AP header, copies
 * the body (file bytes [0x200..size]) to DRAM 0x4009a000, and jumps into it at
 * the header's entry field (0x30). This stub IS that body's entry. It reads a
 * descriptor the packer places at body+0x100 and reprograms the serial flash by
 * looping the firmware's own WriteSPF primitive (XIP, flash 0x3d0fc) over a
 * carried image, 64 KB per call.
 *
 * Body layout (produced by ctkap.py mkflasher):
 *   +0x000  this stub (code), padded to 0x100
 *   +0x100  u32 nchunks
 *   +0x104  nchunks * { u32 dstFlashAddr; u32 srcBodyOff; u32 size }
 *   +....   the image bytes (srcBodyOff is a byte offset within THIS body)
 *
 * WriteSPF(%o0=flashAddr, %o1=srcBuf, %o2=size): 64 KB-aligned erase-then-
 * program; returns 0 on success. srcBuf is a DRAM address, so we add the body
 * base (0x4009a000) to each descriptor's srcBodyOff.
 *
 * NOTE ON REAL HARDWARE: calling the XIP WriteSPF to rewrite the flash sector
 * that *contains* WriteSPF would erase the code being executed. This stub is
 * for verifying flashing LOGIC against the emulator's WriteSPF-contract model
 * (which intercepts at the entry, so the XIP sector is never actually executed
 * during the erase). A hardware-safe body must run a DRAM-resident copy of the
 * flash driver (as the OEM AP does) -- see CT952A_FLASH_FORMAT.md.
 */
    .text
    .align 4
    .globl _start
_start:
    set   0x4009a000, %l0        ! %l0 = body base in DRAM
    ld    [%l0 + 0x100], %l1     ! %l1 = nchunks
    add   %l0, 0x104, %l2        ! %l2 = chunk-table pointer
    cmp   %l1, 0
    be    done
     nop
loop:
    ld    [%l2 + 0x0], %o0       ! dstFlashAddr
    ld    [%l2 + 0x4], %o1       ! srcBodyOff
    ld    [%l2 + 0x8], %o2       ! size
    add   %o1, %l0, %o1          ! srcBuf = base + srcBodyOff
    set   0x0003d0fc, %g1        ! &WriteSPF (XIP)
    jmpl  %g1, %o7               ! WriteSPF(%o0,%o1,%o2)
     nop
    add   %l2, 12, %l2           ! next descriptor
    subcc %l1, 1, %l1
    bne   loop
     nop
done:
    set   0x80007ff4, %g1        ! positive completion signal for the harness
    set   0xc0ded00d, %g2
    st    %g2, [%g1]
    ta    0                      ! trap with ET=0 -> clean emulator halt
    nop
