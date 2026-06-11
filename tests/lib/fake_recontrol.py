#!/usr/bin/env python3
"""Scriptable fake ReControl server for proxy translation tests.

Listens on an ephemeral port, records every command line received, and
replies according to a script of (prefix, response) rules:

  - response str            -> single line + "\n"
  - response list[str]      -> each line + "\n", then ".\n" (multiline)
  - response ("close",)     -> drop the connection (no reply)

Unmatched commands get "OK\n".
"""

import socket
import threading


class FakeReControl:
    def __init__(self):
        self._rules = []          # list of (prefix, response)
        self.received = []        # every command line, in order
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind(("127.0.0.1", 0))
        self._srv.listen(4)
        self.port = self._srv.getsockname()[1]
        self._stop = False
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def script(self, prefix, response):
        """Add a rule: commands starting with `prefix` get `response`."""
        self._rules.append((prefix, response))

    def _respond(self, conn, line):
        for prefix, response in self._rules:
            if line.startswith(prefix):
                if isinstance(response, tuple) and response[0] == "close":
                    raise ConnectionAbortedError  # handled in _serve: close conn
                if isinstance(response, list):
                    for ln in response:
                        conn.sendall((ln + "\n").encode("latin-1"))
                    conn.sendall(b".\n")
                else:
                    conn.sendall((response + "\n").encode("latin-1"))
                return
        conn.sendall(b"OK\n")

    def _serve(self):
        self._srv.settimeout(0.2)
        while not self._stop:
            try:
                conn, _ = self._srv.accept()
            except socket.timeout:
                continue
            try:
                buf = b""
                while not self._stop:
                    data = conn.recv(4096)
                    if not data:
                        break
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        cmd = line.decode("latin-1").rstrip("\r")
                        self.received.append(cmd)
                        self._respond(conn, cmd)
            except ConnectionAbortedError:
                pass
            except OSError:
                pass
            finally:
                conn.close()

    def close(self):
        self._stop = True
        self._thread.join(timeout=2)
        self._srv.close()
