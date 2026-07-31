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
static uint32_t g_diag_mask1, g_diag_mask2, g_diag_clk;   /* startup register readback */
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

    /* Take over the interrupt controller so the stock firmware's RTOS stops running
     * underneath us. Its timer interrupt drives the scheduler that keeps the photo-frame
     * / slideshow / menu thread alive, and that thread is what painted DVD language-menu
     * text over the REPL ("crosstalk") and stole keystrokes -- worst of all with an image
     * on the card, which sends the firmware into its decode/display path. Mask every
     * source and no firmware ISR fires on this core, so the scheduler never preempts us
     * and the UI thread never runs again.
     *
     * This is exactly what the stock AP loader does to seize the machine
     * (aploader.c:474-482), and -- crucially -- it is all plain memory-mapped register
     * writes at REG_PLATFORM_ON_CHIP_BASE (0x80000000). NO privileged instructions: an
     * earlier build used rd/wr %psr, which trapped into the firmware handler (this AP is
     * unprivileged) and dumped a panic trace (RODATA, ...) onto the screen. Masking a
     * source at the controller deasserts its line, so the CPU never sees it regardless of
     * the PSR interrupt level -- the controller writes alone are sufficient. */
    {
        volatile uint32_t *P = (volatile uint32_t *)0x80000000u;
        P[0x090 / 4] = 0x00000000u;   /* INT_MASK_PRIORITY          */
        P[0x0B0 / 4] = 0xFFFFFFFFu;   /* PROC1_1ST_INT_MASK_ENABLE  -> mask all 1st-level */
        P[0x0B4 / 4] = 0x00000000u;   /* PROC1_1ST_INT_PENDING      */
        P[0x0B8 / 4] = 0xFFFFFFFFu;   /* PROC1_1ST_INT_CLEAR        */
        P[0x0D0 / 4] = 0xFFFFFFFFu;   /* PROC1_2ND_INT_MASK_ENABLE  -> mask all 2nd-level */
        P[0x0D4 / 4] = 0x00000000u;   /* PROC1_2ND_INT_PENDING      */
        P[0x0D8 / 4] = 0xFFFFFFFFu;   /* PROC1_2ND_INT_CLEAR        */

        /* Halt the SECOND processor. The emulator confirms PROC2 is the firmware's
         * secondary core (it defaults OFF there, which is why the emulator never shows
         * the crosstalk); on real silicon it runs firmware, and masking PROC1's
         * interrupts above does not reach it -- consistent with "no difference". Gate its
         * clock via a read-modify-write that sets PLAT_MCLK_PROC2_DISABLE (0x80000300
         * bit 0) without disturbing the other clock gates, which stops it dead. Our AP is
         * on PROC1, so this halts the OTHER core only, and the display scans out in
         * hardware so it does not need PROC2. (A full write to RESET_CONTROL_ENABLE would
         * disturb every other block's reset, so use the clock gate, not the reset.) */
        P[0x300 / 4] = P[0x300 / 4] | 0x00000001u;

        /* DIAGNOSTIC: read the masks / clock gate straight back so we can see on screen
         * whether the writes actually stuck (the firmware could be re-enabling them). */
        g_diag_mask1 = P[0x0B0 / 4];
        g_diag_mask2 = P[0x0D0 / 4];
        g_diag_clk   = P[0x300 / 4];
    }

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
        volatile uint32_t *clkgen  = (volatile uint32_t *)0x80000300u; /* CLK_GENERATOR_CONTROL   */
        volatile uint32_t *rst_ena = (volatile uint32_t *)0x80000324u; /* RESET_CONTROL_ENABLE    */
        volatile uint32_t *rst_dis = (volatile uint32_t *)0x80000304u; /* RESET_CONTROL_DISABLE   */

        /* USB was held in reset by the AP loader (aploader.c:471-473 asserts reset on
         * every block except DSU1/TIMER/SERVO/VOU/VOU2/PROM), which is why the whole
         * register block read back as zeros. Earlier builds just RELEASED the reset by
         * writing RESET_CONTROL_DISABLE -- but that is not what the firmware does, and it
         * left the USB PHY in an indeterminate state: it could transmit (SETUP/OUT worked
         * and the device ACKed) but never receive (every IN halted with 0 bytes, on BOTH
         * a low-speed keyboard and a high-speed stick -- host-side, not the device).
         *
         * The firmware does a full reset PULSE of the USB block (0x6398..0x63e0):
         *     REG_PLAT_RESET_CONTROL_ENABLE  = 0x04000000   ; ASSERT usb reset
         *     delay ~2ms
         *     REG_PLAT_RESET_CONTROL_DISABLE = 0x04000000   ; RELEASE usb reset
         *     delay ~2ms
         * which is what reinitialises the PHY. Match it (0x04000000 = PLAT_RESET_USB;
         * the firmware pulses only that bit here, not USBCLKCKT). */
        *clkgen = *clkgen & ~0x01800000u;    /* ungate UCLK48M + HCLK first             */
        for (volatile int i = 0; i < 100000; i++) { }
        *rst_ena = 0x04000000u;              /* ASSERT USB block reset                  */
        for (volatile int i = 0; i < 2000000; i++) { }
        *rst_dis = 0x04000000u;              /* RELEASE USB block reset (PHY reinits)   */
        for (volatile int i = 0; i < 2000000; i++) { }
    }
    /* Retry bringup: on real silicon the port takes time to report a connection
     * after the clocks come back (and after USB_HCExit() tore the controller down),
     * so a single attempt can lose the race even with a keyboard plugged in. Each
     * attempt does a full HCRESET and re-resets the root port, so retrying is safe. */
    int kbd_ok = 0;
    for (int tries = 0; tries < 2 && !kbd_ok; tries++) {
        kbd_ok = usb_kbd_bringup();
        if (!kbd_ok) for (volatile int i = 0; i < 600000; i++) { }
    }
    if (kbd_ok) {
        mp_hal_stdout_tx_strn("[pyapp] USB keyboard ready -- type Python below\n", 48);
    } else {
        extern int usb_kbd_failstep(void);
        mp_printf(&mp_plat_print, "[pyapp] no USB keyboard (step %d); REPL reads UART1\n",
                  usb_kbd_failstep());
    }
    /* ===== DIAGNOSTIC BUILD (temporary): do NOT start the REPL. =====
     * Print the register readback once and spin, so the screen shows ONLY this plus
     * whatever the FIRMWARE paints -- with no REPL running, nothing from our side can be
     * mistaken for crosstalk. Reading it:
     *   M1/M2 = FFFFFFFF and CLK bit0 = 1  -> our interrupt masks + PROC2 clock gate STUCK.
     *   If firmware menu text (FREN/GERM/RODATA...) STILL appears over this static line,
     *     the firmware is running despite the masks/gate -> it is not on PROC1 and the
     *     clock gate did not stop it, so the next fix targets a different mechanism.
     *   If the screen stays clean (just this line), the firmware was quiet and the garbage
     *     came from our own input/REPL path, which is then where I look. */
    mp_printf(&mp_plat_print, "DIAG M1=%08x M2=%08x CLK=%08x kbd=%d\n",
              (unsigned)g_diag_mask1, (unsigned)g_diag_mask2,
              (unsigned)g_diag_clk, kbd_ok);
    for (;;) {
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
