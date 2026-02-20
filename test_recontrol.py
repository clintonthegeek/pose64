#!/usr/bin/env python3
"""Integration test for ReControl TCP interface.

Tests the ReControl TCP server with:
- Happy path: common command sequences
- Error cases: invalid commands, disconnections
- Multi-connection: rejection of multiple simultaneous connections
- Response validation: format and content checks
"""

import socket
import sys
import time
import threading
import argparse
from pathlib import Path


class ReControlClient:
    """Simple TCP client for ReControl protocol."""

    def __init__(self, host='localhost', port=6425, timeout=5):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.socket = None
        self.connected = False

    def connect(self):
        """Connect to ReControl server."""
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.settimeout(self.timeout)
            self.socket.connect((self.host, self.port))
            self.connected = True
            return True
        except socket.error as e:
            print(f"Connection failed: {e}")
            return False

    def disconnect(self):
        """Disconnect from server."""
        if self.socket:
            self.socket.close()
            self.connected = False

    def send_command(self, cmd):
        """Send command and read response."""
        if not self.connected:
            print(f"Not connected")
            return None

        try:
            # Send command
            self.socket.sendall((cmd + '\n').encode())

            # Read response line
            response = self.socket.recv(4096).decode()
            return response
        except socket.timeout:
            print(f"Command '{cmd}' timed out")
            return None
        except socket.error as e:
            print(f"Error sending command: {e}")
            return None

    def read_multiline_response(self):
        """Read multi-line response terminated by period."""
        lines = []
        try:
            while True:
                chunk = self.socket.recv(4096).decode()
                if not chunk:
                    break
                for line in chunk.split('\n'):
                    if line:
                        lines.append(line)
                        if line.strip() == '.':
                            return '\n'.join(lines)
        except socket.timeout:
            pass
        return '\n'.join(lines)


def test_basic_connection(client, tests_passed, tests_failed):
    """Test 1: Basic connection and info command."""
    print("\n=== Test 1: Basic Connection and Info ===")
    if not client.connect():
        print("FAIL: Could not connect to server")
        tests_failed.append("basic_connection")
        return

    response = client.send_command("info")
    if response and response.startswith("OK POSE64"):
        print(f"PASS: Info command response: {response.strip()}")
        tests_passed.append("basic_connection")
    else:
        print(f"FAIL: Info command unexpected response: {response}")
        tests_failed.append("basic_connection")

    client.disconnect()


def test_state_command(client, tests_passed, tests_failed):
    """Test 2: State command with suspend diagnostics."""
    print("\n=== Test 2: State Command ===")
    if not client.connect():
        print("FAIL: Could not connect to server")
        tests_failed.append("state_command")
        return

    response = client.send_command("state")
    if response:
        state = response.strip()
        if state.startswith("OK ") and any(x in state for x in ["running", "suspended", "stopped", "blocked"]):
            print(f"PASS: State command response: {state}")
            tests_passed.append("state_command")
        else:
            print(f"FAIL: State command unexpected response: {response}")
            tests_failed.append("state_command")
    else:
        print("FAIL: State command no response")
        tests_failed.append("state_command")

    client.disconnect()


def test_invalid_command(client, tests_passed, tests_failed):
    """Test 3: Error handling for invalid command."""
    print("\n=== Test 3: Invalid Command Handling ===")
    if not client.connect():
        print("FAIL: Could not connect to server")
        tests_failed.append("invalid_command")
        return

    response = client.send_command("invalid_command arg1 arg2")
    if response and response.startswith("ERR"):
        print(f"PASS: Invalid command properly rejected: {response.strip()}")
        tests_passed.append("invalid_command")
    else:
        print(f"FAIL: Invalid command should produce ERR response: {response}")
        tests_failed.append("invalid_command")

    client.disconnect()


def test_multi_connection_rejection(tests_passed, tests_failed):
    """Test 4: Multi-connection rejection."""
    print("\n=== Test 4: Multi-Connection Rejection ===")

    client1 = ReControlClient()
    client2 = ReControlClient()

    if not client1.connect():
        print("FAIL: First connection failed")
        tests_failed.append("multi_connection")
        return

    print("First connection established")

    # Try second connection - should either fail or be rejected
    if client2.connect():
        # Connection succeeded, now test if it gets rejected
        response = client2.send_command("state")
        if response and response.startswith("ERR"):
            print(f"PASS: Second connection properly rejected: {response.strip()}")
            tests_passed.append("multi_connection")
        else:
            print("FAIL: Second connection should be rejected")
            tests_failed.append("multi_connection")
        client2.disconnect()
    else:
        print("PASS: Second connection was rejected at TCP level")
        tests_passed.append("multi_connection")

    client1.disconnect()


def test_disconnect_and_reconnect(client, tests_passed, tests_failed):
    """Test 5: Disconnect and reconnect."""
    print("\n=== Test 5: Disconnect and Reconnect ===")

    if not client.connect():
        print("FAIL: First connection failed")
        tests_failed.append("disconnect_reconnect")
        return

    response1 = client.send_command("state")
    client.disconnect()
    print("First connection closed")

    time.sleep(0.5)

    if not client.connect():
        print("FAIL: Reconnection failed")
        tests_failed.append("disconnect_reconnect")
        return

    response2 = client.send_command("state")
    if response1 and response2:
        print("PASS: Disconnect and reconnect successful")
        tests_passed.append("disconnect_reconnect")
    else:
        print("FAIL: One of the state commands failed")
        tests_failed.append("disconnect_reconnect")

    client.disconnect()


def test_screenshot_command(client, tests_passed, tests_failed):
    """Test 6: Screenshot command."""
    print("\n=== Test 6: Screenshot Command ===")

    if not client.connect():
        print("FAIL: Could not connect to server")
        tests_failed.append("screenshot_command")
        return

    response = client.send_command("screenshot")
    if response and (response.startswith("OK") or response.startswith("ERR")):
        print(f"PASS: Screenshot command response received (first 100 chars): {response[:100]}")
        tests_passed.append("screenshot_command")
    else:
        print(f"FAIL: Screenshot command unexpected response: {response}")
        tests_failed.append("screenshot_command")

    client.disconnect()


def test_input_commands(client, tests_passed, tests_failed):
    """Test 7: Input commands (tap, key, button)."""
    print("\n=== Test 7: Input Commands ===")

    if not client.connect():
        print("FAIL: Could not connect to server")
        tests_failed.append("input_commands")
        return

    all_passed = True

    # Test tap command
    response = client.send_command("tap 100 150")
    if response and response.startswith("OK"):
        print("PASS: Tap command accepted")
    else:
        print(f"FAIL: Tap command failed: {response}")
        all_passed = False

    # Test key command
    response = client.send_command("key 65")  # 'A' key
    if response and response.startswith("OK"):
        print("PASS: Key command accepted")
    else:
        print(f"FAIL: Key command failed: {response}")
        all_passed = False

    # Test button command
    response = client.send_command("button power")
    if response and response.startswith("OK"):
        print("PASS: Button command accepted")
    else:
        print(f"FAIL: Button command failed: {response}")
        all_passed = False

    if all_passed:
        tests_passed.append("input_commands")
    else:
        tests_failed.append("input_commands")

    client.disconnect()


def test_command_sequence(client, tests_passed, tests_failed):
    """Test 8: Sequence of commands."""
    print("\n=== Test 8: Command Sequence ===")

    if not client.connect():
        print("FAIL: Could not connect to server")
        tests_failed.append("command_sequence")
        return

    commands = [
        "state",
        "info",
        "state",
    ]

    all_passed = True
    for cmd in commands:
        response = client.send_command(cmd)
        if response and response.startswith("OK"):
            print(f"  {cmd}: OK")
        else:
            print(f"  {cmd}: FAIL - {response}")
            all_passed = False

    if all_passed:
        tests_passed.append("command_sequence")
    else:
        tests_failed.append("command_sequence")

    client.disconnect()


def main():
    parser = argparse.ArgumentParser(description='ReControl integration tests')
    parser.add_argument('--host', default='localhost', help='ReControl server host')
    parser.add_argument('--port', type=int, default=6425, help='ReControl server port')
    parser.add_argument('--timeout', type=int, default=5, help='Socket timeout in seconds')
    args = parser.parse_args()

    print(f"ReControl Integration Tests")
    print(f"Connecting to {args.host}:{args.port}")

    tests_passed = []
    tests_failed = []

    client = ReControlClient(host=args.host, port=args.port, timeout=args.timeout)

    # Run tests
    test_basic_connection(client, tests_passed, tests_failed)
    test_state_command(client, tests_passed, tests_failed)
    test_invalid_command(client, tests_passed, tests_failed)
    test_multi_connection_rejection(tests_passed, tests_failed)
    test_disconnect_and_reconnect(client, tests_passed, tests_failed)
    test_screenshot_command(client, tests_passed, tests_failed)
    test_input_commands(client, tests_passed, tests_failed)
    test_command_sequence(client, tests_passed, tests_failed)

    # Summary
    print("\n" + "=" * 60)
    print(f"Tests Passed: {len(tests_passed)}/{len(tests_passed) + len(tests_failed)}")
    if tests_passed:
        print(f"  {', '.join(tests_passed)}")
    if tests_failed:
        print(f"Tests Failed: {len(tests_failed)}")
        print(f"  {', '.join(tests_failed)}")

    return 0 if not tests_failed else 1


if __name__ == '__main__':
    sys.exit(main())
