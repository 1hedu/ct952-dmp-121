/*
 * Link map for the embedded MicroPython "app": a DRAM-resident payload that is
 * injected into a *booted* CT952 firmware and launched in place of a firmware
 * app. Everything (code, rodata, data, bss, heap, stack) lives in the runtime-
 * free high-DRAM window 0x40500000..0x40800000 (verified all-zero after boot),
 * so it never collides with eCos or the firmware's working set. Load address ==
 * run address (no XIP/flash split): the emulator DMA-copies the .bin here.
 *
 * No trap table: the payload rides the firmware's already-installed eCos trap
 * handlers (register-window overflow/underflow + ST_FLUSH_WINDOWS for the NLR
 * longjmp), so start_app.S only has to set a stack and zero .bss.
 */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x40500000, LENGTH = 3M
}

ENTRY(_app_start)

SECTIONS
{
    . = 0x40500000;

    .text : {
        KEEP(*(.text.boot))     /* _app_start pinned at the window base */
        *(.text*)
        *(.rodata*)
    } > RAM

    .data : {
        . = ALIGN(8);
        _data_start = .;
        *(.data*)
        _data_end = .;
    } > RAM

    .bss : {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    } > RAM

    . = ALIGN(16);

    /* stack grows down from the top of the window */
    _stack_top = ORIGIN(RAM) + LENGTH(RAM) - 16;
}
