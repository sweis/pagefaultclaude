#!/usr/bin/env python3
"""Test the serial wire protocol without needing the Claude API.
Connects to QEMU's serial port on TCP and acts as a mock proxy.

Usage:
  1. Start QEMU:
       make all && qemu-system-i386 -kernel build/pagefault_claude \
         -serial tcp:127.0.0.1:4321,server=on,wait=off \
         -display curses -m 2048 -no-reboot -no-shutdown
  2. Run this script: python3 test_protocol.py
  3. Type queries at the prompt; mock responses are sent back.
"""

import socket
import sys
import time

HOST = "127.0.0.1"
PORT = 4321
EOT = b"\x04"


def readline(sock):
    """Read a line from the socket (until \\n)."""
    buf = b""
    while True:
        c = sock.recv(1)
        if not c:
            raise ConnectionError("Connection closed")
        if c == b"\n":
            return buf.decode("utf-8", errors="replace")
        buf += c


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    print(f"Connecting to {HOST}:{PORT}...")

    for attempt in range(30):
        try:
            sock.connect((HOST, PORT))
            print("Connected!")
            break
        except ConnectionRefusedError:
            print(f"  Attempt {attempt + 1}/30 - waiting...")
            time.sleep(1)
    else:
        print("ERROR: Could not connect.", file=sys.stderr)
        sys.exit(1)

    # Read until READY
    while True:
        line = readline(sock)
        print(f"[kernel] {line}")
        if line.strip() == "READY":
            print("Kernel ready! Weird machine is running.\n")
            break

    # Main REPL loop
    try:
        while True:
            user_input = input("test> ")
            if not user_input:
                sock.sendall(b"\n")
                readline(sock)  # consume echo
                continue

            # Send user input to kernel
            sock.sendall(user_input.encode() + b"\n")

            # Read serial output until Q: or BYE
            while True:
                line = readline(sock)

                if line.startswith("Q:"):
                    query = line[2:]
                    print(f"[test] Got query: {query!r}")

                    # Send mock response
                    response = f"[Mock echo] You said: {query}"
                    sock.sendall(b"A:" + response.encode() + EOT)
                    print(f"\n{response}\n")
                    break

                elif line.strip() == "BYE":
                    print("Kernel says goodbye.")
                    return

    except (KeyboardInterrupt, ConnectionError, EOFError):
        print("\nTest ended.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
