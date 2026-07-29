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
        /* Kill the hardware watchdog IMMEDIATELY: SYSCFG1 (0x8000031C) &= ~(1<<28).
         * The AP loader leaves it armed; a bare-metal takeover that never pets it
         * gets reset a fraction of a second in -> boot loop. Do this before the
         * (large) .bss zero below so no long op runs under an armed watchdog. */
        set     0x8000031C, %g1
        ld      [%g1], %g2
        sethi   %hi(0x10000000), %g3
        andn    %g2, %g3, %g2
        st      %g2, [%g1]

        /* ---- early screen paint: diagnostic + validates the OSD path ----------
         * Reconfigure the display to the FIRMWARE'S standard OSD plane (8bpp at
         * DS_OSDFRAME_ST 0x4005F000, stride 480, window 720x240 @ (108,23), YUV
         * palette gated by BRIGHT_CR bit24) and fill it solid white (index 1),
         * BEFORE the C runtime. If the panel turns solid white, our code runs and
         * this documented OSD config is right; the C console then clears it and
         * draws text. Uses only %g regs (no stack yet). */
        set     0x4005F000, %g2         /* g2 = DS_OSDFRAME_ST (the firmware OSD plane) */
        set     0x80001A60, %g1         /* REG_DISP_BRIGHT_CR: unlock palette RAM */
        ld      [%g1], %g3
        sethi   %hi(0x01000000), %g4
        or      %g3, %g4, %g3
        st      %g3, [%g1]
        set     0x80001C00, %g1         /* GAM_OSD palette RAM (BT.601 YUV) */
        set     0x00108080, %g3
        st      %g3, [%g1]              /* [0] = YUV black (index 0 is transparent-key anyway) */
        set     0x00EB8080, %g3
        st      %g3, [%g1 + 4]          /* [1] = YUV white */
        set     0x01010101, %g3         /* index-1 bytes (8bpp solid) */
        mov     %g2, %g4
        set     28800, %g5              /* 480*240/4 words = full plane */
12:     st      %g3, [%g4]
        subcc   %g5, 1, %g5
        bne     12b
        add     %g4, 4, %g4
        /* OSD window: position (108,23), enable | 720x240 */
        set     0x80001A50, %g1         /* REG_DISP_OSD_POS */
        set     0x0017006C, %g3
        st      %g3, [%g1]
        set     0x10F002D0, %g3         /* DISP_OSD_EN | (240<<16) | 720 */
        st      %g3, [%g1 + 4]          /* REG_DISP_OSD_SIZE (0x80001A54) */
        set     0x80000D80, %g1
        st      %g2, [%g1]              /* VCR20 = 0x4005F000 */
        st      %g2, [%g1 + 4]          /* VCR21 = 0x4005F000 */
        set     0x80000014, %g1         /* REG_PLAT_CACHE_CTRL */
        ld      [%g1], %g3
        set     0x00040000, %g4
        andn    %g3, %g4, %g3
        st      %g3, [%g1]              /* clear DCACHE_POWER_SAVING */
        set     0x00400000, %g4
        or      %g3, %g4, %g3
        st      %g3, [%g1]              /* set FLUSH_DCACHE */
        set     512, %g5
13:     subcc   %g5, 1, %g5
        bne     13b
        nop
        set     0x00040000, %g4
        or      %g3, %g4, %g3
        st      %g3, [%g1]              /* restore power saving */

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
        /* unexpected trap: park (record tt in a scratch DRAM word, in-bounds for
         * the real 2 MB frame -- top of DRAM, below the unzip buffer) */
        set     0x401f4000, %g2
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
