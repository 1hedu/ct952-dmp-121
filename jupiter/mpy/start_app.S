/*
 * Entry + trap table for the embedded MicroPython "app" (see ct952_app.ld),
 * linked at the 4 KB-aligned window base 0x40500000.
 *
 * The emulator seizes PROC1 and jumps here while the CT952 firmware is running.
 * Rather than ride eCos's trap handlers (which are coupled to eCos thread
 * context and fault when driven by a non-eCos caller), the app installs its OWN
 * trap table here and points %tbr at it: it manages register-window overflow/
 * underflow and the ST_FLUSH_WINDOWS (ta 3) the NLR longjmp emits, exactly like
 * the standalone build. This is safe because the app runs with interrupts
 * masked (PIL=15) and never returns to the firmware, so eCos's traps are dormant
 * while Python holds the CPU. The firmware's DRAM/state stays intact and
 * inspectable; only PROC1's trap vectoring is temporarily ours.
 *
 * The seize lands on 0x40500000 = trap-table entry 0 (reset slot), which
 * branches to _app_init.
 */

#define T_NOP  nop; nop; nop

        .section .text.boot, "ax", @progbits
        .global _tt
_tt:
        /* tt 0x00: entry (seized here) */
        b       _app_init; nop; nop; nop
        /* tt 0x01..0x04 */
        b       _bad; nop; nop; nop
        b       _bad; nop; nop; nop
        b       _bad; nop; nop; nop
        b       _bad; nop; nop; nop
        /* tt 0x05: window overflow */
        b       _win_ovf; nop; nop; nop
        /* tt 0x06: window underflow */
        b       _win_unf; nop; nop; nop
        /* tt 0x07..0x82 */
        .rept   124
        b       _bad; nop; nop; nop
        .endr
        /* tt 0x83: ta 3 = ST_FLUSH_WINDOWS */
        b       _win_flush; nop; nop; nop
        /* tt 0x84..0xff */
        .rept   124
        b       _bad; nop; nop; nop
        .endr

        .global _app_start
_app_start:                     /* alias for the linker/emulator (== _tt) */
_app_init:
        /* PSR: S=1, ET=1, PIL=15, CWP=0 -- mask interrupts, traps on */
        set     0xF3000FA0, %g1
        wr      %g1, %psr
        T_NOP
        /* WIM: window 1 invalid (clean window baseline) */
        mov     2, %g1
        wr      %g1, %wim
        T_NOP
        /* TBR: point at our own trap table at the window base (4 KB aligned) */
        set     _tt, %g1
        wr      %g1, %tbr
        T_NOP
        /* fresh stack at the top of the payload window */
        set     _stack_top, %sp
        clr     %fp

        /* zero .bss (.data is already in place: the .bin loads at its VMA) */
        set     _bss_start, %l0
        set     _bss_end, %l1
1:
        cmp     %l0, %l1
        bgeu    2f
        nop
        st      %g0, [%l0]
        add     %l0, 4, %l0
        b       1b
        nop
2:
        call    pyapp_main
        nop
3:      ba      3b
        nop

_bad:
        /* unexpected trap: park (record tt in a scratch DRAM word) */
        set     0x40780000, %g2
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

        /* ---- ST_FLUSH_WINDOWS (tt 0x83, ta 3) ---- */
_win_flush:
        mov     %wim, %l3
        mov     %g1, %l5
        mov     %g0, %wim
        T_NOP
        mov     7, %g1
1:
        restore
        std     %l0, [%sp + 0]
        std     %l2, [%sp + 8]
        std     %l4, [%sp + 16]
        std     %l6, [%sp + 24]
        std     %i0, [%sp + 32]
        std     %i2, [%sp + 40]
        std     %i4, [%sp + 48]
        std     %i6, [%sp + 56]
        subcc   %g1, 1, %g1
        bne     1b
        nop
        mov     7, %g1
2:
        save
        subcc   %g1, 1, %g1
        bne     2b
        nop
        mov     %l3, %wim
        T_NOP
        mov     %l5, %g1
        add     %l2, 4, %l4
        jmp     %l2
        rett    %l4

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
