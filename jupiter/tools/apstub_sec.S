/*
 * Section-table AP flasher app for the CheerTek CT952A (big-endian SPARC V8).
 *
 * Unlike apstub.S (a raw-code body for the emulator's direct-jump harness), THIS
 * is the code of a *section* inside a loader-compatible AP. The firmware AP loader
 * (AP_Loader -> ROMLD_BOOT_LoadSectionAndRun @0x4bc -> section iterator @0x528)
 * copies/decompresses this section to its LMA and JUMPS to it (it is the section
 * flagged Load|ProgEntry). It then reflashes by calling the firmware's OWN DRAM-
 * resident flash driver -- so nothing runs from the flash being erased (the true
 * DRAM-driver relocation, given us for free by the section-table AP format).
 *
 * Linked/loaded at LMA 0x40500000 (runtime-free high DRAM). Descriptor at LMA+0x100
 * (packed by ctkap mksectionap): u32 nchunks; { u32 dstFlash; u32 srcOff; u32 size }*;
 * then the image bytes (srcOff is relative to LMA). Per chunk it replicates WriteSPF:
 * 16x4K sector-erase (erase-type [0x40033723]==4) or one erase, then program -- via
 * the resident driver erase 0x4001fea8 / program 0x4002003c.
 */
    .text
    .align 4
    .globl _start
_start:
    set   0x40500000, %l0        ! %l0 = section LMA (this app's base)
    ld    [%l0 + 0x100], %l1     ! %l1 = nchunks
    add   %l0, 0x104, %l2        ! %l2 = chunk table
    cmp   %l1, 0
    be    done
     nop
chunk_loop:
    ld    [%l2 + 0x0], %l3       ! dstFlashAddr
    ld    [%l2 + 0x4], %o0       ! srcOff
    ld    [%l2 + 0x8], %l5       ! size
    add   %o0, %l0, %l4          ! src = LMA + srcOff
    set   0x40033723, %g1        ! erase-type byte
    ldub  [%g1], %g1
    cmp   %g1, 4
    bne   erase_single
     nop
    mov   0, %l6                 ! n = 0 .. 15 (16 x 4K = 64K)
erase_loop:
    sll   %l6, 12, %o0
    add   %l3, %o0, %o0          ! dst + n*0x1000
    set   0x4001fea8, %g1        ! resident erase_4k
    jmpl  %g1, %o7
     nop
    add   %l6, 1, %l6
    cmp   %l6, 16
    bl    erase_loop
     nop
    ba    do_prog
     nop
erase_single:
    mov   %l3, %o0
    set   0x4001fea8, %g1
    jmpl  %g1, %o7               ! erase(dst)
     nop
do_prog:
    mov   %l4, %o0               ! src
    add   %l4, %l5, %o1          ! src_end
    mov   %l3, %o2               ! dstFlash
    set   0x4002003c, %g1        ! resident program
    jmpl  %g1, %o7               ! program(src, src_end, dst)
     nop
    add   %l2, 12, %l2
    subcc %l1, 1, %l1
    bne   chunk_loop
     nop
done:
    set   0x80007ff4, %g1        ! completion signal for the harness
    set   0xc0ded00d, %g2
    st    %g2, [%g1]
    ta    0                      ! clean emulator halt (ET=0)
     nop
