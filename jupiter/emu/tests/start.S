/*
 * ct952emu CPU test -- freestanding SPARC V8 startup with a real trap
 * table, including the canonical window overflow/underflow handlers
 * (SPARC V8 manual style), so deep C recursion exercises the core's
 * trap machinery exactly the way eCos does.
 *
 * Linked at address 0 ("flash"); TBR after reset is 0, so the image
 * starts with the trap table. Result protocol: testmain()'s return
 * value is stored to 0x80007FF0, then 0xC0DED00D to 0x80007FF4, then
 * spin.
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
        /* tt 0x07.. : everything else is fatal for the test */
        .rept   249
        b       _bad; nop; nop; nop
        .endr

_start:
        /* PSR: S=1, ET=1, PIL=15, CWP=0 */
        set     0xF3000FA0, %g1
        wr      %g1, %psr
        T_NOP
        /* WIM: window 1 invalid */
        mov     2, %g1
        wr      %g1, %wim
        T_NOP
        /* stack near top of test DRAM area */
        set     0x40100000, %sp
        clr     %fp
        call    testmain
        nop
        /* store result + done flag */
        set     0x80007FF0, %g2
        st      %o0, [%g2]
        set     0xC0DED00D, %g3
        st      %g3, [%g2 + 4]
1:      ba      1b
        nop

_bad:
        /* unexpected trap: flag 0xDEADDEAD */
        set     0x80007FF0, %g2
        mov     %l1, %g4
        st      %g4, [%g2]
        set     0xDEADDEAD, %g3
        st      %g3, [%g2 + 4]
2:      ba      2b
        nop

        /* ---- window overflow (tt 5): save the next-to-invalid
         * window to its stack. Runs in the invalid window. ---- */
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

        /* ---- window underflow (tt 6): reload the window being
         * returned into from its stack. ---- */
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
