/* ct952emu MicroPython image.
 *  - Trap table + code + rodata + .data init image live in flash (XIP @ 0).
 *  - .data (writable) and .bss live in the 8 MB DRAM @ 0x40000000.
 *  - start.o must lead so the reset trap table sits at address 0
 *    (TBR = 0 after reset).
 */
ENTRY(_tt)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    DRAM  (rwx) : ORIGIN = 0x40000000, LENGTH = 8M
}

SECTIONS
{
    /DISCARD/ : { *(.note*) *(.comment) *(.gnu.attributes) }

    .text 0x0 :
    {
        *start.o (.text)
        *(.text*)
        *(.rodata*)
        . = ALIGN(8);
    } > FLASH
    _data_lma = .;   /* load address of .data in flash */

    .data 0x40000000 : AT(_data_lma)
    {
        . = ALIGN(8);
        _data_start = .;
        *(.data*)
        . = ALIGN(8);
        _data_end = .;
    } > DRAM

    .bss (NOLOAD) :
    {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    } > DRAM

    /* Stack grows down from the top of DRAM. */
    _stack_top = ORIGIN(DRAM) + LENGTH(DRAM) - 16;
}
