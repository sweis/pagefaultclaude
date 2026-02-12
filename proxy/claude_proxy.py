#!/usr/bin/env python3
"""
PageFault Claude - Host-side proxy

Bridges the bare-metal kernel's serial port to the Claude API.
The API key is handled entirely here (via ANTHROPIC_API_KEY env var)
and never passes through the weird machine or the kernel.

Wire protocol:
  Kernel -> Proxy: "READY\n"            (kernel booted)
  Kernel -> Proxy: "Q:<prompt text>\n"   (query for Claude)
  Proxy -> Kernel: "A:<response text>\\x04"  (answer, EOT terminated)

Usage:
  # With QEMU serial on TCP:
  python3 proxy/claude_proxy.py --port 4321

  # With QEMU serial on stdio (pipe mode):
  python3 proxy/claude_proxy.py --pipe
"""

import argparse
import os
import socket
import sys
import time

EOT = b"\x04"


def create_anthropic_client():
    """Create Anthropic client. API key from environment."""
    try:
        import anthropic
        return anthropic.Anthropic()
    except ImportError:
        print("ERROR: 'anthropic' package not installed. Run: pip install anthropic",
              file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: Failed to create Anthropic client: {e}", file=sys.stderr)
        print("Make sure ANTHROPIC_API_KEY is set.", file=sys.stderr)
        sys.exit(1)


def query_claude(client, prompt):
    """Send a prompt to Claude and return the response text."""
    try:
        response = client.messages.create(
            model="claude-sonnet-4-5-20250929",
            max_tokens=512,
            messages=[{"role": "user", "content": prompt}],
        )
        return response.content[0].text
    except Exception as e:
        return f"[API Error: {e}]"


class SerialConnection:
    """Abstraction over TCP socket or stdin/stdout for serial communication."""

    def __init__(self, mode, host="127.0.0.1", port=4321):
        self.mode = mode
        if mode == "tcp":
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            print(f"Connecting to QEMU serial at {host}:{port}...")
            for attempt in range(10):
                try:
                    self.sock.connect((host, port))
                    print("Connected.")
                    return
                except ConnectionRefusedError:
                    if attempt < 9:
                        time.sleep(1)
            print("ERROR: Could not connect to QEMU serial port.", file=sys.stderr)
            sys.exit(1)
        # pipe mode uses stdin/stdout

    def readline(self):
        """Read a line (terminated by \\n) from serial."""
        if self.mode == "tcp":
            buf = b""
            while True:
                c = self.sock.recv(1)
                if not c:
                    raise ConnectionError("Serial connection closed")
                if c == b"\n":
                    return buf.decode("utf-8", errors="replace")
                buf += c
        else:
            return sys.stdin.readline().rstrip("\n")

    def write(self, data):
        """Write bytes to serial."""
        if isinstance(data, str):
            data = data.encode("utf-8")
        if self.mode == "tcp":
            self.sock.sendall(data)
        else:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()

    def close(self):
        if self.mode == "tcp":
            self.sock.close()


def main():
    parser = argparse.ArgumentParser(description="PageFault Claude host proxy")
    parser.add_argument("--port", type=int, default=4321,
                        help="TCP port for QEMU serial (default: 4321)")
    parser.add_argument("--host", default="127.0.0.1",
                        help="Host for QEMU serial (default: 127.0.0.1)")
    parser.add_argument("--pipe", action="store_true",
                        help="Use stdin/stdout instead of TCP")
    args = parser.parse_args()

    client = create_anthropic_client()
    mode = "pipe" if args.pipe else "tcp"
    serial = SerialConnection(mode, args.host, args.port)

    print("PageFault Claude proxy started. Waiting for kernel...", file=sys.stderr)

    try:
        # Wait for kernel READY signal
        while True:
            line = serial.readline()
            print(f"[kernel] {line}", file=sys.stderr)
            if line.strip() == "READY":
                print("Kernel ready. Waiting for queries...", file=sys.stderr)
                break

        # Main loop: handle queries
        while True:
            line = serial.readline()
            print(f"[kernel] {line}", file=sys.stderr)

            if line.startswith("Q:"):
                prompt = line[2:]
                print(f"[proxy] Query: {prompt!r}", file=sys.stderr)

                response = query_claude(client, prompt)
                print(f"[proxy] Response: {response[:80]}...", file=sys.stderr)

                serial.write(b"A:" + response.encode("utf-8") + EOT)

    except (KeyboardInterrupt, ConnectionError):
        print("\nProxy shutting down.", file=sys.stderr)
    finally:
        serial.close()


if __name__ == "__main__":
    main()
