#!/usr/bin/env python3
"""Repro 1.3 — MCP proxy honesty (landmine #4).

Two defects in pose64-mcp-proxy.cpp:
  (a) tcp_connect sets no SO_RCVTIMEO, so a wedged ReControl server makes the
      proxy hang forever on recv — freezing every subsequent MCP call.
  (b) rc_command / rc_command_multi reconnect-and-RESEND on any read failure,
      double-executing non-idempotent commands (install/key/type/poke/delete).

This test drives the real proxy binary over JSON-RPC stdin/stdout against a fake
TCP server, with no emulator involved.

PRE-FIX  -> (a) proxy hangs on a silent server; (b) install is sent twice -> FAIL
POST-FIX -> (a) bounded timeout error; (b) install sent at most once          -> PASS
"""
import json
import os
import select
import socket
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROXY = os.path.join(REPO, "build", "pose64-mcp-proxy")


def start_fake_server(on_accept):
    """on_accept(conn) is called for each connection in its own thread."""
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.listen(8)

    def loop():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=on_accept, args=(conn,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()
    return srv, port


def rpc_line(_id, name, arguments):
    return json.dumps({
        "jsonrpc": "2.0", "id": _id, "method": "tools/call",
        "params": {"name": name, "arguments": arguments},
    }) + "\n"


def read_line_timeout(stream, timeout):
    """Read one line from a file object's underlying fd with a timeout."""
    fd = stream.fileno()
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], end - time.time())
        if not r:
            break
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        buf += chunk
        if b"\n" in buf:
            return buf.split(b"\n", 1)[0].decode("utf-8", "replace")
    return None


# ---------------------------------------------------------------------------

def test_no_double_execute():
    seen = []

    def on_accept(conn):
        # Read whatever the proxy sends, record it, then drop the connection
        # immediately (simulating the server closing mid-command).
        try:
            data = conn.recv(4096)
            if data:
                seen.append(data.decode("latin-1"))
        finally:
            conn.close()

    srv, port = start_fake_server(on_accept)
    proxy = subprocess.Popen(
        [PROXY, "--port", str(port), "--recv-timeout", "3"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        proxy.stdin.write(rpc_line(1, "palm_install",
                                   {"path": "/tmp/repro13.prc"}).encode())
        proxy.stdin.flush()
        resp = read_line_timeout(proxy.stdout, 6)
        assert resp is not None, "proxy gave no response to palm_install"
    finally:
        proxy.terminate()
        try:
            proxy.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proxy.kill()
        srv.close()

    installs = [d for d in seen if d.startswith("install")]
    if len(installs) > 1:
        raise AssertionError(
            f"DOUBLE-EXECUTE: 'install' sent {len(installs)}x after a dropped "
            f"connection (non-idempotent command must never be auto-resent)")
    print(f"  no-double-execute: install seen {len(installs)}x (<=1) OK")


def test_timeout_not_hang():
    holds = []

    def on_accept(conn):
        # Read the command, then hold the connection open and never reply.
        try:
            conn.recv(4096)
            holds.append(conn)
            while True:
                time.sleep(1)
        except OSError:
            pass

    srv, port = start_fake_server(on_accept)
    proxy = subprocess.Popen(
        [PROXY, "--port", str(port), "--recv-timeout", "2"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        t0 = time.time()
        proxy.stdin.write(rpc_line(2, "palm_install",
                                   {"path": "/tmp/repro13.prc"}).encode())
        proxy.stdin.flush()
        resp = read_line_timeout(proxy.stdout, 8)  # generous: timeout is 2s
        dt = time.time() - t0
        if resp is None:
            raise AssertionError(
                "HANG: proxy never responded to a silent server (no SO_RCVTIMEO)")
        assert dt < 6.0, f"proxy timeout not bounded ({dt:.1f}s)"
        assert "timeout" in resp.lower(), f"expected a timeout error, got: {resp[:120]}"
        print(f"  silent-server: bounded timeout error in {dt:.1f}s OK")
    finally:
        proxy.terminate()
        try:
            proxy.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proxy.kill()
        for c in holds:
            try:
                c.close()
            except OSError:
                pass
        srv.close()


def main():
    if not os.path.isfile(PROXY):
        raise SystemExit(f"proxy not built: {PROXY}")
    test_no_double_execute()
    test_timeout_not_hang()
    print("PASS 1.3 — proxy: bounded recv timeout + no double-execute")


if __name__ == "__main__":
    main()
