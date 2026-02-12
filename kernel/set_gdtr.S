/*
 * set_gdtr - Load GDTR and segment registers, then load Task Register
 *
 * void set_gdtr(uint32_t table_limit, uint32_t base_addr);
 *
 * Sets up the GDT pointer, reloads all segment registers with the
 * new selectors, and loads the Task Register with the x86 TSS (0x18).
 */

.global set_gdtr
.type set_gdtr, @function

set_gdtr:
    sub     $0x10, %esp

    /* Build the 6-byte GDTR structure on the stack */
    mov     0x14(%esp), %eax        /* table_limit */
    mov     %ax, 0xa(%esp)          /* GDTR.limit (16 bits) */
    mov     0x18(%esp), %eax        /* base_addr */
    mov     %eax, 0xc(%esp)         /* GDTR.base (32 bits) */

    /* Load GDTR */
    lgdtl   0xa(%esp)

    add     $0x10, %esp

    /* Far jump to reload CS with new code segment selector (0x08) */
    ljmp    $0x08, $.Lreload_cs

.Lreload_cs:
    /* Reload data segment registers */
    mov     $0x10, %ax              /* Data segment selector */
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* Load Task Register with x86 TSS selector (0x18) */
    mov     $0x18, %ax
    ltr     %ax

    ret

.size set_gdtr, . - set_gdtr
