/* MicroPython on the CheerTek CT952/CT909 (big-endian SPARC V8), running
 * under ct952emu. Milestone: run a Python script and print over UART. */
#include <stdint.h>
#include <string.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "shared/runtime/pyexec.h"

#if MICROPY_ENABLE_COMPILER
void do_str(const char *src, mp_parse_input_kind_t input_kind) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src, strlen(src), 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t parse_tree = mp_parse(lex, input_kind);
        mp_obj_t module_fun = mp_compile(&parse_tree, source_name, true);
        mp_call_function_0(module_fun);
        nlr_pop();
    } else {
        // uncaught exception
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    }
}
#endif

static char *stack_top;
#if MICROPY_ENABLE_GC
static char heap[MICROPY_HEAP_SIZE];
#endif

// A tiny startup script, run once before the REPL: bring up the OSD plane and
// print a hint, so the user drops into a prompt with `ct952` ready to draw.
static const char *startup_script =
    "import ct952\n"
    "ct952.init()\n"
    "print('ct952 OSD ready:', ct952.WIDTH, 'x', ct952.HEIGHT, '-- try ct952.fill(9)')\n"
    ;

// USB HID keyboard bring-up (modusb_kbd.c): enumerate + configure a keyboard on
// EHCI port 0 so the REPL below can read from it. Returns 1 if one attached.
extern int usb_kbd_bringup(void);

// Disable the hardware watchdog (modct952.c) -- must run before mp_init on real
// silicon, or the SoC resets us mid-boot.
extern void ct952_watchdog_off(void);

// Entry from start.S (after .data copy / .bss zero / stack set up).
int mpy_main(void) {
    int stack_dummy;
    stack_top = (char *)&stack_dummy;

    mp_hal_stdout_tx_strn("\n[ct952] MicroPython booting...\n", 31);

    #if MICROPY_ENABLE_GC
    gc_init(heap, heap + sizeof(heap));
    #endif
    mp_init();

    // Bring up a USB keyboard (if attached) so the REPL can read from it.
    if (usb_kbd_bringup()) {
        mp_hal_stdout_tx_strn("[ct952] USB keyboard ready -- type Python below\n", 48);
    } else {
        mp_hal_stdout_tx_strn("[ct952] no USB keyboard; REPL reads UART1 RX\n", 45);
    }

    #if MICROPY_ENABLE_COMPILER
    // One-time startup, then an interactive REPL reading from the USB keyboard
    // and UART1. This is the branch's goal: a live Python prompt on the DVD
    // player, driven by a real USB keyboard through the modeled EHCI stack.
    do_str(startup_script, MP_PARSE_FILE_INPUT);
    for (;;) {
        if (pyexec_friendly_repl() != 0) {
            break;   // Ctrl-D / soft reset
        }
    }
    #endif

    mp_deinit();
    mp_hal_stdout_tx_strn("[ct952] done.\n", 14);

    // Signal completion to the emulator's test protocol and spin.
    *(volatile uint32_t *)0x80007FF0 = 0;
    *(volatile uint32_t *)0x80007FF4 = 0xC0DED00D;
    for (;;) {
    }
    return 0;
}

/* Entry for the embedded "app" build (start_app.S): MicroPython launched INSIDE
 * a booted CT952 firmware, in place of a firmware app. Unlike mpy_main() it does
 * not run the emulator completion protocol -- it just brings up the interpreter
 * (and a USB keyboard) and drops into the REPL, from which the live firmware is
 * reachable via ct952.peek32/poke32/call. */
int pyapp_main(void) {
    int stack_dummy;
    stack_top = (char *)&stack_dummy;

    /* FIRST: kill the hardware watchdog. On real silicon the AP loader leaves it
     * armed (a normal upgrade-AP flashes and reboots within its window); our
     * bare-metal takeover never pets it, so the SoC resets a fraction of a second
     * in -> boot loop. Disable it before anything slow (mp_init / GC). The
     * emulator has no watchdog on this path, which is why it never showed up. */
    ct952_watchdog_off();

    mp_hal_stdout_tx_strn("\n[pyapp] MicroPython launched inside the firmware\n", 49);

    #if MICROPY_ENABLE_GC
    gc_init(heap, heap + sizeof(heap));
    #endif
    mp_init();

    #if MICROPY_ENABLE_COMPILER
    // Bind the hardware module and bring up the ON-SCREEN console, so the REPL is
    // visible on the frame's display (no serial cable needed). After ct952.init()
    // every print()/REPL byte is mirrored to the OSD plane.
    do_str("import ct952\n"
           "ct952.init()\n"
           "print('MicroPython on the CT952 frame -- ct952.peek32/poke32/call ready')\n"
           "print('OSD console', ct952.WIDTH, 'x', ct952.HEIGHT, '  >>>')\n",
           MP_PARSE_FILE_INPUT);

    /* Default embedded script -- runs with NO keyboard, so the frame is useful
     * standalone. It is INVESTIGATIVE: it prints the display registers that could
     * account for the two things still unexplained about this display (HW
     * reference 10.22/10.24): the scanout pitch being 292 while VCR22/VCR23 both
     * say 308, and the OSD's absolute vertical limit at panel line ~139. Dumped:
     * H_REQ (DRAM accesses per line), REDUNDANT (extra first access), the V/H
     * scaling registers, LB_CR1/CR2 (OSD line-buffer control -- leading suspect
     * for the ~139 limit), VCR25 (OSD upscaling) and MEM_LINE.
     *
     * NOTE: the hook removed here probed a "PYSC" header at 0x40740000, which is
     * OUTSIDE this part's 2 MB DRAM (0x40000000..0x40200000) -- an out-of-bounds
     * read on real silicon that only ever worked under the emulator. */
    static const char investigate[] =
        /* hand-rolled hex: this port builds at MICROPY_CONFIG_ROM_LEVEL_MINIMUM,
         * where MICROPY_PY_BUILTINS_STR_OP_MODULO is 0, so "'%08X' % v" raises
         * TypeError (it silently produced no output on the first attempt). */
        "import ct952\n"
        "D='0123456789ABCDEF'\n"
        "def h(a):\n"
        "    v=ct952.peek32(a)\n"
        "    s=''\n"
        "    for i in range(8):\n"
        "        s=D[v&15]+s\n"
        "        v>>=4\n"
        "    return s\n"
        /* LB_CR1 is CLOSED: the write-back test read 00300000, so the write stuck,
         * and the pitch stayed 292 -- LB_CR1's low field genuinely does not set the
         * scanout pitch. Those lines are gone; the remaining registers are recorded in
         * the hardware reference, so the console rows they used are now free for the
         * USB enumeration diagnostics printed below. */
        "print('mpy ok')\n"
;
    mp_hal_stdout_tx_strn("[pyapp] embedded investigate script\n", 36);
    do_str(investigate, MP_PARSE_FILE_INPUT);

    /* Milestone 3: bring up the USB keyboard and hand the frame an interactive
     * REPL on its own screen -- no serial cable, no host. If no keyboard is found
     * the REPL still reads UART1 RX, so behaviour degrades rather than hanging.
     *
     * RE-ENABLE THE USB CLOCKS FIRST. aploader.c:134-139 runs
     *     HAL_PowerControl(HAL_POWER_USB, HAL_POWER_SAVE); USB_HCExit();
     * for the USB/servo source before jumping to the AP, and HAL_POWER_SAVE sets
     * PLAT_UCLK48M_USB_DISABLE|PLAT_HCLK_USB_DISABLE in
     * REG_PLAT_CLK_GENERATOR_CONTROL (0x80000300, hsystem.c:562-584). With those
     * clocks gated the EHCI registers are dead, so usb_kbd_bringup() sees no port
     * connection and gives up -- exactly what happened on the first attempt.
     * Clearing the two bits is the documented inverse of HAL_POWER_NORMAL, and is
     * harmless if they were already clear. */
    {
        volatile uint32_t *clkgen  = (volatile uint32_t *)0x80000300u; /* CLK_GENERATOR_CONTROL */
        volatile uint32_t *rst_dis = (volatile uint32_t *)0x80000304u; /* RESET_CONTROL_DISABLE */

        /* RELEASE USB FROM RESET -- this is why the whole USB register block read
         * back as zeros on hardware. aploader.c:471-473, immediately before jumping
         * to the AP, does:
         *     REG_PLAT_RESET_CONTROL_ENABLE = INT_SET_ALL & ~(DSU1|TIMER|SERVO|
         *                                                     VOU|VOU2|PROM)
         * i.e. it ASSERTS reset on every block except those six. USB is not among
         * them, so the controller is held in reset and reads 0 (that is also why the
         * display keeps working: VOU/VOU2 ARE in the keep-list).
         * The firmware's own idiom for bringing a block back is to write the
         * matching *_DISABLE bit to RESET_CONTROL_DISABLE (see input.c:1008-1009,
         * gdi.c:3186-3187, hsystem.c:251-254). */
        *rst_dis = 0x0C000000u;   /* PLAT_RESET_USB_DISABLE | USBCLKCKT_DISABLE */
        for (volatile int i = 0; i < 100000; i++) { }

        /* then ungate the USB clocks (HAL_POWER_SAVE gated them, hsystem.c:562+) */
        *clkgen = *clkgen & ~0x01800000u;               /* UCLK48M + HCLK for USB */
        for (volatile int i = 0; i < 400000; i++) { }   /* let the clocks/PHY settle */
    }
    /* Retry bringup: on real silicon the port takes time to report a connection
     * after the clocks come back (and after USB_HCExit() tore the controller down),
     * so a single attempt can lose the race even with a keyboard plugged in. Each
     * attempt does a full HCRESET, so retrying is safe. */
    /* The clock setup the STOCK FIRMWARE's USB init does and we never did. Its
     * USB_HCInit path (0xad64c) runs, for every mode:
     *     REG_PLAT_CLK_FREQ_CONTROL1 (0x80000308) |= 0x04008000
     * i.e. bits 26 and 15. Neither is an audio divider (hadac.c preserves both in its
     * 0xC7C08000 mask) and bit 24 is the video clock, so these are the USB side. The AP
     * loader calls USB_HCExit() before jumping to us, so whatever the firmware set up
     * while enumerating during boot has been torn down again.
     *
     * The USB PHY runs off UPLL: hsystem.c MODE_UPLL programs it as
     *     (0 << 20) + (0 << 18) + (1 << 11) + 14   -> "Fout = 288" (288/6 = 48MHz)
     * so if UPLL reads back 0 it is not running and the PHY has no clock at all. Both
     * values are reported below rather than assumed. */
    {
        volatile uint32_t *clkfreq1 = (volatile uint32_t *)0x80000308u;
        volatile uint32_t *upll     = (volatile uint32_t *)0x80000318u;
        *clkfreq1 = *clkfreq1 | 0x04008000u;
        if (*upll == 0u) {
            *upll = (0u << 20) | (0u << 18) | (1u << 11) | 14u;   /* Fout = 288MHz */
            for (volatile int i = 0; i < 200000; i++) { }         /* let it lock */
        }
    }

    int kbd_ok = 0;
    extern int usb_kbd_failstep(void);
    extern uint32_t usb_kbd_dbg(int);
    {
        int tries;
        /* A full retry re-resets the root port, which is worth doing twice before
         * declaring failure; the descriptor read itself already retries internally. */
        for (tries = 0; tries < 2 && !kbd_ok; tries++) {
            kbd_ok = usb_kbd_bringup();
            if (!kbd_ok) for (volatile int i = 0; i < 600000; i++) { }
        }
    }
    if (kbd_ok) {
        mp_hal_stdout_tx_strn("[pyapp] USB keyboard ready\n", 27);
    } else {
        /* name the failing step; the per-attempt lines scroll off the 7-row console */
        mp_printf(&mp_plat_print, "[pyapp] no USB kbd FAILSTEP=%d\n",
                  usb_kbd_failstep());
        /* One short line, printed LAST so it cannot be cut off or wrapped: the
         * STATUS BYTE of each control stage plus the QH overlay token. Which stage
         * halted is the discriminator -- a halted SETUP means the device rejected
         * the request outright, whereas a good SETUP with a halted DATA stage means
         * it accepted the request and then refused to return the descriptor. */
        /* FULL tokens, not just the status byte. TotalBytes lives in bits 30:16, and it
         * is the number that decides whether "the device ACKed our SETUP" was ever true:
         * a SETUP qTD that retires with TotalBytes still 8 sent NOTHING, which would mean
         * the bus never carried a packet and every protocol conclusion drawn from s=00 is
         * void. A=SETUP token, B=DATA token. A=xx00xxxx means the 8 bytes went out. */
        mp_printf(&mp_plat_print, "A=%08x B=%08x\n",
                  (unsigned)usb_kbd_dbg(0), (unsigned)usb_kbd_dbg(5));
        /* K = REG_PLAT_CLK_FREQ_CONTROL1, U = UPLL. If K is missing bits 26/15 the write
         * above did not stick; if U is 0 the USB PHY has no 48MHz clock. */
        mp_printf(&mp_plat_print, "K=%08x U=%08x\n",
                  (unsigned)*(volatile uint32_t *)0x80000308u,
                  (unsigned)*(volatile uint32_t *)0x80000318u);
        /* The SETUP packet as the controller actually fetched it from DMA memory. A
         * device ACKs any well-formed packet and then STALLs a request it cannot
         * parse, so garbage here produces exactly the s=00 d=40 we are chasing.
         * Expect 8006000100000800. */
        {
            extern uint32_t usb_kbd_dv(int);
            extern uint32_t usb_kbd_nodev(void);
            extern uint32_t usb_kbd_portsc(void);
            extern uint32_t usb_kbd_barein(void);
            extern uint32_t usb_kbd_rx(void);
            extern uint32_t usb_kbd_ttctrl(void);
            /* The SETUP bytes read back correct (8006000100000800) on hardware, so that
             * line is gone; V= keeps the six-configuration sweep and R= whatever landed in
             * the IN buffer. */
            mp_printf(&mp_plat_print, "V=%02x%02x%02x%02x%02x%02x R=%08x\n",
                      (unsigned)(usb_kbd_dv(0) & 0xFF), (unsigned)(usb_kbd_dv(1) & 0xFF),
                      (unsigned)(usb_kbd_dv(2) & 0xFF), (unsigned)(usb_kbd_dv(3) & 0xFF),
                      (unsigned)(usb_kbd_dv(4) & 0xFF), (unsigned)(usb_kbd_dv(5) & 0xFF),
                      (unsigned)usb_kbd_rx());
            /* LAST line, because only the last one is reliably readable on a 7-row
             * console: the no-device control experiment first (0x48/0x68 = the bus
             * really times out when nobody answers, so 0x40 elsewhere is a genuine
             * device STALL; 0x40 here means the controller halts regardless of any
             * device), then the zero-length probe and the port state. */
            /* i= is the decisive one: a lone IN to endpoint 0 with nothing pending.
             * A device answers that with NAK, which is invisible in the status, so the
             * qTD should stay ACTIVE (0x80) and time out. 0x40 instead means every IN
             * halts no matter what the device says, i.e. IN completion itself is
             * broken rather than the device rejecting our requests. */
            /* The firmware's board flag at 0x40040f0c read back 00, so the stock stack
             * does NOT force full-speed connect on this board and PORTSC bit 24 is not
             * the answer. T= replaces it: the TTCTRL readback, which says whether the
             * embedded TT's hub address finally stuck at the right register (+0x1C). */
            mp_printf(&mp_plat_print, "i=%02x n=%02x z=%02x T=%08x P=%08x\n",
                      (unsigned)(usb_kbd_barein() & 0xFF),
                      (unsigned)(usb_kbd_nodev() & 0xFF),
                      (unsigned)(usb_kbd_dv(7) & 0xFF),
                      (unsigned)usb_kbd_ttctrl(),
                      (unsigned)usb_kbd_portsc());
        }
    }
    /* The register dump that used to run here has been REMOVED, and that matters for
     * the diagnostics above: it printed three more lines after them, which on a 7-row
     * scrolling console is what left only a single line of USB state readable. PORTSC
     * is now reported inside that summary as P=, and CAP/MODE/CLK/RST are already
     * recorded in the hardware reference (10.31-10.33). */
    for (;;) {
        if (pyexec_friendly_repl() != 0) break;
    }
    #endif

    mp_deinit();
    for (;;) {
    }
    return 0;
}

#if MICROPY_ENABLE_GC
void gc_collect(void) {
    // Flush the SPARC register windows to the stack (ST_FLUSH_WINDOWS, handled
    // by start*.S) so GC roots living only in unspilled register windows become
    // visible to the C-stack scan below. Without this, a collection triggered
    // deep in the VM (e.g. from a C module) can free a still-live object.
    __asm__ __volatile__("ta 3" ::: "memory");
    gc_collect_start();
    void *dummy;
    gc_collect_root(&dummy, ((mp_uint_t)stack_top - (mp_uint_t)&dummy) / sizeof(mp_uint_t));
    gc_collect_end();
}
#endif

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    mp_raise_OSError(MP_ENOENT);
}

mp_import_stat_t mp_import_stat(const char *path) {
    return MP_IMPORT_STAT_NO_EXIST;
}

void nlr_jump_fail(void *val) {
    mp_hal_stdout_tx_strn("[ct952] FATAL: nlr_jump_fail\n", 29);
    for (;;) {
    }
}

void MP_NORETURN __fatal_error(const char *msg) {
    mp_hal_stdout_tx_strn("[ct952] FATAL: ", 15);
    mp_hal_stdout_tx_strn(msg, strlen(msg));
    mp_hal_stdout_tx_strn("\n", 1);
    for (;;) {
    }
}

#ifndef NDEBUG
// glibc's assert() macro expands to __assert_fail; provide it (no libc here).
void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function) {
    (void)file; (void)line; (void)function;
    mp_hal_stdout_tx_strn("[ct952] assert failed: ", 23);
    mp_hal_stdout_tx_strn(assertion, strlen(assertion));
    __fatal_error("assert");
}
#endif
