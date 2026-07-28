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

#if MICROPY_ENABLE_GC
void gc_collect(void) {
    // Scan the C stack for GC roots. (SPARC register-window roots are
    // handled by flushing windows in start.S's helper before entry to
    // collection-heavy paths; simple scripts keep roots on the C stack.)
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
