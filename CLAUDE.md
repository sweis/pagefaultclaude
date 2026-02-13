# PageFault Claude — Weird Machine REPL

A Claude API client where the core REPL state machine runs entirely via **x86 page fault cascades** — zero application instructions executed. The MMU is the computer.

Inspired by:
- [Page Fault Liberation Army](https://fahrplan.events.ccc.de/congress/2012/Fahrplan/events/5265.en.html) (CCC 2012)
- [Movfuscator](https://github.com/xoreaxeaxeax/movfuscator) (xoreaxeaxeax)
- Bangert & Bratus, "The Page-Fault Weird Machine" (WOOT 2013)

## Architecture

Three layers:

1. **Page fault weird machine** (`kernel/weirdmachine.c`) — A Turing-complete one-instruction computer (`movdbz`: move-decrement-branch-if-zero) implemented purely via TSS task switches triggered by cascading page faults and double faults.

2. **I/O bridge** (`kernel/kernel.c`) — Traps exits from the weird machine, services I/O requests (PS/2 keyboard input, VGA display, serial communication), and resumes the fault cascade at the appropriate instruction.

3. **Host proxy** (`proxy/claude_proxy.py`) — Pure API bridge running on the host. Listens for queries on the serial port, calls the Claude API, and sends responses back. The API key stays here and never touches the weird machine.

## How It Works

The `movdbz dst, src, target_nz, target_z` instruction:
- Computes `dst = src - 1`
- Branches to `target_nz` if `src > 0`, or `target_z` if `src == 0`

This is implemented without executing ANY instructions:
- Each movdbz expands to 3 x86 TSS task switches
- The CPU tries to execute at an unmapped EIP → page fault
- The page fault handler is a **task gate** pointing to the next instruction's TSS
- The error code push decrements ESP (the "register"), and whether the stack write succeeds or faults determines the branch (#PF = nonzero, #DF = zero)
- Three TSS GDT slots rotate to handle the busy-bit constraint

## Key Technical Note

**Constants must be `desired_value + 1`** because movdbz computes `dst = src - 1`. For example, to set R_CMD to `WM_IO_READ_BYTE` (1), the constant register must be initialized to 2.

Source registers are NOT consumed by movdbz execution (the save goes to the destination page via per-instruction page directory remapping), so constants persist across resume cycles.

## I/O Design

The user types directly in the QEMU window via the PS/2 keyboard. The kernel's I/O bridge reads keystrokes, accumulates a line buffer, and displays input on the VGA screen. Serial is used only for the proxy wire protocol (`Q:`/`A:` messages) and logging.

The `input_read()` function polls both PS/2 keyboard and serial, returning whichever has data first. This lets the user type interactively in the QEMU window while keeping automated tests (`run_test.py`) working via serial input.

## Build & Run

See [README.md](README.md) for dependencies, build instructions, and testing.
