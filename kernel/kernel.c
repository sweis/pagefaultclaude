/*
 * PageFault Claude - Bare-metal kernel
 *
 * A Claude API client where the core REPL logic runs entirely via x86 page
 * fault cascades (zero instructions). Network I/O is handled by a minimal
 * x86 bridge stub communicating over serial to a host-side proxy.
 *
 * This file contains the VGA, serial, and PS/2 keyboard I/O primitives,
 * plus the initial kernel entry point.
 *
 * User input comes from the PS/2 keyboard (typed in the QEMU window)
 * with serial as a fallback for automated tests.
 *
 * Wire protocol over serial:
 *   Kernel -> Proxy: "READY\n"           (kernel booted)
 *   Kernel -> Proxy: "Q:<prompt text>\n"  (query for Claude)
 *   Proxy -> Kernel: "A:<response text>\x04"  (answer, terminated by EOT)
 *   Kernel -> Proxy: "Claude: <text>\n"  (response echoed for logging)
 */

#include <stdint.h>
#include <stddef.h>
#include "weirdmachine.h"

/* ========== VGA Text Mode ========== */

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile uint16_t *)0xB8000)

#define VGA_COLOR(fg, bg) ((bg) << 4 | (fg))
#define VGA_ENTRY(c, color) ((uint16_t)(c) | (uint16_t)(color) << 8)

enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14,
    VGA_WHITE = 15,
};

static size_t vga_row;
static size_t vga_col;
static uint8_t vga_color;

static void vga_init(void) {
    vga_row = 0;
    vga_col = 0;
    vga_color = VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK);

    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[y * VGA_WIDTH + x] = VGA_ENTRY(' ', vga_color);
}

static void vga_scroll(void) {
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[y * VGA_WIDTH + x] = VGA_MEMORY[(y + 1) * VGA_WIDTH + x];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = VGA_ENTRY(' ', vga_color);
    vga_row = VGA_HEIGHT - 1;
}

static void vga_putchar(char c) {
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_HEIGHT) vga_scroll();
        return;
    }
    if (c == '\r') { vga_col = 0; return; }
    if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            VGA_MEMORY[vga_row * VGA_WIDTH + vga_col] = VGA_ENTRY(' ', vga_color);
        }
        return;
    }

    VGA_MEMORY[vga_row * VGA_WIDTH + vga_col] = VGA_ENTRY(c, vga_color);
    if (++vga_col >= VGA_WIDTH) {
        vga_col = 0;
        if (++vga_row >= VGA_HEIGHT) vga_scroll();
    }
}

static void vga_puts(const char *s) {
    while (*s) vga_putchar(*s++);
}

static void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_color = VGA_COLOR(fg, bg);
}

/* ========== I/O Ports ========== */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ========== Serial Port (COM1) ========== */

#define COM1 0x3F8

static void serial_init(void) {
    outb(COM1 + 1, 0x00);    /* Disable all interrupts */
    outb(COM1 + 3, 0x80);    /* Enable DLAB */
    outb(COM1 + 0, 0x01);    /* Divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);    /* 8N1 */
    outb(COM1 + 2, 0xC7);    /* Enable FIFO */
    outb(COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static int serial_received(void) {
    return inb(COM1 + 5) & 0x01;
}

static char serial_read(void) {
    while (!serial_received()) ;
    return inb(COM1);
}

static void serial_write(char c) {
    while (!(inb(COM1 + 5) & 0x20)) ;
    outb(COM1, c);
}

static void serial_puts(const char *s) {
    while (*s) serial_write(*s++);
}

/* ========== PS/2 Keyboard (Scan Code Set 1) ========== */

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64

static int kbd_shift;

/* Scan code set 1 → ASCII (unshifted), indices 0x00–0x39 */
static const char sc_unshifted[58] = {
       0,    0,  '1', '2', '3', '4', '5', '6',  /* 0x00-0x07 */
     '7', '8', '9', '0', '-', '=', '\b', '\t',  /* 0x08-0x0F */
     'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',   /* 0x10-0x17 */
     'o', 'p', '[', ']', '\n',  0,  'a', 's',   /* 0x18-0x1F */
     'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',   /* 0x20-0x27 */
    '\'', '`',   0, '\\', 'z', 'x', 'c', 'v',  /* 0x28-0x2F */
     'b', 'n', 'm', ',', '.', '/',   0,  '*',   /* 0x30-0x37 */
       0, ' ',                                    /* 0x38-0x39 */
};

/* Scan code set 1 → ASCII (shifted) */
static const char sc_shifted[58] = {
       0,    0,  '!', '@', '#', '$', '%', '^',   /* 0x00-0x07 */
     '&', '*', '(', ')', '_', '+', '\b', '\t',   /* 0x08-0x0F */
     'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',   /* 0x10-0x17 */
     'O', 'P', '{', '}', '\n',  0,  'A', 'S',   /* 0x18-0x1F */
     'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',   /* 0x20-0x27 */
     '"', '~',   0,  '|', 'Z', 'X', 'C', 'V',  /* 0x28-0x2F */
     'B', 'N', 'M', '<', '>', '?',   0,  '*',   /* 0x30-0x37 */
       0, ' ',                                    /* 0x38-0x39 */
};

static void kbd_init(void) {
    /* Flush any pending data from the PS/2 controller */
    while (inb(KBD_STATUS_PORT) & 0x01)
        inb(KBD_DATA_PORT);
    kbd_shift = 0;
}

static int kbd_has_key(void) {
    return inb(KBD_STATUS_PORT) & 0x01;
}

/*
 * Poll both PS/2 keyboard and serial; return the first available ASCII char.
 * Keyboard input lets the user type in the QEMU window.
 * Serial fallback keeps automated tests (run_test.py) working.
 */
static char input_read(void) {
    while (1) {
        /* Check PS/2 keyboard */
        if (kbd_has_key()) {
            uint8_t sc = inb(KBD_DATA_PORT);

            /* Track shift key state */
            if (sc == 0x2A || sc == 0x36) { kbd_shift = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { kbd_shift = 0; continue; }

            /* Ignore key-release (break) codes */
            if (sc & 0x80) continue;

            /* Ignore scancodes beyond our table */
            if (sc >= 58) continue;

            char c = kbd_shift ? sc_shifted[sc] : sc_unshifted[sc];
            if (c) return c;
            continue;  /* non-printable key, keep polling */
        }

        /* Check serial (fallback for automated tests) */
        if (serial_received()) {
            return inb(COM1);
        }
    }
}

/* ========== I/O Bridge Buffer ========== */

#define PROMPT_BUF_SIZE 1024

static char prompt_buf[PROMPT_BUF_SIZE];
static size_t prompt_len;

/* Simple string comparison (up to n chars) */
static int streq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/*
 * ========== Weird Machine REPL Program ==========
 *
 * The REPL state machine runs inside the page fault weird machine.
 * It uses I/O bridge "exits" to communicate with the outside world.
 *
 * Register allocation:
 *   r0 = cmd    (I/O command: 0=exit, 1=read_byte, 2=write_byte, 3=send_query, 4=recv_response)
 *   r1 = data   (byte value for read/write)
 *   r2 = state  (internal state for quit detection)
 *   r3 = temp
 *
 * The movdbz REPL program:
 *
 *   -- PHASE 1: Signal "read a line" to bridge --
 *   L0:  movdbz r0, CMD_READ_BYTE, L1, L1   ; r0 = READ_BYTE (1)
 *   L1:  movdbz _, _, EXIT, EXIT             ; exit to bridge (read byte)
 *
 *   -- PHASE 1b: Bridge resumes here after reading a byte into r1.
 *        Bridge checks: if byte=='\n', resume at L2 (send query).
 *        If byte!='\n', resume at L0 (read more). --
 *
 *   -- PHASE 2: Signal "send query" to bridge --
 *   L2:  movdbz r0, CMD_SEND_Q, L3, L3      ; r0 = SEND_QUERY (3)
 *   L3:  movdbz _, _, EXIT, EXIT             ; exit to bridge
 *
 *   -- PHASE 2b: Bridge resumes here after sending query --
 *
 *   -- PHASE 3: Signal "receive response" to bridge --
 *   L4:  movdbz r0, CMD_RECV_R, L5, L5      ; r0 = RECV_RESPONSE (4)
 *   L5:  movdbz _, _, EXIT, EXIT             ; exit to bridge
 *
 *   -- PHASE 3b: Bridge resumes here after response is fully received --
 *   -- Loop back to PHASE 1 (read next line) --
 *   L6:  movdbz r0, 1, L0, L0               ; jmp L0 (loop)
 *
 * The bridge handles:
 *   - READ_BYTE: read char from keyboard/serial, check for newline/quit
 *   - SEND_QUERY: send accumulated prompt_buf over serial as "Q:...\n"
 *   - RECV_RESPONSE: read serial until EOT, display on VGA + echo to serial
 *   - EXIT: halt
 *
 * Constants needed: CMD values (1, 3, 4), plus 1 for the loop-back.
 */

/* Instruction labels for the movdbz REPL program */
enum {
    L_READ_CMD   = 0,   /* Set r0 = READ_BYTE */
    L_READ_EXIT  = 1,   /* Exit to bridge */
    L_SEND_CMD   = 2,   /* Set r0 = SEND_QUERY */
    L_SEND_EXIT  = 3,   /* Exit to bridge */
    L_RECV_CMD   = 4,   /* Set r0 = RECV_RESPONSE */
    L_RECV_EXIT  = 5,   /* Exit to bridge */
    L_LOOP       = 6,   /* Loop back to L0 */
    NUM_REPL_INSTS = 7,
};

/* REPL register indices */
enum {
    R_CMD  = 0,
    R_DATA = 1,
    R_TEMP = 2,
};

static void build_repl_program(void) {
    /* Allocate user registers */
    wm_write_reg(R_CMD, 0);
    wm_write_reg(R_DATA, 0);
    wm_write_reg(R_TEMP, 0);

    /* Allocate constants.
     * movdbz computes: dst = src - 1.  So to get the desired command
     * code N in R_CMD, the constant must be initialised to N + 1. */
    int c_read  = wm_alloc_const(WM_IO_READ_BYTE + 1);      /* 2 → cmd 1 */
    int c_sendq = wm_alloc_const(WM_IO_SEND_QUERY + 1);     /* 4 → cmd 3 */
    int c_recvr = wm_alloc_const(WM_IO_RECV_RESPONSE + 1);  /* 5 → cmd 4 */
    int c_one   = wm_alloc_const(1);                          /* loop-back */

    /* L0: movdbz r0, c_read, L1, L1   -- r0 = READ_BYTE */
    wm_gen_movdbz(L_READ_CMD,  R_CMD, c_read, L_READ_EXIT, L_READ_EXIT);

    /* L1: movdbz discard, discard, EXIT, EXIT  -- exit to bridge */
    wm_gen_movdbz(L_READ_EXIT, WM_REG_DISCARD, WM_REG_DISCARD, -1, -1);

    /* L2: movdbz r0, c_sendq, L3, L3  -- r0 = SEND_QUERY */
    wm_gen_movdbz(L_SEND_CMD,  R_CMD, c_sendq, L_SEND_EXIT, L_SEND_EXIT);

    /* L3: movdbz discard, discard, EXIT, EXIT  -- exit to bridge */
    wm_gen_movdbz(L_SEND_EXIT, WM_REG_DISCARD, WM_REG_DISCARD, -1, -1);

    /* L4: movdbz r0, c_recvr, L5, L5  -- r0 = RECV_RESPONSE */
    wm_gen_movdbz(L_RECV_CMD,  R_CMD, c_recvr, L_RECV_EXIT, L_RECV_EXIT);

    /* L5: movdbz discard, discard, EXIT, EXIT  -- exit to bridge */
    wm_gen_movdbz(L_RECV_EXIT, WM_REG_DISCARD, WM_REG_DISCARD, -1, -1);

    /* L6: movdbz r_temp, c_one, L0, L0  -- unconditional jump to L0 */
    wm_gen_movdbz(L_LOOP, R_TEMP, c_one, L_READ_CMD, L_READ_CMD);

    /* Pre-generate all instruction pages */
    wm_generate();
}

/*
 * I/O Bridge: services weird machine I/O requests over serial.
 *
 * The weird machine runs a state machine via page faults.
 * Each time it needs I/O, it sets r_cmd to a command code and exits.
 * The bridge reads r_cmd, performs the I/O, and resumes the weird machine
 * at the appropriate instruction.
 */
static void io_bridge_loop(void) {
    prompt_len = 0;
    int need_prompt = 1;

    serial_puts("READY\n");

    /* First launch: start at L0 (read command) */
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("[weird machine: launching fault cascade]\n");

    wm_launch();

    /* The weird machine has exited. Service I/O requests in a loop. */
    while (1) {
        uint32_t cmd = wm_read_reg(R_CMD);

        switch (cmd) {
        case WM_IO_READ_BYTE: {
            /* Show prompt on VGA at the start of a new input line */
            if (need_prompt) {
                vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
                vga_puts("pagefault> ");
                need_prompt = 0;
            }

            /* Read one byte from keyboard or serial (whichever has data) */
            char c = input_read();

            if (c == '\n' || c == '\r') {
                /* End of line */
                serial_write('\n');  /* Echo newline */
                vga_putchar('\n');

                /* Check for "quit" */
                if (prompt_len == 4 && streq(prompt_buf, "quit", 4)) {
                    vga_set_color(VGA_YELLOW, VGA_BLACK);
                    vga_puts("[quit]\n");
                    serial_puts("BYE\n");
                    return;
                }

                /* Empty line: skip query, read again */
                if (prompt_len == 0) {
                    need_prompt = 1;
                    wm_write_reg(R_CMD, 0);
                    wm_resume(L_READ_CMD);
                    break;
                }

                /* Resume weird machine at send-query phase */
                need_prompt = 1;
                wm_write_reg(R_CMD, 0);
                wm_resume(L_SEND_CMD);
            } else if (c == '\b' || c == 0x7f) {
                /* Backspace */
                if (prompt_len > 0) {
                    prompt_len--;
                    serial_write('\b');
                    serial_write(' ');
                    serial_write('\b');
                    vga_putchar('\b');
                }
                wm_write_reg(R_CMD, 0);
                wm_resume(L_READ_CMD);
            } else {
                /* Accumulate byte in buffer, echo it */
                if (prompt_len < PROMPT_BUF_SIZE - 1) {
                    prompt_buf[prompt_len++] = c;
                }
                serial_write(c);    /* Echo to serial */
                vga_set_color(VGA_WHITE, VGA_BLACK);
                vga_putchar(c);     /* Echo to VGA */

                /* Resume weird machine at read-cmd (read next byte) */
                wm_write_reg(R_CMD, 0);
                wm_resume(L_READ_CMD);
            }
            break;
        }

        case WM_IO_SEND_QUERY: {
            /* Send the accumulated buffer as a query */
            prompt_buf[prompt_len] = '\0';

            vga_set_color(VGA_DARK_GREY, VGA_BLACK);
            vga_puts("[sending query via fault cascade]\n");

            /* Wire protocol: Q:<text>\n */
            serial_puts("Q:");
            for (size_t i = 0; i < prompt_len; i++)
                serial_write(prompt_buf[i]);
            serial_write('\n');

            /* Reset buffer for next prompt */
            prompt_len = 0;

            /* Resume weird machine at recv-response phase */
            wm_write_reg(R_CMD, 0);
            wm_resume(L_RECV_CMD);
            break;
        }

        case WM_IO_RECV_RESPONSE: {
            /* Read response from proxy: "A:<text>\x04" */
            /* Skip "A:" prefix */
            char c1 = serial_read();
            char c2 = serial_read();
            (void)c1; (void)c2;

            /* Relay response bytes to VGA and serial (for proxy logging) */
            vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
            vga_puts("Claude: ");
            serial_puts("Claude: ");
            while (1) {
                char c = serial_read();
                if (c == 0x04) break;
                vga_putchar(c);
                serial_write(c);
            }
            vga_putchar('\n');
            vga_putchar('\n');
            serial_write('\n');

            /* Resume weird machine at loop-back instruction */
            wm_write_reg(R_CMD, 0);
            wm_resume(L_LOOP);
            break;
        }

        case WM_IO_EXIT:
        default:
            /* Program done or unknown command */
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_puts("[weird machine exited]\n");
            return;
        }
    }
}

/* ========== Kernel Main ========== */

void kernel_main(void) {
    vga_init();
    serial_init();
    kbd_init();

    /* Banner */
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("=== PageFault Claude v0.2 ===\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("A Claude client computed via x86 page faults.\n");
    vga_puts("The MMU is the computer. Zero instructions executed.\n");
    vga_puts("REPL state machine runs in movdbz via fault cascades.\n");
    vga_puts("--------------------------------------------\n\n");

    /* Set up the page fault weird machine */
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("[init] Setting up page fault weird machine...\n");
    wm_setup();

    /* Build the REPL program in movdbz */
    vga_puts("[init] Building movdbz REPL program...\n");
    build_repl_program();

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts("[init] Ready. Type in the QEMU window. 'quit' to exit.\n\n");

    /* Run the I/O bridge loop (weird machine controls the REPL) */
    io_bridge_loop();

    /* Halted */
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("[halted]\n");
    while (1) __asm__ volatile ("hlt");
}
