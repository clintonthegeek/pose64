#!/usr/bin/env python3
"""ReControl CLI — one-shot command interface for POSE64.

Usage:
    python3 scripts/rc.py [--port PORT] <command> [args...]

Examples:
    python3 scripts/rc.py state
    python3 scripts/rc.py --port 6425 ui
    python3 scripts/rc.py tap-id 1005
    python3 scripts/rc.py type Hello World
    python3 scripts/rc.py screenshot /tmp/screen.png
    python3 scripts/rc.py apps

Exit code 0 if response starts with OK, 1 if ERR or connection failure.
"""

import socket
import sys

# Multi-line commands that return dot-terminated responses
MULTILINE_COMMANDS = {"ui", "info", "apps"}

DEFAULT_PORT = 6416
DEFAULT_HOST = "localhost"
DEFAULT_TIMEOUT = 10


def send_command(host, port, command, timeout=DEFAULT_TIMEOUT):
    """Send a command and return the response text."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        s.sendall((command + "\n").encode())

        cmd_name = command.split()[0].lower() if command.strip() else ""

        if cmd_name in MULTILINE_COMMANDS:
            # Read until dot terminator
            raw = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                raw += chunk
                text = raw.decode("latin-1")
                for line in text.split("\n"):
                    if line.strip() == ".":
                        return text
            return raw.decode("latin-1") if raw else None
        else:
            # Single-line response
            raw = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                raw += chunk
                if b"\n" in raw:
                    return raw.decode("latin-1").split("\n", 1)[0] + "\n"
            return raw.decode("latin-1") if raw else None
    except (ConnectionRefusedError, TimeoutError, OSError) as e:
        print(f"Connection error: {e}", file=sys.stderr)
        return None
    finally:
        s.close()


def main():
    args = sys.argv[1:]

    # Parse --port
    port = DEFAULT_PORT
    if "--port" in args:
        idx = args.index("--port")
        if idx + 1 < len(args):
            port = int(args[idx + 1])
            args = args[:idx] + args[idx + 2:]
        else:
            print("Error: --port requires a value", file=sys.stderr)
            return 1

    if not args:
        print(__doc__.strip())
        return 1

    command = " ".join(args)
    response = send_command(DEFAULT_HOST, port, command)

    if response is None:
        print("Error: no response", file=sys.stderr)
        return 1

    # Print response (strip trailing whitespace but preserve structure)
    print(response.rstrip())

    # Exit code based on first line
    first_line = response.split("\n", 1)[0]
    return 0 if first_line.startswith("OK") else 1


if __name__ == "__main__":
    sys.exit(main())
