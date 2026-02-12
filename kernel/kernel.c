/*
 * PageFault Claude - Bare-metal kernel
 *
 * A Claude API client where the core REPL logic runs entirely via x86 page
 * fault cascades (zero instructions). Network I/O is handled by a minimal
 * x86 bridge stub communicating over serial to a host-side proxy.
 *
 * This file contains the VGA and serial I/O primitives, plus the initial
 * kernel entry point.
 *
 * Wire protocol over serial:
 *   Kernel -> Proxy: "READY\n"           (kernel booted)
 *   Kernel -> Proxy: "Q:<prompt text>\n"  (query for Claude)
 *   Proxy -> Kernel: "A:<response text>\x04"  (answer, terminated by EOT)
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

/* ========== Keyboard (PS/2) ========== */

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

/* Scancode set 1 -> ASCII (US QWERTY, lowercase only, simplified) */
static const char scancode_to_ascii[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' ',0, 0,0,0,0,0,0,0,0,0,0, 0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static char kbd_getchar(void) {
    while (1) {
        if (inb(KBD_STATUS) & 0x01) {
            uint8_t scancode = inb(KBD_DATA);
            if (scancode & 0x80) continue;  /* Ignore key release */
            char c = scancode_to_ascii[scancode];
            if (c) return c;
        }
    }
}

/* ========== REPL ========== */

#define PROMPT_BUF_SIZE 1024

static char prompt_buf[PROMPT_BUF_SIZE];
static size_t prompt_len;

/*
 * Read a line of user input from keyboard.
 * Echoes characters to VGA. Returns on Enter.
 */
static void read_prompt(void) {
    prompt_len = 0;

    while (prompt_len < PROMPT_BUF_SIZE - 1) {
        char c = kbd_getchar();

        if (c == '\n') {
            prompt_buf[prompt_len] = '\0';
            vga_putchar('\n');
            return;
        }
        if (c == '\b') {
            if (prompt_len > 0) {
                prompt_len--;
                vga_putchar('\b');
            }
            continue;
        }

        prompt_buf[prompt_len++] = c;
        vga_putchar(c);
    }
    prompt_buf[prompt_len] = '\0';
}

/*
 * Send prompt to host proxy via serial.
 * Protocol: "Q:<text>\n"
 */
static void send_query(void) {
    serial_puts("Q:");
    for (size_t i = 0; i < prompt_len; i++)
        serial_write(prompt_buf[i]);
    serial_write('\n');
}

/*
 * Receive response from host proxy via serial.
 * Protocol: "A:<text>\x04" (EOT terminated)
 * Displays each character on VGA as it arrives (streaming feel).
 */
static void receive_response(void) {
    /* Skip the "A:" prefix */
    char c1 = serial_read();
    char c2 = serial_read();
    (void)c1; (void)c2;

    /* Read and display response until EOT (0x04) */
    while (1) {
        char c = serial_read();
        if (c == 0x04) break;  /* EOT = end of response */
        vga_putchar(c);
    }
}

/* ========== Number to string ========== */

static void vga_putnum(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) { vga_putchar('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) vga_putchar(buf[i]);
}

static void serial_putnum(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) { serial_write('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) serial_write(buf[i]);
}

/*
 * Test: compute 3 + 5 = 8 using the page fault weird machine.
 * Uses the saturated addition algorithm from instless_comp:
 *
 *   r0 = input A (3)
 *   r1 = input B (5)
 *   r2 = temp
 *   r3 = result
 *
 *   L0: movdbz r2, 1024, L1, L1     ; r2 = 1024
 *   L1: movdbz r0, r0, L2, L3       ; while r0 > 0:
 *   L2: movdbz r2, r2, L1, L1       ;   r2-- (subtracting r0 from r2)
 *   L3: movdbz r1, r1, L4, L5       ; while r1 > 0:
 *   L4: movdbz r2, r2, L3, L3       ;   r2-- (subtracting r1 from r2)
 *   L5: movdbz r3, 1024, L7, L7     ; r3 = 1024
 *   L7: movdbz r2, r2, L8, exit     ; while r2 > 0:
 *   L8: movdbz r3, r3, L7, L7       ;   r3-- (subtracting r2 from r3)
 *                                    ; result: r3 = 1024 - (1024 - r0 - r1) = r0 + r1
 */
static void test_weird_machine(void) {
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("[TEST] Page fault weird machine: computing 3 + 5...\n");
    serial_puts("TEST_WM_START\n");

    /* Set up the weird machine */
    wm_setup();

    /* Allocate registers */
    wm_write_reg(0, 3);      /* r0 = 3 (input A) */
    wm_write_reg(1, 5);      /* r1 = 5 (input B) */
    wm_write_reg(2, 0);      /* r2 = temp */
    wm_write_reg(3, 0);      /* r3 = result */

    /* Allocate constant 1024 */
    int const_1024 = wm_alloc_const(1024);

    /* Generate the addition program */
    /* L0: movdbz r2, 1024, L1, L1  */
    wm_gen_movdbz(0, 2, const_1024, 1, 1);
    /* L1: movdbz r0, r0, L2, L3    */
    wm_gen_movdbz(1, 0, 0, 2, 3);
    /* L2: movdbz r2, r2, L1, L1    */
    wm_gen_movdbz(2, 2, 2, 1, 1);
    /* L3: movdbz r1, r1, L4, L5    */
    wm_gen_movdbz(3, 1, 1, 4, 5);
    /* L4: movdbz r2, r2, L3, L3    */
    wm_gen_movdbz(4, 2, 2, 3, 3);
    /* L5: movdbz r3, 1024, L7, L7  */
    wm_gen_movdbz(5, 3, const_1024, 7, 7);
    /* L7: movdbz r2, r2, L8, exit  */
    wm_gen_movdbz(7, 2, 2, 8, -1);
    /* L8: movdbz r3, r3, L7, L7    */
    wm_gen_movdbz(8, 3, 3, 7, 7);

    /* Run the program! (zero instructions during computation) */
    vga_puts("[TEST] Launching fault cascade...\n");
    wm_run();

    /* Read the result */
    uint32_t result = wm_read_reg(3);

    vga_puts("[TEST] Result: r3 = ");
    vga_putnum(result);
    vga_puts(" (expected 8)\n");

    serial_puts("TEST_WM_RESULT=");
    serial_putnum(result);
    serial_write('\n');

    if (result == 8) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("[TEST] PASS - Page fault computation works!\n\n");
        serial_puts("TEST_WM_PASS\n");
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("[TEST] FAIL - Expected 8, got ");
        vga_putnum(result);
        vga_puts("\n\n");
        serial_puts("TEST_WM_FAIL\n");
    }
}

/* ========== Kernel Main ========== */

void kernel_main(void) {
    vga_init();
    serial_init();

    /* Banner */
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("=== PageFault Claude v0.1 ===\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("A Claude client computed via x86 page faults.\n");
    vga_puts("The MMU is the computer. Zero instructions executed.\n");
    vga_puts("--------------------------------------------\n\n");

    /* Test the page fault weird machine */
    test_weird_machine();

    /* Signal proxy that we're ready */
    serial_puts("READY\n");

    /* REPL loop */
    while (1) {
        /* Prompt */
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("> ");

        /* Read user input */
        vga_set_color(VGA_WHITE, VGA_BLACK);
        read_prompt();

        /* Skip empty input */
        if (prompt_len == 0) continue;

        /* Send to proxy */
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("[sending...]\n");
        send_query();

        /* Receive and display response */
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        receive_response();
        vga_putchar('\n');
        vga_putchar('\n');
    }
}
