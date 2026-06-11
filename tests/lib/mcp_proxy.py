#!/usr/bin/env python3
"""Drive a pose64-mcp-proxy process over stdio JSON-RPC for tests."""

import json
import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class McpProxy:
    def __init__(self, port, binary=None, recv_timeout=5):
        binary = binary or os.path.join(REPO, "build", "pose64-mcp-proxy")
        self.proc = subprocess.Popen(
            [binary, "--port", str(port), "--recv-timeout", str(recv_timeout)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True)
        self._id = 0
        self.request("initialize", {})

    def request(self, method, params):
        self._id += 1
        msg = {"jsonrpc": "2.0", "id": self._id, "method": method, "params": params}
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("proxy closed stdout")
        return json.loads(line)

    def tools_list(self):
        return self.request("tools/list", {})["result"]["tools"]

    def call(self, name, arguments=None):
        return self.request("tools/call",
                            {"name": name, "arguments": arguments or {}})["result"]

    @staticmethod
    def text(result):
        return "".join(c["text"] for c in result["content"] if c["type"] == "text")

    @staticmethod
    def is_error(result):
        return result.get("isError", False)

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=5)
