/*
 * Page Fault Weird Machine - Header
 *
 * Implements instruction-less computation via x86 page fault cascades.
 * Based on "The Page-Fault Weird Machine" by Bangert, Bratus et al. (WOOT'13)
 * and kristerw/instless_comp.
 */

#ifndef WEIRDMACHINE_H
#define WEIRDMACHINE_H

#include <stdint.h>

/* Maximum number of registers (including constants) */
#define MAX_REGISTERS     64

/* Maximum number of movdbz assembly instructions */
#define MAX_ASM_INSTS     256

/*
 * Set up the page fault weird machine infrastructure.
 * Must be called after paging is enabled and the kernel is running.
 */
void wm_setup(void);

/*
 * Generate a movdbz instruction.
 *   dest_reg:  destination register number (or WM_REG_DISCARD)
 *   src_reg:   source register number (or constant register)
 *   dest_nz:   assembly instruction number to jump to if result != 0
 *   dest_z:    assembly instruction number to jump to if result == 0
 *              Use -1 for exit (return to normal x86 execution).
 */
void wm_gen_movdbz(int asm_inst, int dest_reg, int src_reg, int dest_nz, int dest_z);

/*
 * Set the initial value of a register.
 */
void wm_write_reg(int reg_nr, uint32_t value);

/*
 * Read the current value of a register.
 */
uint32_t wm_read_reg(int reg_nr);

/*
 * Allocate and initialize a constant register.
 * Returns the register number.
 */
int wm_alloc_const(uint32_t value);

/*
 * Start executing the movdbz program.
 * Returns when the program hits an "exit" instruction.
 */
void wm_run(void);

/* Special register: writes are discarded */
#define WM_REG_DISCARD    (-2)
/* Special constant: always 1 */
#define WM_REG_CONST_ONE  (-3)

#endif /* WEIRDMACHINE_H */
