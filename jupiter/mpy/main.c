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
        /* LB_CR1 write-back check. Clearing LB_CR1's low field did NOT move the
         * pitch (still 292), which kills the "pitch = stride - LB_CR1_low" idea --
         * but only if the write actually STUCK. So write it and read it straight
         * back: if it reads 00300010 again the register is write-protected or
         * re-driven and the test was inconclusive; if it reads 00300000 the write
         * held and LB_CR1 genuinely does not affect the pitch. Costs one line. */
        "print('LB1 before=' + h(0x80001A28))\n"
        "ct952.poke32(0x80001A28, 0x00300000)\n"
        "print('LB1 after =' + h(0x80001A28))\n"
        "ct952.poke32(0x80001A28, 0x00300010)\n"
        "print('HREQ=' + h(0x80001A08) + ' RED=' + h(0x80001A18))\n"
        "print('VSCL=' + h(0x80001A1C) + ' LB2=' + h(0x80001A2C))\n"
        "print('V22 =' + h(0x80000D88) + ' V23=' + h(0x80000D8C))\n"
        /* EHCI state, read BEFORE any bringup attempt. The keyboard works on the
         * CT952_PYAPP path but not on --apload, and rather than guess why, look at
         * the controller: CAPLENGTH/HCIVERSION says whether the block responds at
         * all, PORTSC0 bit0 (CCS) says whether a device is seen, and USBCMD bit0
         * (RS) whether it is running. On hardware this also answers the only
         * question that matters -- is the controller alive after the loader's
         * USB_HCExit() + power-down, once the clocks are restored. */
        "print('CAP=' + h(0xA0000100) + ' CMD=' + h(0xA0000110))\n"
        "print('STS=' + h(0xA0000114) + ' CFG=' + h(0xA0000150))\n"
        "print('PORT=' + h(0xA0000154) + ' CLK=' + h(0x80000300))\n";
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
        volatile uint32_t *clkgen = (volatile uint32_t *)0x80000300u;
        *clkgen = *clkgen & ~0x01800000u;               /* UCLK48M + HCLK for USB */
        for (volatile int i = 0; i < 400000; i++) { }   /* let the clocks/PHY settle */
    }
    /* Retry bringup: on real silicon the port takes time to report a connection
     * after the clocks come back (and after USB_HCExit() tore the controller down),
     * so a single attempt can lose the race even with a keyboard plugged in. Each
     * attempt does a full HCRESET, so retrying is safe. */
    int kbd_ok = 0;
    {
        int tries;
        for (tries = 0; tries < 8 && !kbd_ok; tries++) {
            kbd_ok = usb_kbd_bringup();
            if (!kbd_ok) for (volatile int i = 0; i < 600000; i++) { }
        }
    }
    if (kbd_ok) {
        mp_hal_stdout_tx_strn("[pyapp] USB keyboard ready\n", 27);
    } else {
        mp_hal_stdout_tx_strn("[pyapp] no USB kbd; REPL on UART1 RX\n", 37);
    }
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
