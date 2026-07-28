#include <stdint.h>

// MicroPython build options for the CheerTek CT952/CT909 (big-endian SPARC V8).

// Minimal feature set + the cheap compiler extensions the demo uses
// (tuple assignment). These are compile-time only, no extra link deps.
#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_MINIMUM)
#define MICROPY_COMP_DOUBLE_TUPLE_ASSIGN (1)
#define MICROPY_COMP_TRIPLE_TUPLE_ASSIGN (1)

// Slice syntax (b[0:4]) -- handy for the SD/USB byte buffers at the REPL.
#define MICROPY_PY_BUILTINS_SLICE         (1)

#define MICROPY_ENABLE_COMPILER     (1)

// SPARC has no dedicated NLR implementation; use the setjmp/longjmp path
// (our setjmp.h shim maps these to the gcc builtins, no libc needed).
#define MICROPY_NLR_SETJMP          (1)

#define MICROPY_ENABLE_GC                 (1)
#define MICROPY_HELPER_REPL               (1)
#define MICROPY_MODULE_FROZEN_MPY         (0)
#define MICROPY_ENABLE_EXTERNAL_IMPORT    (0)

#define MICROPY_ALLOC_PATH_MAX            (256)

// Use the minimum headroom in the chunk allocator for parse nodes.
#define MICROPY_ALLOC_PARSE_CHUNK_INIT    (16)

// No float for the first bring-up (soft-float target; keep it simple).
#define MICROPY_FLOAT_IMPL                (MICROPY_FLOAT_IMPL_NONE)

// Long ints so 2**32 etc. work in the demo.
#define MICROPY_LONGINT_IMPL              (MICROPY_LONGINT_IMPL_MPZ)

// Disable all optional sys module features.
#define MICROPY_PY_SYS_MODULES            (0)
#define MICROPY_PY_SYS_EXIT               (0)
#define MICROPY_PY_SYS_PATH               (0)
#define MICROPY_PY_SYS_ARGV               (0)

// type definitions for the specific machine
typedef long mp_off_t;

// Freestanding: no <alloca.h>; gcc provides the builtin.
#define alloca(x) __builtin_alloca(x)

#define MICROPY_HW_BOARD_NAME "ct952"
#define MICROPY_HW_MCU_NAME   "sparc-v8-be"

// Generous heap out of the 8 MB DRAM.
#define MICROPY_HEAP_SIZE     (256 * 1024)

#define MP_STATE_PORT MP_STATE_VM
