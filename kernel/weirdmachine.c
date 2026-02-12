/*
 * Page Fault Weird Machine - Implementation
 *
 * Instruction-less computation via x86 page fault cascades.
 * The CPU is trapped in a cascade of page faults and double faults,
 * never executing a single application instruction. The TSS save/load
 * mechanism IS the computation.
 *
 * Based on Bangert/Bratus (WOOT'13) and kristerw/instless_comp.
 *
 * Memory layout (physical pages starting at PROG_BASE_ADDR):
 *   Page 0:    Stack page
 *   Page 1:    Stack page table
 *   Page 2:    GDT page table
 *   Page 3-6:  GDT (4 pages = 16KB, holds TSS descriptors)
 *   Page 7:    Initial page directory
 *   Page 8:    Initial page table for INST_ADDRESS range
 *   Page 9:    Initial instruction page (first TSS head)
 *   Page 10:   REG_CONST_ONE (constant register = 1)
 *   Page 11:   REG_DISCARD (write sink)
 *   Page 12+:  User registers (r0, r1, ...)
 *   After regs: Constant registers
 *   After consts: Instruction pages (4 pages per real instruction)
 */

#include "weirdmachine.h"
#include <stdint.h>
#include <stddef.h>

/* ========== Address Space Layout ========== */

#define STACK_ADDRESS       0x00000000   /* PDE[0] */
#define INST_ADDRESS        0x00400000   /* PDE[1] - instruction + IDT */
#define IDT_ADDRESS         INST_ADDRESS /* IDT is first page of INST range */
#define X86_BASE_ADDRESS    0x00c00000   /* PDE[3] - kernel code */
#define GDT_ADDRESS         0x01800000   /* PDE[6] - GDT */
#define X86_PD_ADDRESS      0x07c00000   /* Normal x86 page directory */
#define PROG_BASE_ADDR      0x08000000   /* All program pages */

#define PROG_BASE_PAGE      (PROG_BASE_ADDR >> 12)

/* Convert program page number to virtual address */
#define PAGE2VIRT(x)        ((uint32_t *)((PROG_BASE_ADDR) + ((x) << 12)))

/* Page table flags */
#define PG_P    0x001   /* Present */
#define PG_W    0x002   /* Writable */
#define PG_PS   0x080   /* Page Size (4MB) */

/* ========== Program Page Assignments ========== */

#define STACK_PAGE          0
#define STACK_PT_PAGE       1
#define GDT_PT_PAGE         2
#define GDT_PAGE0           3
#define GDT_PAGE1           4
#define GDT_PAGE2           5
#define GDT_PAGE3           6
#define INIT_PD             7
#define INIT_PT             8
#define INIT_INST           9
#define REG_CONST_ONE_PAGE  10
#define REG_DISCARD_PAGE    11
#define REG_R0_PAGE         12

/* Instruction page offsets within each 4-page group */
#define PD_OFF              0   /* Page directory */
#define INST_PT_OFF         1   /* Page table for INST_ADDRESS range */
#define INST_OFF            2   /* Instruction page (TSS head) */
#define IDT_OFF             3   /* IDT page */

#define PAGES_PER_INST      4

/* ========== Module State ========== */

static int num_user_regs;       /* Number of user registers (r0, r1, ...) */
static int num_const_regs;      /* Number of constant registers */
static int first_inst_page;     /* Page number of first instruction */
static int num_asm_insts;       /* Number of movdbz assembly instructions */

/* x86 kernel TSS (saved state for returning from weird machine) */
static uint32_t x86_tss[26] __attribute__((aligned(128)));

/* ========== CR/Control Register Access ========== */

static inline uint32_t read_cr0(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(uint32_t val) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(val));
}

static inline void write_cr3(uint32_t val) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val));
}

static inline uint32_t read_cr4(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(uint32_t val) {
    __asm__ volatile ("mov %0, %%cr4" : : "r"(val));
}

static inline uint32_t read_eflags(void) {
    uint32_t val;
    __asm__ volatile ("pushfl; popl %0" : "=r"(val));
    return val;
}

/* ========== Memory Utilities ========== */

static void memset32(uint32_t *dst, uint32_t val, size_t count) {
    for (size_t i = 0; i < count; i++)
        dst[i] = val;
}

/* ========== Segment Descriptor Encoding ========== */

/*
 * Encode an x86 segment descriptor (8 bytes = 2 DWORDs).
 * Type: segment type (0x9A=code, 0x92=data, 0x89=TSS available)
 * G: granularity (0=byte, 1=4KB)
 */
static void encode_seg_descr(uint32_t *p, uint32_t type, uint32_t g,
                              uint32_t base, uint32_t limit)
{
    p[0] = ((base & 0xffff) << 16) | (limit & 0xffff);
    p[1] = ((base & 0xff000000)
            | 0x00400000
            | (g << 23)
            | (limit & 0x000f0000)
            | (type << 8)
            | ((base & 0x00ff0000) >> 16));
}

/* ========== TSS Slot Rotation ========== */

/*
 * Map instruction number to one of 3 rotating TSS GDT selectors.
 * -1 = exit (return to x86 TSS at selector 0x18).
 */
static uint32_t inst_to_tss_selector(int inst_nr) {
    if (inst_nr < 0) return 0x18;   /* Exit: x86 kernel TSS */
    switch (inst_nr % 3) {
        case 0:  return 0x1ff8;
        case 1:  return 0x2ff8;
        default: return 0x3ff8;
    }
}

/*
 * Map instruction number to its TSS virtual address.
 * The TSS is placed at offset 0xFFD0 within the appropriate
 * 4KB-aligned region of the INST_ADDRESS space.
 */
static uint32_t inst_to_tss_addr(int inst_nr) {
    switch (inst_nr % 3) {
        case 0:  return INST_ADDRESS + 0x0ffd0;
        case 1:  return INST_ADDRESS + 0x1ffd0;
        default: return INST_ADDRESS + 0x2ffd0;
    }
}

/* ========== Register Setup ========== */

/*
 * Initialize a register page.
 * The register value is stored in the ESP field of the TSS,
 * shifted left by 2 (because error code push decrements ESP by 4).
 */
static void gen_reg(int reg_page, uint32_t value) {
    uint32_t *p = PAGE2VIRT(reg_page);
    memset32(p, 0, 1024);

    /* These are TSS fields at the tail end of the structure:
     * The TSS head is on the instruction page, tail on the register page.
     * Page offset 0 = TSS offset 48 (EDX)
     * Page offset 2 = TSS offset 56 (ESP) <-- register value here
     */
    p[2] = value << 2;     /* ESP = value * 4 */
    p[6] = 0x10;           /* ES = data segment selector */
    p[7] = 0x08;           /* CS = code segment selector */
    p[8] = 0x10;           /* SS = data segment selector */
    p[9] = 0x10;           /* DS = data segment selector */
    p[10] = 0x10;          /* FS = data segment selector */
    p[11] = 0x10;          /* GS = data segment selector */
    p[12] = 0;             /* LDT segment selector */
}

/* ========== Page Table Generation ========== */

/*
 * Generate a page directory for one instruction.
 * Maps: stack, instruction/IDT range, kernel code, GDT.
 */
static void generate_pagetable(uint32_t pd_page) {
    uint32_t *pde = PAGE2VIRT(pd_page);
    memset32(pde, 0, 1024);

    /* PDE[0]: Stack at 0x00000000 */
    uint32_t *pt_stack = PAGE2VIRT(STACK_PT_PAGE);
    pt_stack[0] = PG_P | PG_W | ((PROG_BASE_PAGE + STACK_PAGE) << 12);
    pde[0] = PG_P | PG_W | ((PROG_BASE_PAGE + STACK_PT_PAGE) << 12);

    /* PDE[1]: Instruction + IDT at 0x00400000 */
    uint32_t *pt_inst = PAGE2VIRT(pd_page + INST_PT_OFF);
    pt_inst[0] = PG_P | PG_W | ((PROG_BASE_PAGE + pd_page + IDT_OFF) << 12);
    pde[1] = PG_P | PG_W | ((PROG_BASE_PAGE + pd_page + INST_PT_OFF) << 12);

    /* PDE[3]: Kernel code at 0x00C00000 (4MB identity map) */
    pde[3] = PG_P | PG_PS | PG_W | (3 << 22);

    /* PDE[6]: GDT at 0x01800000 */
    uint32_t *pt_gdt = PAGE2VIRT(GDT_PT_PAGE);
    for (int i = 0; i < 4; i++)
        pt_gdt[i] = PG_P | PG_W | ((PROG_BASE_PAGE + GDT_PAGE0 + i) << 12);
    pde[6] = PG_P | PG_W | ((PROG_BASE_PAGE + GDT_PT_PAGE) << 12);

    /* PDE for PROG_BASE_ADDR: identity map program pages (4MB) */
    pde[PROG_BASE_ADDR >> 22] = PG_P | PG_PS | PG_W | (PROG_BASE_ADDR);
}

/*
 * Generate the IDT page for one instruction.
 * Sets up task gates for #PF (vector 14) and #DF (vector 8).
 */
static void generate_idt_page(uint32_t pd_page,
                               int dest_pf_inst,    /* #PF target (nonzero) */
                               int dest_df_inst)    /* #DF target (zero) */
{
    uint32_t *p = PAGE2VIRT(pd_page + IDT_OFF);
    memset32(p, 0, 1024);

    uint32_t tss_pf = inst_to_tss_selector(dest_pf_inst);
    uint32_t tss_df = inst_to_tss_selector(dest_df_inst);

    /* IDT entry 8: Double Fault (#DF) - branch-if-zero path */
    p[16] = tss_df << 16;          /* TSS selector in upper 16 bits */
    p[17] = 0xe500;                /* Task gate, Present, DPL=3 */

    /* IDT entry 14: Page Fault (#PF) - branch-not-zero path */
    p[28] = tss_pf << 16;
    p[29] = 0xe500;
}

/*
 * Generate the instruction page (TSS head).
 * Contains CR3, EIP (unmapped!), EFLAGS, and a fresh GDT descriptor
 * to clear the TSS busy bit.
 */
static void generate_inst_page(uint32_t pd_page, int inst_nr) {
    uint32_t *p = PAGE2VIRT(pd_page + INST_OFF);
    memset32(p, 0, 1024);

    uint32_t tss_addr = inst_to_tss_addr(inst_nr);

    /* TSS starts at offset 0xFD0 within this page (DWORD 1012).
     * Fields at the head of the TSS: */
    p[1019] = (PROG_BASE_PAGE + pd_page) << 12;  /* CR3: this instruction's PD */
    p[1020] = 0xfffefff;                           /* EIP: unmapped -> page fault! */
    p[1021] = read_eflags();                       /* EFLAGS */

    /* Write a fresh TSS descriptor (busy bit clear) into the position
     * that the GDT maps to. This is the key trick: the GDT page is
     * mapped through this instruction's page table, so the CPU sees
     * a non-busy TSS descriptor when it tries to task-switch. */
    encode_seg_descr(&p[1022], 0x89, 0, tss_addr, 0x67);
}

/*
 * Map the destination TSS for this instruction.
 * The destination register page becomes the tail of the next instruction's TSS.
 */
static void map_dest_tss(uint32_t pd_page, int inst_nr, int reg_page) {
    uint32_t *pt = PAGE2VIRT(pd_page + INST_PT_OFF);
    uint32_t tss_addr = inst_to_tss_addr(inst_nr);
    uint32_t seg_descr = inst_to_tss_selector(inst_nr);
    uint32_t pt_idx = (tss_addr & 0x003ff000) >> 12;

    /* Map the GDT page containing this TSS's descriptor */
    pt[pt_idx] = PG_P | PG_W | ((PROG_BASE_PAGE + GDT_PAGE0 + (seg_descr >> 12)) << 12);
    /* Map the register page as the next page (TSS tail with ESP) */
    pt[pt_idx + 1] = PG_P | PG_W | ((PROG_BASE_PAGE + reg_page) << 12);
}

/*
 * Map the source TSS for the next instruction.
 * The instruction page becomes the head, source register page the tail.
 */
static void map_src_tss(uint32_t pd_page, int next_inst_nr, int reg_page) {
    uint32_t *pt = PAGE2VIRT(pd_page + INST_PT_OFF);
    uint32_t tss_addr = inst_to_tss_addr(next_inst_nr);
    uint32_t inst_off_page = first_inst_page + next_inst_nr * PAGES_PER_INST + INST_OFF;
    uint32_t pt_idx = (tss_addr & 0x003ff000) >> 12;

    /* Map the instruction page (TSS head with CR3, EIP, EFLAGS) */
    pt[pt_idx] = PG_P | PG_W | ((PROG_BASE_PAGE + inst_off_page) << 12);
    /* Map the source register page (TSS tail with ESP = value) */
    pt[pt_idx + 1] = PG_P | PG_W | ((PROG_BASE_PAGE + reg_page) << 12);
}

/* ========== Instruction Generation ========== */

/*
 * Generate one real instruction (internal).
 * Each movdbz assembly instruction expands to 3 real instructions.
 */
static void gen_inst(int inst_nr,
                     int dest_pf_inst, int dest_df_inst,
                     int dest_reg_page,
                     int pf_input_reg_page, int df_input_reg_page)
{
    uint32_t pd_page = first_inst_page + inst_nr * PAGES_PER_INST + PD_OFF;

    generate_pagetable(pd_page);
    generate_idt_page(pd_page, dest_pf_inst, dest_df_inst);
    generate_inst_page(pd_page, inst_nr);
    map_dest_tss(pd_page, inst_nr, dest_reg_page);

    if (dest_pf_inst >= 0)
        map_src_tss(pd_page, dest_pf_inst, pf_input_reg_page);
    if (dest_df_inst >= 0)
        map_src_tss(pd_page, dest_df_inst, df_input_reg_page);
}

/*
 * Convert a user register number to a page number.
 * Handles special registers (discard, const_one).
 */
static int reg_to_page(int reg_nr) {
    if (reg_nr == WM_REG_DISCARD)   return REG_DISCARD_PAGE;
    if (reg_nr == WM_REG_CONST_ONE) return REG_CONST_ONE_PAGE;
    return REG_R0_PAGE + reg_nr;
}

/* ========== GDT and TSS Setup ========== */

static void init_gdt(uint32_t *gdt) {
    memset32(gdt, 0, 4096);  /* 4 pages * 1024 DWORDs = 16KB */

    /* Null descriptor at index 0 */
    /* Selector 0x08: Code segment (ring 0, flat) */
    encode_seg_descr(&gdt[2], 0x9A, 1, 0, 0xfffff);
    /* Selector 0x10: Data segment (ring 0, flat) */
    encode_seg_descr(&gdt[4], 0x92, 1, 0, 0xfffff);
    /* Selector 0x18: x86 kernel TSS (for returning from weird machine) */
    encode_seg_descr(&gdt[6], 0x89, 0, (uint32_t)x86_tss, 0x67);

    /* Three rotating TSS slots at the end of GDT pages 0, 1, 2 */
    encode_seg_descr(&gdt[0x7fe], 0x89, 0, 0x40ffd0, 0x67);   /* Selector 0x1FF8 */
    encode_seg_descr(&gdt[0xbfe], 0x89, 0, 0x41ffd0, 0x67);   /* Selector 0x2FF8 */
    encode_seg_descr(&gdt[0xffe], 0x89, 0, 0x42ffd0, 0x67);   /* Selector 0x3FF8 */
}

static void init_tss(void) {
    for (int i = 0; i < 26; i++)
        x86_tss[i] = 0;
    x86_tss[7] = X86_PD_ADDRESS;   /* CR3: kernel's page directory */
}

/* ========== Initial Paging ========== */

/*
 * Set up the initial x86 page directory.
 * Identity maps the first 2GB using 4MB pages (PSE).
 */
static void init_x86_paging(void) {
    uint32_t *pde = (uint32_t *)X86_PD_ADDRESS;
    for (int i = 0; i < 512; i++)
        pde[i] = PG_P | PG_PS | PG_W | ((uint32_t)i << 22);

    write_cr3(X86_PD_ADDRESS);
    write_cr4(read_cr4() | 0x10);       /* Enable PSE */
    write_cr0(read_cr0() | (1u << 31)); /* Enable paging */
}

/* ========== External Assembly: Load GDT and TR ========== */

extern void set_gdtr(uint32_t table_limit, uint32_t base_addr);

/* Load IDTR with the IDT address and limit */
static void set_idtr(uint32_t base_addr, uint16_t table_limit) {
    struct __attribute__((__packed__)) {
        uint16_t limit;
        uint32_t base;
    } idtr;
    idtr.limit = table_limit;
    idtr.base = base_addr;
    __asm__ volatile ("lidtl %0" : : "m"(idtr) : "memory");
}

/* ========== Public API ========== */

void wm_setup(void) {
    /* Enable paging with identity mapping */
    init_x86_paging();

    /* Initialize TSS for returning from weird machine */
    init_tss();

    /* Initialize GDT at physical GDT_ADDRESS (accessible under identity mapping).
     * This is needed because set_gdtr/lgdt reads from this address before
     * we switch to the weird machine's page directories. */
    init_gdt((uint32_t *)GDT_ADDRESS);

    /* Load GDTR and Task Register */
    set_gdtr(4 * 4096 - 1, GDT_ADDRESS);

    /* Load IDTR - the IDT is mapped at IDT_ADDRESS (= INST_ADDRESS = 0x00400000)
     * in each instruction's page directory */
    set_idtr(IDT_ADDRESS, 0x7ff);

    /* Set default register/instruction counts */
    num_user_regs = 0;
    num_const_regs = 0;
    num_asm_insts = 0;
}

void wm_write_reg(int reg_nr, uint32_t value) {
    int page;
    if (reg_nr >= 0) {
        page = REG_R0_PAGE + reg_nr;
        if (reg_nr >= num_user_regs)
            num_user_regs = reg_nr + 1;
    } else {
        return;  /* Can't write to special regs this way */
    }
    gen_reg(page, value);
}

uint32_t wm_read_reg(int reg_nr) {
    int page = REG_R0_PAGE + reg_nr;
    uint32_t val = *(PAGE2VIRT(page) + 2);
    return val >> 2;
}

int wm_alloc_const(uint32_t value) {
    int reg_nr = num_user_regs + num_const_regs;
    int page = REG_R0_PAGE + reg_nr;
    gen_reg(page, value);
    num_const_regs++;
    return reg_nr;
}

void wm_gen_movdbz(int asm_inst, int dest_reg, int src_reg, int dest_nz, int dest_z) {
    /* Calculate first_inst_page based on current register allocation */
    first_inst_page = REG_R0_PAGE + num_user_regs + num_const_regs;

    int dest_page = reg_to_page(dest_reg);
    int src_page = reg_to_page(src_reg);

    int i = asm_inst * 3;   /* Real instruction base number */

    /* NOP 0: read source, write to discard, both paths -> real inst */
    gen_inst(i, i + 2, i + 2, REG_DISCARD_PAGE, src_page, src_page);

    /* NOP 1: same as NOP 0 (needed for TSS rotation) */
    gen_inst(i + 1, i + 2, i + 2, REG_DISCARD_PAGE, src_page, src_page);

    /* REAL: read const_one, write to dest, branch to targets */
    int real_dest_nz = (dest_nz < 0) ? -1 : dest_nz * 3;
    int real_dest_z  = (dest_z < 0)  ? -1 : dest_z * 3 + 1;

    gen_inst(i + 2, real_dest_nz, real_dest_z,
             dest_page, REG_CONST_ONE_PAGE, REG_CONST_ONE_PAGE);

    if (asm_inst >= num_asm_insts)
        num_asm_insts = asm_inst + 1;
}

void wm_run(void) {
    /* Initialize special registers */
    gen_reg(REG_CONST_ONE_PAGE, 1);
    gen_reg(REG_DISCARD_PAGE, 0);

    /* Finalize first_inst_page */
    first_inst_page = REG_R0_PAGE + num_user_regs + num_const_regs;

    /* Set up the initial page directory using the same function as
     * real instructions. This maps stack, IDT, kernel, and GDT. */
    generate_pagetable(INIT_PD);

    /* Map the first instruction's source TSS into the initial PD's
     * page table so the ljmp can read it. */
    map_src_tss(INIT_PD, 0, REG_CONST_ONE_PAGE);

    /* Initialize program GDT pages with the same descriptors.
     * These are what the weird machine sees via per-instruction
     * page directories (which map GDT_ADDRESS to program GDT pages). */
    init_gdt(PAGE2VIRT(GDT_PAGE0));

    /* Switch to initial page directory and launch the fault cascade.
     * Under INIT_PD, GDT_ADDRESS maps to the program GDT pages. */
    write_cr3((PROG_BASE_PAGE + INIT_PD) << 12);

    /* ljmp to TSS selector 0x1FF8 triggers the first task switch.
     * The CPU saves current state into x86_tss (selector 0x18).
     * When the program exits, we resume here. */
    __asm__ volatile (
        "ljmp $0x1ff8, $0x0\n\t"
        "addl $4, %%esp"        /* Clean up error code pushed by fault */
        : : : "memory"
    );

    /* Restore normal page directory */
    write_cr3(X86_PD_ADDRESS);
}
