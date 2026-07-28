/*
 * CT952 MicroPython freestanding SPARC V8 startup.
 * Linked at address 0 ("flash"); TBR = 0 after reset, so the image begins
 * with the trap table. Reset -> _start, which sets up PSR/WIM/stack, copies
 * .data from its flash LMA into DRAM, zeroes .bss, and calls mpy_main().
 * Window overflow/underflow handlers (SPARC V8 manual style, NWIN=8) let
 * deep C recursion in the parser/compiler/VM spill and reload register
 * windows exactly as eCos does.
 */

#define T_NOP  nop; nop; nop

        .section .text
        .global _tt
_tt:
        /* tt 0x00: reset */
        b       _start; nop; nop; nop
        /* tt 0x01..0x04 */
        b       _bad; nop; nop; nop
        b       _bad; nop; nop; nop
        b       _bad; nop; nop; nop
        b       _bad; nop; nop; nop
        /* tt 0x05: window overflow */
        b       _win_ovf; nop; nop; nop
        /* tt 0x06: window underflow */
        b       _win_unf; nop; nop; nop
        /* tt 0x07.. : everything else is fatal */
        .rept   249
        b       _bad; nop; nop; nop
        .endr

        .global _start
_start:
        /* PSR: S=1, ET=1, PIL=15, CWP=0 */
        set     0xF3000FA0, %g1
        wr      %g1, %psr
        T_NOP
        /* WIM: window 1 invalid */
        mov     2, %g1
        wr      %g1, %wim
        T_NOP
        /* stack at top of DRAM */
        set     _stack_top, %sp
        clr     %fp

        /* ---- copy .data from flash LMA to DRAM VMA ---- */
        set     _data_lma, %l0          /* src */
        set     _data_start, %l1        /* dst */
        set     _data_end, %l2          /* dst end */
1:
        cmp     %l1, %l2
        bgeu    2f
        nop
        ld      [%l0], %l3
        st      %l3, [%l1]
        add     %l0, 4, %l0
        add     %l1, 4, %l1
        b       1b
        nop
2:
        /* ---- zero .bss ---- */
        set     _bss_start, %l1
        set     _bss_end, %l2
3:
        cmp     %l1, %l2
        bgeu    4f
        nop
        st      %g0, [%l1]
        add     %l1, 4, %l1
        b       3b
        nop
4:
        /* into C */
        call    mpy_main
        nop
        /* mpy_main should not return; if it does, spin */
5:      ba      5b
        nop

_bad:
        /* unexpected trap: record tt (in %l1 as psr-tt) and spin */
        set     0x80007FF0, %g2
        mov     %l1, %g4
        st      %g4, [%g2]
        set     0xDEADDEAD, %g3
        st      %g3, [%g2 + 4]
6:      ba      6b
        nop

        /* ---- window overflow (tt 5) ---- */
_win_ovf:
        mov     %wim, %l3
        sll     %l3, 7, %l4         /* NWIN-1 = 7 */
        srl     %l3, 1, %l3
        or      %l3, %l4, %l3
        and     %l3, 0xFF, %l3
        mov     %g0, %wim
        T_NOP
        save
        std     %l0, [%sp + 0]
        std     %l2, [%sp + 8]
        std     %l4, [%sp + 16]
        std     %l6, [%sp + 24]
        std     %i0, [%sp + 32]
        std     %i2, [%sp + 40]
        std     %i4, [%sp + 48]
        std     %i6, [%sp + 56]
        restore
        mov     %l3, %wim
        T_NOP
        jmp     %l1
        rett    %l2

        /* ---- window underflow (tt 6) ---- */
_win_unf:
        mov     %wim, %l3
        srl     %l3, 7, %l4
        sll     %l3, 1, %l3
        or      %l3, %l4, %l3
        and     %l3, 0xFF, %l3
        mov     %l3, %wim
        T_NOP
        restore
        restore
        ldd     [%sp + 0], %l0
        ldd     [%sp + 8], %l2
        ldd     [%sp + 16], %l4
        ldd     [%sp + 24], %l6
        ldd     [%sp + 32], %i0
        ldd     [%sp + 40], %i2
        ldd     [%sp + 48], %i4
        ldd     [%sp + 56], %i6
        save
        save
        jmp     %l1
        rett    %l2
