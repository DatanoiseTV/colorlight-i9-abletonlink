/*
 * Linker script for the LinkFPGA firmware.
 *
 * Memory layout matches the LiteX SoC defaults: ROM is the integrated
 * BIOS slot at 0x00000000 and SRAM is the integrated scratch at
 * 0x10000000. The actual addresses are pulled from the auto-generated
 * `generated/regions.ld` so changes in soc.py propagate automatically.
 */

OUTPUT_FORMAT("elf32-littleriscv", "elf32-littleriscv", "elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

INCLUDE generated/output_format.ld
INCLUDE generated/regions.ld

SECTIONS
{
    .text :
    {
        _ftext = .;
        *(.text.start)
        *(.text .text.* .gnu.linkonce.t.*)
        _etext = .;
    } > rom

    .rodata :
    {
        . = ALIGN(4);
        _frodata = .;
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        *(.rodata1)
        _erodata = .;
    } > rom

    .data :
    {
        . = ALIGN(4);
        _fdata = .;
        *(.data .data.* .gnu.linkonce.d.*)
        *(.data1)
        _gp = ALIGN(16);
        *(.sdata .sdata.* .gnu.linkonce.s.*)
        _edata = .;
    } > sram AT > rom

    .bss :
    {
        . = ALIGN(4);
        _fbss = .;
        *(.dynsbss)
        *(.sbss .sbss.* .gnu.linkonce.sb.*)
        *(.scommon)
        *(.dynbss)
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        _ebss = .;
    } > sram

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
    }
}

PROVIDE(_fstack = ORIGIN(sram) + LENGTH(sram) - 4);
