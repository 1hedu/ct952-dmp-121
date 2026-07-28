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

// The demo script the milestone runs: exercise the interpreter, then draw
// to the panel through the ct952 display module -- Python pixels on a DVD
// player's OSD plane.
static const char *demo_script =
    "print('hello from MicroPython on a CT952 DVD player!')\n"
    "import ct952\n"
    "W = ct952.WIDTH\n"
    "H = ct952.HEIGHT\n"
    "print('OSD panel:', W, 'x', H)\n"
    "ct952.init()\n"
    "# palette: 0=bg, 1..8=colour bars, 9=white, 10=navy, 11=cyan, 16..47=grey ramp\n"
    "ct952.palette(0, 0x101018)\n"
    "bars = [0xff3030,0xff9020,0xf0e000,0x30c040,0x2090ff,0x5030ff,0xc040ff,0xf0f0f0]\n"
    "for i in range(len(bars)):\n"
    "    ct952.palette(1 + i, bars[i])\n"
    "ct952.palette(9, 0xffffff)\n"
    "ct952.palette(10, 0x101840)\n"
    "ct952.palette(11, 0x40d0ff)\n"
    "for i in range(32):\n"
    "    s = i * 255 // 31\n"
    "    ct952.palette(16 + i, (s << 16) | (s << 8) | s)\n"
    "ct952.fill(0)\n"
    "# eight colour bars across the top band\n"
    "bw = W // 8\n"
    "for i in range(8):\n"
    "    ct952.rect(i * bw, 8, bw - 2, 70, 1 + i)\n"
    "# a 32-step grey gradient bar\n"
    "gw = W // 32\n"
    "for i in range(32):\n"
    "    ct952.rect(i * gw, 88, gw, 28, 16 + i)\n"
    "# a navy 'dialog' with a cyan border, drawn entirely in Python\n"
    "ct952.rect(40, 128, W - 80, H - 152, 11)\n"
    "ct952.rect(44, 132, W - 88, H - 160, 10)\n"
    "# a bar-chart of the first Fibonacci numbers, computed in Python\n"
    "fib = [1, 1]\n"
    "for i in range(9):\n"
    "    fib.append(fib[len(fib) - 1] + fib[len(fib) - 2])\n"
    "print('fib:', fib)\n"
    "base = H - 30\n"
    "top = fib[len(fib) - 1]\n"
    "for i in range(len(fib)):\n"
    "    bh = fib[i] * 80 // top\n"
    "    ct952.rect(60 + i * 32, base - bh, 24, bh, 1 + (i % 8))\n"
    "print('drawn: colour bars + grey ramp + dialog + Fibonacci bar chart')\n"
    ;

// Entry from start.S (after .data copy / .bss zero / stack set up).
int mpy_main(void) {
    int stack_dummy;
    stack_top = (char *)&stack_dummy;

    mp_hal_stdout_tx_strn("\n[ct952] MicroPython booting...\n", 31);

    #if MICROPY_ENABLE_GC
    gc_init(heap, heap + sizeof(heap));
    #endif
    mp_init();

    #if MICROPY_ENABLE_COMPILER
    do_str(demo_script, MP_PARSE_FILE_INPUT);
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
