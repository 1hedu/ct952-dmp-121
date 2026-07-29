/*
 * Link map for the embedded MicroPython "app". The REAL CT952A frame has only
 * 2 MB of DRAM (16 Mbit, HAL_DRAM_16M): 0x40000000..0x40200000. The AP loader
 * copies the compressed .AP to DS_AP_CODE_AREA (0x4009a000) and keeps the
 * firmware + section table + loader globals in low DRAM, so the safe region for
 * our decompressed payload is ABOVE the AP body:
 *
 *    0x40000000..0x400b5000  firmware image + section table + AP body (keep)
 *    0x400c0000..0x401f0000  our payload: code/rodata/data/bss/heap (~1.2 MB)
 *    0x401f0000              stack top (grows down)
 *    0x401f8000..0x40200000  AP UNZIP work buffer (used during decompress only)
 *
 * (The earlier 0x40500000 window only existed because the emulator wrongly
 * modelled 8 MB; on silicon that address is 3 MB past the end of DRAM -> the
 * decompress wrote into nothing and the frame boot-looped.)
 *
 * Load address == run address; the AP loader decompresses the .bin to here.
 * start_app.S installs its own trap table, kills the watchdog, sets the stack,
 * and zeroes .bss.
 */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x400c0000, LENGTH = 0x130000   /* 0x400c0000..0x401f0000 */
}

ENTRY(_app_start)

SECTIONS
{
    . = 0x400c0000;

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
