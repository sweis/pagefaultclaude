/* Linker script for PageFault Claude bare-metal kernel
 *
 * The kernel is loaded at 0x00C00000 (12MB) to match the page fault
 * weird machine's address space layout, where PDE[3] identity-maps
 * this region with a 4MB page.
 */

ENTRY(_start)

SECTIONS
{
    /* Load at 12MB - matches weird machine PDE[3] mapping */
    . = 0x00C00000;

    /* Code (includes multiboot header at the start) */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.text)
    }

    /* Read-only data */
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    /* Initialized data */
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    /* Uninitialized data (BSS) */
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
}
