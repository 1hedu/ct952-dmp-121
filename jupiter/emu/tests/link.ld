/* ct952emu CPU test: trap table + code at 0 (flash), data in DRAM.
 * The trap table must sit at address 0 (TBR base = 0 after reset), so
 * start.o's .text leads and nothing (not even ELF notes) precedes it. */
ENTRY(_tt)
SECTIONS
{
    /DISCARD/ : { *(.note*) *(.comment) *(.gnu.attributes) }
    . = 0x0;
    .text : { tests/start.o (.text) *(.text*) *(.rodata*) }
    . = 0x40080000;
    .data : { *(.data*) }
    .bss  : { *(.bss*) *(COMMON) }
    /* Only flash (address 0) is loaded by the harness -- initialized
     * data would be silently lost. Keep the payload .bss-only. */
    ASSERT(SIZEOF(.data) == 0, "test payload must not have .data")
}
