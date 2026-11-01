#!/usr/bin/env python3
"""Exercise the packaged stdio MCP transport without opening a human profile."""
import argparse
import json
import os
import select
import subprocess
import tempfile
from pathlib import Path


class Client:
    def __init__(self, server, bus=None, env=None):
        command = [str(server)] + (["--bus-name", bus] if bus else [])
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)
        self.sequence = 0

    def send(self, value):
        self.process.stdin.write(json.dumps(value, ensure_ascii=False) + "\n")
        self.process.stdin.flush()

    def receive(self, timeout=35):
        assert select.select([self.process.stdout], [], [], timeout)[0], "MCP response timed out"
        line = self.process.stdout.readline()
        assert line, "MCP server closed stdout"
        return json.loads(line)

    def rpc(self, method, params=None):
        self.sequence += 1
        request = {"jsonrpc": "2.0", "id": self.sequence, "method": method}
        if params is not None:
            request["params"] = params
        self.send(request)
        response = self.receive()
        assert response["jsonrpc"] == "2.0" and response["id"] == self.sequence, response
        return response

    def initialize(self, version="2025-11-25"):
        result = self.rpc("initialize", {"protocolVersion": version, "capabilities": {},
                                         "clientInfo": {"name": "gnosis-test", "version": "1"}})
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return result["result"]

    def call(self, name, args=None, error=False):
        response = self.rpc("tools/call", {"name": name, "arguments": args or {}})
        assert "result" in response, response
        result = response["result"]
        assert result.get("isError", False) == error, result
        return result

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=5)


def test(server):
    client = Client(server)
    try:
        assert client.rpc("tools/list")["error"]["code"] == -32002
        init = client.initialize("future-version")
        assert init["protocolVersion"] == "2025-11-25" and init["capabilities"] == {"tools": {}}
        tools = client.rpc("tools/list")["result"]["tools"]
        names = {tool["name"] for tool in tools}
        assert {"snapshot", "click", "move", "type_text", "scroll", "press_key", "list_tabs", "open_tab",
                "list_windows", "activate_window"} <= names
        assert "set_mode" not in names
        assert all(tool["inputSchema"]["additionalProperties"] is False for tool in tools)
        assert client.rpc("ping")["result"] == {}
        client.send({"jsonrpc": "2.0", "id": "雪🦊", "method": "ping"})
        assert client.receive()["id"] == "雪🦊"
        assert client.rpc("unknown")["error"]["code"] == -32601
        assert client.rpc("tools/call", {"name": "snapshot", "arguments": []})["error"]["code"] == -32602
        client.process.stdin.write("not json\n")
        client.process.stdin.flush()
        assert client.receive()["error"]["code"] == -32700
        client.process.stdin.write('"' + "x" * (1024 * 1024) + '"\n')
        client.process.stdin.flush()
        assert client.receive()["error"]["code"] == -32600
        assert client.rpc("ping")["result"] == {}
    finally:
        client.close()
    for version in ["2024-11-05", "2025-03-26", "2025-06-18"]:
        client = Client(server)
        try:
            assert client.initialize(version)["protocolVersion"] == version
        finally:
            client.close()
    print("MCP_PROTOCOL_PASS initialization, negotiation, schemas, framing, Unicode, errors, limits")

    # A real isolated user bus establishes discovery without touching a browser.
    with tempfile.TemporaryDirectory(prefix="gnosis-mcp-runtime-") as runtime:
        address = "unix:path=" + str(Path(runtime) / "bus")
        daemon = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--address=" + address,
                                   "--print-address=1"], stdout=subprocess.PIPE, text=True)
        try:
            assert daemon.stdout.readline().startswith(address)
            env = dict(os.environ, XDG_RUNTIME_DIR=runtime)
            for key in ["DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY"]:
                env.pop(key, None)
            client = Client(server, "com.neg_zero.Gnosis.MissingBrowser", env=env)
            try:
                client.initialize()
                result = client.call("get_state", error=True)
                assert "org.freedesktop.DBus.Error.NameHasNoOwner" in result["content"][0]["text"], result
            finally:
                client.close()
            # An explicit address must never silently fall back to a different bus.
            env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=" + str(Path(runtime) / "missing")
            client = Client(server, "com.neg_zero.Gnosis.MissingBrowser", env=env)
            try:
                client.initialize()
                result = client.call("get_state", error=True)
                assert "Open Gnosis" not in result["content"][0]["text"], result
            finally:
                client.close()
        finally:
            daemon.terminate()
            daemon.wait(timeout=5)
    print("MCP_BUS_PASS desktop socket discovery without display/address; explicit bus isolation")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server")
    test(parser.parse_args().server)
