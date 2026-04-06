/*
 * Minimal RISC-V crt0 for the LinkFPGA standalone firmware.
 *
 * The LiteX BIOS jumps here after loading the firmware into main_ram
 * (0x40000000). We:
 *   1. Set up the stack pointer at the top of main_ram.
 *   2. Set up the global pointer (gp) for sdata.
 *   3. Zero .bss.
 *   4. Copy .data init values from rom (no-op for us — .data is in
 *      main_ram already, the loader put the right contents there).
 *   5. Call link_app_main(). Never return.
 */

    .section .text.start, "ax", @progbits
    .global _start
    .type _start, @function

_start:
    .option push
    .option norelax
    la    gp, __global_pointer$
    .option pop

    la    sp, _fstack

    /* Zero .bss */
    la    t0, _fbss
    la    t1, _ebss
1:  beq   t0, t1, 2f
    sw    zero, 0(t0)
    addi  t0, t0, 4
    j     1b
2:

    call  link_app_main

    /* Should never return — but if we do, halt. */
3:  wfi
    j     3b

    .size _start, . - _start
