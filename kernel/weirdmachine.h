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

/*
 * Generate all instruction pages for the current program.
 * Called once after all wm_gen_movdbz() calls, before the run loop.
 */
void wm_generate(void);

/*
 * Run one step: launch (or resume) the weird machine.
 * The program runs until it hits an exit instruction (branch target -1).
 * The I/O bridge then inspects registers to determine what the weird
 * machine wants, performs the I/O, updates registers, and calls
 * wm_resume() to continue.
 */
void wm_launch(void);

/*
 * Resume the weird machine from a given assembly instruction.
 * Used by the I/O bridge after servicing a request.
 * entry_asm_inst: the movdbz instruction number to resume at.
 */
void wm_resume(int entry_asm_inst);

/* Special register: writes are discarded */
#define WM_REG_DISCARD    (-2)
/* Special constant: always 1 */
#define WM_REG_CONST_ONE  (-3)

/* I/O bridge command codes (written to r_cmd by the movdbz program) */
#define WM_IO_EXIT           0   /* Program done */
#define WM_IO_READ_BYTE      1   /* Read a byte from keyboard/serial */
#define WM_IO_WRITE_BYTE     2   /* Write r_data byte to serial */
#define WM_IO_SEND_QUERY     3   /* Send accumulated buffer as query */
#define WM_IO_RECV_RESPONSE  4   /* Receive response, relay bytes via serial */

#endif /* WEIRDMACHINE_H */
