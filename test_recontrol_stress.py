#!/usr/bin/env python3
"""Stress test for ReControl TCP interface.

Launches a pose64 subprocess, hammers the TCP interface with rapid
commands, disconnect/reconnect cycles, and mixed workloads, then
verifies the server stays alive throughout.
"""

import argparse
import os
import signal
import subprocess
import sys
import time

from test_recontrol import ReControlClient


TEMP_SCREENSHOT = "/tmp/pose_stress_test.png"


def wait_for_server(host, port, timeout=15):
    """Poll-connect until the ReControl server accepts commands (not just TCP)."""
    deadline = time.time() + timeout
    attempt = 0
    while time.time() < deadline:
        attempt += 1
        client = ReControlClient(host=host, port=port, timeout=2)
        if client.connect():
            # Verify we get a real command response, not "ERR busy"
            resp = client.send_command("state")
            client.disconnect()
            if resp and resp.startswith("OK"):
                print(f"  Server ready after {attempt} attempt(s)")
                return True
            # Got ERR busy or no response — server not ready for us yet
            time.sleep(0.5)
            continue
        time.sleep(0.5)
    return False


def valid_response(resp):
    """Return True if resp is a non-empty string starting with OK or ERR."""
    return resp is not None and (resp.startswith("OK") or resp.startswith("ERR"))


def send_multiline_command(client, cmd):
    """Send a command that produces a multi-line response terminated by '.'.

    Returns the full response text (including the terminator line), or None
    on error/timeout.  Only the *first* line is used for OK/ERR validation
    by callers.  Uses latin-1 decoding since Palm OS text is not UTF-8.
    """
    if not client.connected:
        return None
    try:
        client.socket.sendall((cmd + "\n").encode())
        raw = b''
        while True:
            chunk = client.socket.recv(4096)
            if not chunk:
                break
            raw += chunk
            text = raw.decode('latin-1')
            # Check for "." terminator on its own line
            for line in text.split('\n'):
                if line.strip() == '.':
                    return text
        return raw.decode('latin-1') if raw else None
    except Exception as e:
        print(f"  multiline read error: {e}")
        return None


# ---------------------------------------------------------------------------
# Individual stress tests
# ---------------------------------------------------------------------------

def test_rapid_taps(host, port):
    """(a) Send 50 rapid tap commands."""
    name = "rapid_taps"
    count = 50
    print(f"\n=== Stress: Rapid Taps ({count}x) ===")

    client = ReControlClient(host=host, port=port, timeout=5)
    if not client.connect():
        print("  FAIL: could not connect")
        return name, False

    failures = 0
    for i in range(count):
        resp = client.send_command("tap 80 80")
        if not valid_response(resp):
            failures += 1
            if failures <= 3:
                print(f"  bad response #{i}: {resp!r}")

    client.disconnect()

    if failures:
        print(f"  FAIL: {failures}/{count} bad responses")
        return name, False
    print(f"  PASS: all {count} responses valid")
    return name, True


def test_rapid_state(host, port):
    """(b) Send 50 rapid state queries."""
    name = "rapid_state"
    count = 50
    print(f"\n=== Stress: Rapid State Queries ({count}x) ===")

    client = ReControlClient(host=host, port=port, timeout=5)
    if not client.connect():
        print("  FAIL: could not connect")
        return name, False

    failures = 0
    for i in range(count):
        resp = client.send_command("state")
        if not valid_response(resp):
            failures += 1
            if failures <= 3:
                print(f"  bad response #{i}: {resp!r}")

    client.disconnect()

    if failures:
        print(f"  FAIL: {failures}/{count} bad responses")
        return name, False
    print(f"  PASS: all {count} responses valid")
    return name, True


def test_screenshots(host, port):
    """(c) Take 3 screenshots."""
    name = "screenshots"
    count = 3
    print(f"\n=== Stress: Screenshots ({count}x) ===")

    client = ReControlClient(host=host, port=port, timeout=10)
    if not client.connect():
        print("  FAIL: could not connect")
        return name, False

    failures = 0
    for i in range(count):
        resp = client.send_command(f"screenshot {TEMP_SCREENSHOT}")
        if not valid_response(resp):
            failures += 1
            print(f"  bad response #{i}: {resp!r}")

    client.disconnect()

    if failures:
        print(f"  FAIL: {failures}/{count} bad responses")
        return name, False
    print(f"  PASS: all {count} responses valid")
    return name, True


def test_ui_queries(host, port):
    """(d) Send 3 ui queries (multi-line)."""
    name = "ui_queries"
    count = 3
    print(f"\n=== Stress: UI Queries ({count}x) ===")

    client = ReControlClient(host=host, port=port, timeout=10)
    if not client.connect():
        print("  FAIL: could not connect")
        return name, False

    failures = 0
    for i in range(count):
        resp = send_multiline_command(client, "ui")
        if resp is None:
            failures += 1
            print(f"  no response #{i}")
        else:
            first_line = resp.split("\n", 1)[0]
            if not valid_response(first_line):
                failures += 1
                print(f"  bad first line #{i}: {first_line!r}")

    client.disconnect()

    if failures:
        print(f"  FAIL: {failures}/{count} bad responses")
        return name, False
    print(f"  PASS: all {count} responses valid")
    return name, True


def test_info_queries(host, port):
    """(e) Send 3 info queries (multi-line)."""
    name = "info_queries"
    count = 3
    print(f"\n=== Stress: Info Queries ({count}x) ===")

    client = ReControlClient(host=host, port=port, timeout=10)
    if not client.connect():
        print("  FAIL: could not connect")
        return name, False

    failures = 0
    for i in range(count):
        resp = send_multiline_command(client, "info")
        if resp is None:
            failures += 1
            print(f"  no response #{i}")
        else:
            first_line = resp.split("\n", 1)[0]
            if not valid_response(first_line):
                failures += 1
                print(f"  bad first line #{i}: {first_line!r}")

    client.disconnect()

    if failures:
        print(f"  FAIL: {failures}/{count} bad responses")
        return name, False
    print(f"  PASS: all {count} responses valid")
    return name, True


def test_mixed_rapid(host, port):
    """(f) Alternate state and tap commands 30 times."""
    name = "mixed_rapid"
    count = 30
    print(f"\n=== Stress: Mixed Rapid Commands ({count}x) ===")

    client = ReControlClient(host=host, port=port, timeout=5)
    if not client.connect():
        print("  FAIL: could not connect")
        return name, False

    failures = 0
    for i in range(count):
        cmd = "state" if i % 2 == 0 else "tap 80 80"
        resp = client.send_command(cmd)
        if not valid_response(resp):
            failures += 1
            if failures <= 3:
                print(f"  bad response #{i} ({cmd}): {resp!r}")

    client.disconnect()

    if failures:
        print(f"  FAIL: {failures}/{count} bad responses")
        return name, False
    print(f"  PASS: all {count} responses valid")
    return name, True


def test_disconnect_reconnect_under_load(host, port):
    """(g) Disconnect mid-command, reconnect, verify server alive."""
    name = "disconnect_reconnect"
    print("\n=== Stress: Disconnect/Reconnect Under Load ===")

    client = ReControlClient(host=host, port=port, timeout=5)
    if not client.connect():
        print("  FAIL: initial connect failed")
        return name, False

    # Send a command then immediately disconnect (don't wait for response)
    try:
        client.socket.sendall(b"tap 80 80\n")
    except Exception:
        pass
    client.disconnect()

    time.sleep(0.5)

    # Reconnect and verify
    client2 = ReControlClient(host=host, port=port, timeout=5)
    if not client2.connect():
        print("  FAIL: reconnect failed -- server may have crashed")
        return name, False

    resp = client2.send_command("state")
    client2.disconnect()

    if not valid_response(resp):
        print(f"  FAIL: post-reconnect state response bad: {resp!r}")
        return name, False

    print("  PASS: server survived disconnect/reconnect under load")
    return name, True


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def run_tests(host, port):
    """Run all stress tests and return results list."""
    results = []
    results.append(test_rapid_taps(host, port))
    results.append(test_rapid_state(host, port))
    results.append(test_screenshots(host, port))
    results.append(test_ui_queries(host, port))
    results.append(test_info_queries(host, port))
    results.append(test_mixed_rapid(host, port))
    results.append(test_disconnect_reconnect_under_load(host, port))
    return results


def print_summary(results):
    """Print test summary and return exit code."""
    passed = [n for n, ok in results if ok]
    failed = [n for n, ok in results if not ok]

    print("\n" + "=" * 60)
    print(f"Stress Test Results: {len(passed)}/{len(results)} passed")
    if passed:
        for n in passed:
            print(f"  PASS  {n}")
    if failed:
        for n in failed:
            print(f"  FAIL  {n}")

    return 0 if not failed else 1


def main():
    parser = argparse.ArgumentParser(description="Stress test for ReControl TCP interface")
    parser.add_argument("--psf", default="m515.psf",
                        help="Path to PSF ROM file (default: m515.psf)")
    parser.add_argument("--port", type=int, default=6427,
                        help="TCP port for ReControl (default: 6427)")
    parser.add_argument("--build-dir", default="./build",
                        help="Build directory containing pose64 binary (default: ./build)")
    parser.add_argument("--no-launch", action="store_true",
                        help="Connect to an already-running instance instead of launching one")
    args = parser.parse_args()

    host = "localhost"
    port = args.port

    if args.no_launch:
        print(f"Connecting to existing server on port {port} ...")
        if not wait_for_server(host, port, timeout=5):
            print(f"ERROR: no server listening on port {port}")
            return 1

        results = run_tests(host, port)
        return print_summary(results)

    exe = os.path.join(args.build_dir, "pose64")
    if not os.path.isfile(exe):
        print(f"ERROR: executable not found: {exe}")
        return 1

    cmd = [exe, "-psf", args.psf, "--port", str(args.port)]
    print(f"Launching: {' '.join(cmd)}")

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        print(f"Waiting for server on port {port} ...")
        if not wait_for_server(host, port, timeout=15):
            print("ERROR: server did not start within 15 seconds")
            return 1

        results = run_tests(host, port)

        # Final check: process still alive
        print("\n=== Final Check: Process Alive ===")
        retcode = proc.poll()
        if retcode is not None:
            print(f"  FAIL: process exited with code {retcode}")
            results.append(("process_alive", False))
        else:
            print("  PASS: process still running")
            results.append(("process_alive", True))

        return print_summary(results)

    finally:
        # Clean up subprocess
        if proc.poll() is None:
            print("\nTerminating pose64 subprocess ...")
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                print("Killing pose64 subprocess ...")
                proc.kill()
                proc.wait()

        # Clean up temp screenshot
        if os.path.exists(TEMP_SCREENSHOT):
            try:
                os.remove(TEMP_SCREENSHOT)
                print(f"Removed temp file: {TEMP_SCREENSHOT}")
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main())
