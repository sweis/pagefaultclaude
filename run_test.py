#!/usr/bin/env python3
"""End-to-end test: starts QEMU, connects to serial, drives the REPL."""

import os
import socket
import subprocess
import sys
import time

PORT = 4322
KERNEL = "build/pagefault_claude"
TIMEOUT = 15


def readline(sock):
    buf = b""
    while True:
        c = sock.recv(1)
        if not c:
            raise ConnectionError("closed")
        buf += c
        if c == b"\n":
            return buf.decode(errors="replace").rstrip("\n")


def main():
    if not os.path.exists(KERNEL):
        print(f"ERROR: {KERNEL} not found. Run 'make' first.", file=sys.stderr)
        sys.exit(1)

    # Start QEMU
    print(f"Starting QEMU (serial on TCP port {PORT})...")
    qemu = subprocess.Popen(
        [
            "qemu-system-i386",
            "-kernel", KERNEL,
            "-serial", f"tcp:127.0.0.1:{PORT},server=on,wait=on",
            "-monitor", "none",
            "-display", "none",
            "-m", "2048",
            "-no-reboot", "-no-shutdown",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    print(f"QEMU PID: {qemu.pid}")

    # Connect (QEMU waits for us with wait=on, then starts the guest)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    for _ in range(20):
        try:
            sock.connect(("127.0.0.1", PORT))
            print("Connected to serial port")
            break
        except ConnectionRefusedError:
            time.sleep(0.5)
    else:
        print("FAILED to connect")
        qemu.kill()
        sys.exit(1)

    sock.settimeout(TIMEOUT)

    try:
        # Wait for READY
        print("Waiting for kernel boot...")
        while True:
            line = readline(sock)
            print(f"  << {line}")
            if line.strip() == "READY":
                print("=== KERNEL READY ===\n")
                break

        # Test 1: basic query
        print("TEST 1: Send 'hello'")
        sock.sendall(b"hello\n")
        while True:
            line = readline(sock)
            print(f"  << {line}")
            if line.startswith("Q:"):
                query = line[2:]
                assert query == "hello", f"Expected 'hello', got {query!r}"
                print(f"  >> Query received: {query!r}")
                sock.sendall(b"A:Hello from test!\x04")
                print(f"  >> Sent response")
                break
            elif line.strip() == "BYE":
                print("FAIL: Got BYE instead of Q:")
                sys.exit(1)

        time.sleep(0.5)

        # Test 2: second query (proves resume works)
        print("\nTEST 2: Send 'world'")
        sock.sendall(b"world\n")
        while True:
            line = readline(sock)
            print(f"  << {line}")
            if line.startswith("Q:"):
                query = line[2:]
                assert query == "world", f"Expected 'world', got {query!r}"
                print(f"  >> Query received: {query!r}")
                sock.sendall(b"A:Second response!\x04")
                print(f"  >> Sent response")
                break

        time.sleep(0.5)

        # Test 3: empty line (should not produce a query)
        print("\nTEST 3: Send empty line")
        sock.sendall(b"\n")
        time.sleep(0.5)
        # The kernel should loop back to READ_BYTE without sending Q:
        # Send a real query to verify the machine is still alive
        print("  >> Sending 'alive' to verify machine is still running")
        sock.sendall(b"alive\n")
        while True:
            line = readline(sock)
            print(f"  << {line}")
            if line.startswith("Q:"):
                query = line[2:]
                assert query == "alive", f"Expected 'alive', got {query!r}"
                print(f"  >> Query received: {query!r}")
                sock.sendall(b"A:Still alive!\x04")
                print(f"  >> Sent response")
                break

        time.sleep(0.5)

        # Test 4: quit
        print("\nTEST 4: Send 'quit'")
        sock.sendall(b"quit\n")
        while True:
            line = readline(sock)
            print(f"  << {line}")
            if line.strip() == "BYE":
                print("  >> Got BYE")
                break

        print("\n" + "=" * 50)
        print("ALL TESTS PASSED!")
        print("  - First query works (launch)")
        print("  - Second query works (resume)")
        print("  - Empty line handled")
        print("  - Quit works")
        print("  - The page fault weird machine REPL is functional!")
        print("=" * 50)

    except socket.timeout:
        print("\nFAIL: Timeout waiting for data")
        sys.exit(1)
    except Exception as e:
        print(f"\nFAIL: {e}")
        sys.exit(1)
    finally:
        sock.close()
        qemu.kill()
        qemu.wait()
        print("QEMU stopped.")


if __name__ == "__main__":
    main()
