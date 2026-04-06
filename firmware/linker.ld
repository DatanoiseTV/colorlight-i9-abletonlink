/*
 * Linker script for the LinkFPGA firmware ELF.
 *
 * The firmware is a standalone executable that lives in `main_ram`
 * (SDRAM, 32 MB at 0x40000000). The LiteX BIOS in BRAM serial-boots
 * or spi-boots it into main_ram and jumps to the entry point.
 *
 * All sections (.text / .rodata / .data / .bss / heap / stack) live
 * in main_ram. We don't touch the BIOS's BRAM ROM at all.
 */

OUTPUT_FORMAT("elf32-littleriscv", "elf32-littleriscv", "elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

INCLUDE generated/output_format.ld
INCLUDE generated/regions.ld

SECTIONS
{
    .text 0x40000000 :
    {
        _ftext = .;
        *(.text.start)
        KEEP(*(.text.isr))
        *(.text .stub .text.* .gnu.linkonce.t.*)
        _etext = .;
    } > main_ram

    .rodata :
    {
        . = ALIGN(8);
        _frodata = .;
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        *(.rodata1)
        *(.got .got.*)
        *(.toc .toc.*)
        FILL(0);
        . = ALIGN(8);
        _erodata = .;
    } > main_ram

    .data :
    {
        . = ALIGN(8);
        _fdata = .;
        *(.data .data.* .gnu.linkonce.d.*)
        *(.data1)
        /* `__global_pointer$` should sit in the middle of the small-
         * data area so RISC-V's gp-relative addressing reaches both
         * positive and negative offsets. */
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.* .gnu.linkonce.s.*)
        FILL(0);
        . = ALIGN(8);
        _edata = .;
    } > main_ram

    .bss :
    {
        . = ALIGN(8);
        _fbss = .;
        *(.dynsbss)
        *(.sbss .sbss.* .gnu.linkonce.sb.*)
        *(.scommon)
        *(.dynbss)
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
        _end = .;
    } > main_ram

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
    }
}

/* Heap grows up from after .bss; stack grows down from the end of
 * main_ram. Picolibc's sbrk uses _heap_start / _heap_end. */
PROVIDE(__heap_start = _ebss);
PROVIDE(__heap_end   = ORIGIN(main_ram) + LENGTH(main_ram) - 0x10000);
PROVIDE(_fstack      = ORIGIN(main_ram) + LENGTH(main_ram) - 16);
