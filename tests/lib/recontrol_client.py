#!/usr/bin/env python3
"""ReControl TCP client — the one shared client for every test and script.

Moved here from test_recontrol.py (Phase 3a consolidation, recovery-plan
task 3.4). Import as:  from tests.lib.recontrol_client import ReControlClient
"""

import socket


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
