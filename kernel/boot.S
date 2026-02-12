/* Multiboot boot stub for PageFault Claude
 *
 * Sets up protected mode with a flat memory model, then jumps to kernel_main().
 * This is the ONLY code that runs real x86 instructions for setup.
 * Once the weird machine starts, zero application instructions execute.
 */

.set ALIGN,    1<<0             /* align loaded modules on page boundaries */
.set MEMINFO,  1<<1             /* provide memory map */
.set FLAGS,    ALIGN | MEMINFO  /* Multiboot flags */
.set MAGIC,    0x1BADB002       /* Multiboot magic number */
.set CHECKSUM, -(MAGIC + FLAGS) /* checksum */

/* Put multiboot header at the very start of .text so it's early in the file */
.section .text
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

/* Stack */
.section .bss
.align 16
stack_bottom:
.skip 16384  /* 16 KB stack */
stack_top:

/* Entry point */
.section .text
.global _start
.type _start, @function
_start:
    /* Set up stack */
    mov $stack_top, %esp

    /* Call kernel main */
    call kernel_main

    /* Halt if kernel_main returns */
    cli
1:  hlt
    jmp 1b

.size _start, . - _start
