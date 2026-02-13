# PageFault Claude

A Claude API client where the core REPL logic runs entirely via **x86 page fault cascades** — zero application instructions executed for the computation. The CPU is trapped in an endless cascade of page faults and double faults, with the TSS save/restore mechanism performing all computation.

## Quick Start

```bash
# 1. Install dependencies (Ubuntu/Debian)
sudo apt-get install -y gcc gcc-multilib nasm qemu-system-x86 python3
pip3 install anthropic   # only needed for live Claude API mode

# 2. Build
make

# 3. Run automated test (no API key needed)
python3 run_test.py

# 4. Run with mock proxy (interactive, no API key needed)
#    Terminal 1: start QEMU
qemu-system-i386 -kernel build/pagefault_claude \
  -serial tcp:127.0.0.1:4321,server=on,wait=on \
  -monitor none -display none -m 2048 -no-reboot -no-shutdown &
#    Terminal 2: start test proxy
python3 test_protocol.py

# 5. Run with real Claude API
export ANTHROPIC_API_KEY="sk-ant-..."
#    Terminal 1: start QEMU
qemu-system-i386 -kernel build/pagefault_claude \
  -serial tcp:127.0.0.1:4321,server=on,wait=on \
  -monitor none -display none -m 2048 -no-reboot -no-shutdown &
#    Terminal 2: start proxy
python3 proxy/claude_proxy.py --port 4321
```

## Dependencies

| Dependency | Purpose | Install |
|---|---|---|
| `gcc`, `gcc-multilib` | Cross-compile 32-bit bare-metal kernel | `apt install gcc gcc-multilib` |
| `nasm` | Assembler (optional, not currently used) | `apt install nasm` |
| `qemu-system-x86` | x86 emulator to run the bare-metal kernel | `apt install qemu-system-x86` |
| `python3` | Host-side proxy and test scripts | (usually pre-installed) |
| `anthropic` (pip) | Claude API client (only for live mode) | `pip3 install anthropic` |
| `grub-pc-bin`, `xorriso`, `mtools` | Only needed for `make iso` (bootable ISO) | `apt install grub-pc-bin xorriso mtools` |

Or run `make deps` to install everything at once.

## How to Test

### Automated test (recommended)

```bash
make && python3 run_test.py
```

This starts QEMU, connects to the serial port, sends multiple queries, verifies responses, tests empty lines, and sends quit. Expected output:

```
Starting QEMU (serial on TCP port 4322)...
Connected to serial port
Waiting for kernel boot...
  << READY
=== KERNEL READY ===

TEST 1: Send 'hello'
  << hello
  << Q:hello
  >> Query received: 'hello'
  >> Sent response

TEST 2: Send 'world'
  << world
  << Q:world
  >> Query received: 'world'
  >> Sent response
...
ALL TESTS PASSED!
```

### Interactive test (mock responses)

```bash
# Terminal 1: Start QEMU with serial on TCP
qemu-system-i386 -kernel build/pagefault_claude \
  -serial tcp:127.0.0.1:4321,server=on,wait=on \
  -monitor none -display none -m 2048 -no-reboot -no-shutdown

# Terminal 2: Start mock proxy (echoes queries back)
python3 test_protocol.py
```

Type at the `test>` prompt. The kernel reads your input via page fault cascades, sends it as a query, and the mock proxy echoes it back.

### Live Claude API

```bash
export ANTHROPIC_API_KEY="sk-ant-..."

# Terminal 1
qemu-system-i386 -kernel build/pagefault_claude \
  -serial tcp:127.0.0.1:4321,server=on,wait=on \
  -monitor none -display none -m 2048 -no-reboot -no-shutdown

# Terminal 2
python3 proxy/claude_proxy.py --port 4321
```

The proxy also supports `--no-api` flag for mock mode without an API key.

### With VGA display (see the weird machine's output)

Replace `-display none` with `-display curses` or `-display gtk` to see the VGA text output, which shows the boot banner, fault cascade status, and Claude's responses rendered on the bare-metal VGA driver.

## Wire Protocol

```
Kernel → Proxy:  "READY\n"               (kernel booted, weird machine running)
Proxy  → Kernel: <typed characters>\n    (user input, forwarded by proxy)
Kernel → Proxy:  <echo characters>\n     (echo of typed input)
Kernel → Proxy:  "Q:<prompt text>\n"     (query for Claude API)
Proxy  → Kernel: "A:<response>\x04"      (response, EOT-terminated)
Kernel → Proxy:  "BYE\n"                (user typed 'quit')
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Host Machine                                           │
│  ┌─────────────────────────────────────────────────┐    │
│  │  claude_proxy.py                                │    │
│  │  - Reads user input from terminal               │    │
│  │  - Forwards to kernel via serial (TCP)          │    │
│  │  - Calls Claude API on Q: messages              │    │
│  │  - Sends A: responses back                      │    │
│  │  - API key stays here (never in the guest)      │    │
│  └──────────────────────┬──────────────────────────┘    │
│                         │ Serial (TCP socket)           │
│  ┌──────────────────────┴──────────────────────────┐    │
│  │  QEMU (x86 emulator, 2GB RAM)                   │    │
│  │  ┌──────────────────────────────────────────┐   │    │
│  │  │  Bare-metal kernel (kernel.c)            │   │    │
│  │  │  - VGA text-mode display                 │   │    │
│  │  │  - Serial COM1 @ 115200 baud             │   │    │
│  │  │  - I/O bridge: traps WM exits,           │   │    │
│  │  │    services I/O, resumes fault cascade    │   │    │
│  │  │                                          │   │    │
│  │  │  ┌──────────────────────────────────┐    │   │    │
│  │  │  │  Page Fault Weird Machine        │    │   │    │
│  │  │  │  (weirdmachine.c)                │    │   │    │
│  │  │  │                                  │    │   │    │
│  │  │  │  7-instruction movdbz REPL:      │    │   │    │
│  │  │  │  L0: set cmd = READ_BYTE         │    │   │    │
│  │  │  │  L1: exit to bridge              │    │   │    │
│  │  │  │  L2: set cmd = SEND_QUERY        │    │   │    │
│  │  │  │  L3: exit to bridge              │    │   │    │
│  │  │  │  L4: set cmd = RECV_RESPONSE     │    │   │    │
│  │  │  │  L5: exit to bridge              │    │   │    │
│  │  │  │  L6: loop back to L0             │    │   │    │
│  │  │  │                                  │    │   │    │
│  │  │  │  Zero instructions executed.     │    │   │    │
│  │  │  │  All computation via #PF/#DF     │    │   │    │
│  │  │  │  cascades + TSS save/restore.    │    │   │    │
│  │  │  └──────────────────────────────────┘    │   │    │
│  │  └──────────────────────────────────────────┘   │    │
│  └─────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

## File Structure

```
kernel/
  boot.S            Multiboot entry point (sets stack, calls kernel_main)
  set_gdtr.S        Assembly: loads GDTR, segment regs, Task Register
  kernel.c          VGA, serial, I/O bridge loop, REPL program builder
  weirdmachine.c    Page fault weird machine engine
  weirdmachine.h    Public API
  linker.ld         Linker script (kernel at 0x00C00000)
  grub.cfg          GRUB bootloader config

proxy/
  claude_proxy.py   Host-side serial ↔ Claude API bridge

test_protocol.py    Mock proxy for testing without API key
run_test.py         Automated end-to-end test
Makefile            Build system
```

## Makefile Targets

| Target | Description |
|---|---|
| `make` | Build the kernel (`build/pagefault_claude`) |
| `make clean` | Remove build artifacts |
| `make deps` | Install all build dependencies |
| `make iso` | Create a bootable GRUB ISO |
| `make run` | Run in QEMU with curses display |
| `make run-wm` | Run headless with 2GB RAM |

## How the Weird Machine Works

The `movdbz dst, src, nz, z` instruction ("move-decrement-branch-if-zero") is Turing-complete. It is implemented purely through x86 hardware mechanisms:

1. An `ljmp` to a TSS selector triggers a hardware task switch
2. The new TSS has EIP pointing to unmapped memory → **page fault**
3. The page fault pushes an error code, decrementing ESP (the "register value")
4. If the stack write succeeds → `#PF` (nonzero path)
5. If the stack write faults (ESP was 0) → `#DF` (zero path)
6. The fault handler is a **task gate** pointing to the next instruction's TSS
7. Each instruction's page directory remaps the GDT to clear TSS busy bits
8. The cascade continues until an instruction branches to -1 (exit)

No application instruction is ever executed. The CPU is trapped in an infinite loop of task switches triggered by memory protection faults.
