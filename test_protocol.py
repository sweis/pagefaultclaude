#!/usr/bin/env python3
"""Test the serial wire protocol without needing the Claude API.
Connects to QEMU's serial port on TCP and simulates the proxy side.

Usage:
  1. Start QEMU: qemu-system-i386 -kernel build/pagefault_claude \
       -serial tcp:127.0.0.1:4321,server=on,wait=off -display curses
  2. Run this script: python3 test_protocol.py
  3. Type in the QEMU window -> see queries appear here
  4. Responses are sent back as mock data
"""

import socket
import sys
import time

HOST = "127.0.0.1"
PORT = 4321
EOT = b"\x04"


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    print(f"Connecting to {HOST}:{PORT}...")

    for attempt in range(10):
        try:
            sock.connect((HOST, PORT))
            print("Connected!")
            break
        except ConnectionRefusedError:
            print(f"  Attempt {attempt + 1}/10 - waiting...")
            time.sleep(1)
    else:
        print("ERROR: Could not connect.", file=sys.stderr)
        sys.exit(1)

    # Read until READY
    buf = b""
    while True:
        c = sock.recv(1)
        if not c:
            print("Connection closed.", file=sys.stderr)
            sys.exit(1)
        buf += c
        if buf.endswith(b"\n"):
            line = buf.decode().strip()
            print(f"[kernel] {line}")
            if line == "READY":
                print("Kernel ready! Type in the QEMU window to send queries.")
                break
            buf = b""

    # Handle queries
    buf = b""
    while True:
        c = sock.recv(1)
        if not c:
            break
        buf += c
        if buf.endswith(b"\n"):
            line = buf.decode().strip()
            print(f"[kernel] {line}")
            buf = b""

            if line.startswith("Q:"):
                query = line[2:]
                print(f"[test] Got query: {query!r}")
                # Send mock response
                response = f"A:[Mock] You said: {query}"
                sock.sendall(response.encode() + EOT)
                print(f"[test] Sent response")

    sock.close()


if __name__ == "__main__":
    main()
