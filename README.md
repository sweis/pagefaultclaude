# PageFault Claude

A Claude API client where the core REPL logic runs entirely via **x86 page fault cascades** — zero application instructions executed. The CPU is trapped in an endless cascade of page faults and double faults, with the TSS save/restore mechanism performing all computation.

You type directly in the QEMU window. Your keystrokes go through a page fault weird machine, and Claude's responses appear on the VGA display.

<img width="641" height="479" alt="image" src="https://github.com/user-attachments/assets/c69443e2-8388-47e1-ac58-50508eb12e14" />

## Build & Run

```bash
# Install dependencies
sudo apt-get install -y gcc gcc-multilib qemu-system-x86 python3
pip3 install anthropic

# Build
make

# Run the automated test (no API key needed)
make && python3 run_test.py

# Run with the Claude API
export ANTHROPIC_API_KEY="sk-ant-..."
make run-proxy
```

`make run-proxy` starts the proxy in the background and opens QEMU with a curses display. Type your questions directly in the QEMU window.

## How It Works

### Three layers

```
┌─────────────────────────────────────────────────────────┐
│  Host                                                   │
│  ┌─────────────────────────────────────────────────┐    │
│  │  claude_proxy.py                                │    │
│  │  Pure API bridge: Q: → Claude API → A:          │    │
│  │  API key stays here (never in the guest)        │    │
│  └──────────────────────┬──────────────────────────┘    │
│                         │ Serial (TCP)                  │
│  ┌──────────────────────┴──────────────────────────┐    │
│  │  QEMU                                           │    │
│  │  ┌──────────────────────────────────────────┐   │    │
│  │  │  Bare-metal kernel                       │   │    │
│  │  │  PS/2 keyboard → I/O bridge → VGA        │   │    │
│  │  │                                          │   │    │
│  │  │  ┌──────────────────────────────────┐    │   │    │
│  │  │  │  Page Fault Weird Machine        │    │   │    │
│  │  │  │                                  │    │   │    │
│  │  │  │  7 movdbz instructions:          │    │   │    │
│  │  │  │    read → send query →           │    │   │    │
│  │  │  │    recv response → loop          │    │   │    │
│  │  │  │                                  │    │   │    │
│  │  │  │  Zero instructions executed.     │    │   │    │
│  │  │  │  All computation via #PF/#DF     │    │   │    │
│  │  │  │  cascades + TSS save/restore.    │    │   │    │
│  │  │  └──────────────────────────────────┘    │   │    │
│  │  └──────────────────────────────────────────┘   │    │
│  └─────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

1. **Page fault weird machine** (`kernel/weirdmachine.c`) — A Turing-complete one-instruction computer (`movdbz`: move-decrement-branch-if-zero) implemented purely via TSS task switches triggered by cascading page faults and double faults.

2. **I/O bridge** (`kernel/kernel.c`) — Traps exits from the weird machine, reads the PS/2 keyboard, drives the VGA display, talks to the proxy over serial, and resumes the fault cascade.

3. **Host proxy** (`proxy/claude_proxy.py`) — Pure serial-to-API bridge. Waits for `Q:` queries, calls the Claude API, sends `A:` responses. Logs all serial traffic to the screen.

### The movdbz instruction

`movdbz dst, src, nz, z` ("move-decrement-branch-if-zero") is Turing-complete. It computes `dst = src - 1` and branches to `nz` if nonzero, `z` if zero. Implemented purely through x86 hardware:

1. `ljmp` to a TSS selector triggers a hardware task switch
2. The new TSS has EIP pointing to unmapped memory → **#PF** (page fault)
3. The fault pushes an error code, decrementing ESP (the "register value")
4. If the stack write succeeds → another **#PF** (nonzero path)
5. If the stack write faults (ESP was 0) → **#DF** (double fault = zero path)
6. The fault handler is a **task gate** pointing to the next instruction's TSS
7. Each instruction's page directory remaps the GDT to clear TSS busy bits
8. The cascade continues until an instruction branches to -1 (exit)

No application instruction is ever executed. The CPU is trapped in an infinite loop of task switches triggered by memory protection faults.

## Wire Protocol

User input comes from the PS/2 keyboard (typed in the QEMU window). Serial is used only for the proxy wire protocol and logging.

```
Kernel → Proxy:  "READY\n"               kernel booted
Kernel → Proxy:  <echoed keystrokes>\n    keyboard input echoed for logging
Kernel → Proxy:  "Q:<prompt>\n"           query for Claude API
Proxy  → Kernel: "A:<response>\x04"       response (EOT-terminated)
Kernel → Proxy:  "Claude: <text>\n"       response echoed for logging
Kernel → Proxy:  "BYE\n"                 user typed 'quit'
```

## Testing

### Automated test (no API key needed)

```bash
make && python3 run_test.py
```

Sends queries via serial, verifies responses, tests empty lines, and sends quit.

### Interactive with mock responses

```bash
# Terminal 1: QEMU with VGA display (type here)
qemu-system-i386 -kernel build/pagefault_claude \
  -serial tcp:127.0.0.1:4321,server=on,wait=on \
  -display curses -m 2048 -no-reboot -no-shutdown

# Terminal 2: mock proxy
python3 test_protocol.py
```

### Live Claude API

```bash
export ANTHROPIC_API_KEY="sk-ant-..."
make run-proxy
```

Proxy logs go to `proxy.log`. To watch them: `tail -f proxy.log` in another terminal.

For a two-terminal setup (proxy logs visible on screen):

```bash
# Terminal 1: QEMU (type here)
qemu-system-i386 -kernel build/pagefault_claude \
  -serial tcp:127.0.0.1:4321,server=on,wait=on \
  -display curses -m 2048 -no-reboot -no-shutdown

# Terminal 2: proxy (logs to screen)
python3 proxy/claude_proxy.py --port 4321
```

The `--no-api` flag on `claude_proxy.py` enables mock mode without an API key.

## Files

```
kernel/
  boot.S            Multiboot entry, sets stack, calls kernel_main
  set_gdtr.S        Loads GDTR, segment regs, Task Register
  kernel.c          VGA, serial, PS/2 keyboard, I/O bridge, REPL program
  weirdmachine.c    Page fault weird machine engine
  weirdmachine.h    Public API
  linker.ld         Linker script (kernel at 0x00C00000)
  grub.cfg          GRUB bootloader config

proxy/
  claude_proxy.py   Serial ↔ Claude API bridge

test_protocol.py    Mock proxy for testing without API key
run_test.py         Automated end-to-end test
Makefile            Build system
```

## Makefile Targets

| Target | Description |
|---|---|
| `make` | Build the kernel |
| `make run-proxy` | Build, start proxy + QEMU (type in the QEMU window) |
| `make run` | Run in QEMU with curses display (no proxy) |
| `make clean` | Remove build artifacts |
| `make deps` | Install all build dependencies |
| `make iso` | Create a bootable GRUB ISO |
