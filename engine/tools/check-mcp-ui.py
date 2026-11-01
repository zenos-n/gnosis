#!/usr/bin/env python3
"""Test the shipping Gnosis MCP against real GTK/Brave with local web fixtures."""
import argparse
import ast
import base64
import http.server
import json
import os
from pathlib import Path
import resource
import re
import signal
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from mcp_protocol_test import Client


def wait_for(function, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = function()
        if value:
            return value
        time.sleep(.1)
    raise TimeoutError("Condition did not become true")


class Fixture(http.server.BaseHTTPRequestHandler):
    private_cookies = []
    def log_message(self, *_args):
        pass

    def do_GET(self):
        if self.path.startswith('/?private'):
            self.private_cookies.append(self.headers.get('Cookie', ''))
        if self.path == "/slow":
            time.sleep(5)
        if self.path == "/second":
            text = "<!doctype html><title>Second page</title><h1>Second page</h1>"
        elif self.path == "/frame":
            text = """<!doctype html><style>body{padding:10px}input{padding:8px}</style>
            <input aria-label='Frame input' id=entry><script>
            entry.addEventListener('input',event=>parent.postMessage({value:entry.value,trusted:event.isTrusted},'*'));
            </script>"""
        else:
            text = """<!doctype html><meta charset=utf-8><title>MCP fixture</title>
            <style>body{font:18px sans-serif;padding:20px}input,button{font:inherit;padding:12px}#space{height:1600px}</style>
            <h1>Gnosis MCP fixture</h1><label>Name <input aria-label='Name' id=name></label>
            <button onclick="result.textContent=nameInput.value+' trusted='+event.isTrusted">Apply</button>
            <p id=result>Not applied</p><a href=/second>Next page</a>
            <button onclick="window.open('/second','mcp-popup','width=500,height=400')">Popup</button>
            <div id=shadow></div><iframe title='Child frame' src=FRAME_URL></iframe><p id=cookie></p><div id=space></div>
            <script>
            shadow.attachShadow({mode:'open'}).innerHTML='<button>Shadow button</button>';
            if(!location.search)document.cookie='gnosis_fixture=shared;path=/';cookie.textContent='cookie: '+document.cookie;
            const nameInput=document.getElementById('name');
            window.inputTrusted=[]; nameInput.addEventListener('input',event=>inputTrusted.push(event.isTrusted));
            nameInput.addEventListener('keydown',event=>{if(event.key==='Enter')result.textContent=nameInput.value+' enter='+event.isTrusted});
            window.addEventListener('message',event=>{if(event.source===document.querySelector('iframe').contentWindow)
              result.textContent='Frame: '+event.data.value+' trusted='+event.data.trusted});
            </script>"""
            text = text.replace('FRAME_URL', f'http://localhost:{self.server.server_port}/frame')
        body = text.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except BrokenPipeError:
            pass


def gdbus(bus, method, *args, path="/com/neg_zero/Gnosis"):
    return subprocess.run(["gdbus", "call", "--session", "--dest", bus,
                           "--object-path", path, "--method", method, *args],
                          capture_output=True, text=True, timeout=8)


def discover(pid, require_mcp=True):
    names = gdbus("org.freedesktop.DBus", "org.freedesktop.DBus.ListNames",
                  path="/org/freedesktop/DBus")
    for name in ast.literal_eval(names.stdout)[0]:
        if not name.startswith(":"):
            continue
        owner = gdbus("org.freedesktop.DBus", "org.freedesktop.DBus.GetConnectionUnixProcessID",
                      json.dumps(name), path="/org/freedesktop/DBus")
        match = re.search(r"uint32 (\d+)", owner.stdout)
        if not match or int(match[1]) != pid:
            continue
        if not require_mcp:
            return name
        try:
            result = gdbus(name, "com.neg_zero.Gnosis.BrowserControl1.Execute", '"get_state"', '"{}"',
                           path="/com/neg_zero/Gnosis/BrowserControl")
        except subprocess.TimeoutExpired:
            continue
        if result.returncode == 0:
            return name


def run(args):
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_port}"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    state_file = output / "gtk.json"
    for suffix in [".cleanup", ".command", ".capture"]:
        state_file.with_name(state_file.name + suffix).unlink(missing_ok=True)
    clients = []
    private = None
    with tempfile.TemporaryDirectory(prefix="gnosis-mcp-test-") as temp:
        env = dict(os.environ, GSETTINGS_BACKEND="memory", GTK_A11Y="none", GIO_USE_VFS="local",
                   LANG="C.UTF-8",
                   GDK_BACKEND="x11", GSK_RENDERER="cairo", GNOSIS_SOFTWARE_RENDERING="1",
                   GNOSIS_MCP_TEST_STATE=str(state_file), LD_PRELOAD=str(args.probe.resolve()))
        env.pop("WAYLAND_DISPLAY", None)
        if args.desktop:
            for key in ["DISPLAY", "WAYLAND_DISPLAY", "GDK_BACKEND", "GSK_RENDERER", "GNOSIS_SOFTWARE_RENDERING"]:
                env.pop(key, None)
                if key in os.environ:
                    env[key] = os.environ[key]
        with (output / "browser.log").open("w") as log:
            process = subprocess.Popen([str(args.browser.resolve()), "--profile=" + temp, url + "/"],
                                       env=env, stdout=log, stderr=log, start_new_session=True)
            try:
                bus = wait_for(lambda: discover(process.pid), 50)
                client = Client(args.mcp, bus)
                clients.append(client)
                client.initialize()

                def action(name, value=None):
                    parameters = "[]" if value is None else "[<'" + value + "'>]"
                    result = gdbus(bus, "org.gtk.Actions.Activate", json.dumps(name), parameters, "{}")
                    assert result.returncode == 0, result.stderr

                def native():
                    try:
                        state = json.loads(state_file.read_text())
                        return state["windows"] if state.get("pid") == process.pid else []
                    except (FileNotFoundError, json.JSONDecodeError):
                        return []

                def snapshot(tab=None, image=False):
                    arguments = {"screenshot": image}
                    if tab:
                        arguments["tab_id"] = tab
                    result = client.call("snapshot", arguments)
                    return json.loads(result["content"][0]["text"]), result

                def tab_list():
                    return json.loads(client.call("list_tabs")["content"][0]["text"])["tabs"]

                def ref(name, info):
                    return next(element["ref"] for element in info["elements"] if element["name"] == name)

                assert json.loads(client.call("get_state")["content"][0]["text"])["mode"] == "off"
                if args.session_bus:
                    bare_env = {key: value for key, value in os.environ.items() if key not in {
                        "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR", "DISPLAY", "WAYLAND_DISPLAY"}}
                    bare = Client(args.mcp, bus, env=bare_env)
                    try:
                        bare.initialize()
                        assert json.loads(bare.call("get_state")["content"][0]["text"])["mode"] == "off"
                    finally:
                        bare.close()
                    print("MCP_DESKTOP_BUS_PASS browser reached without session/display environment", flush=True)
                client.call("list_tabs", error=True)
                action("ai-browsing-mode", "shared")
                wait_for(lambda: native() and json.loads(state_file.read_text())["mode"] == "shared")
                assert all(not window["header_control"] and set(window["menu_modes"]) == {"off","shared","separate"}
                           for window in native()), native()
                user = tab_list()[0]
                user_id = user["tab_id"]
                # Navigate through the actual native loading path before reading its renderer.
                client.call("navigate", {"tab_id": user_id, "url": url + "/"})
                info, result = snapshot(user_id, True)
                assert "Gnosis MCP fixture" in info["text"]
                assert any(element["name"] == "Shadow button" for element in info["elements"])
                png = next(item for item in result["content"] if item["type"] == "image")
                assert base64.b64decode(png["data"]).startswith(b"\x89PNG\r\n\x1a\n")
                (output / "viewport.png").write_bytes(base64.b64decode(png["data"]))
                name_ref = ref("Name", info)
                client.call("move", {"tab_id": user_id, "ref": name_ref})
                wait_for(lambda: any(window["cursor"] for window in native()))
                (state_file.with_name(state_file.name + ".capture")).write_text("shared")
                wait_for(lambda: not state_file.with_name(state_file.name + ".capture").exists())
                client.call("type_text", {"tab_id": user_id, "ref": name_ref, "text": "Żółw 🦊"})
                client.call("click", {"tab_id": user_id, "ref": ref("Apply", info)})
                assert "Żółw 🦊 trusted=true" in snapshot(user_id)[0]["text"]
                client.call("type_text", {"tab_id": user_id, "ref": name_ref, "text": "Replaced", "replace": True})
                client.call("press_key", {"tab_id": user_id, "key": "Enter"})
                assert "Replaced enter=true" in snapshot(user_id)[0]["text"]
                frame = next(element for element in snapshot(user_id)[0]["elements"] if element["tag"] == "iframe")
                client.call("click", {"tab_id": user_id, "x": frame["bounds"]["x"] + 35, "y": frame["bounds"]["y"] + 35})
                client.call("type_text", {"tab_id": user_id, "text": "Frame 🦊"})
                assert "Frame: Frame 🦊 trusted=true" in snapshot(user_id)[0]["text"]
                client.call("click", {"tab_id": user_id, "ref": "stale-reference"}, error=True)
                client.call("navigate", {"tab_id": user_id, "url": "file:///etc/passwd"}, error=True)
                client.call("navigate", {"tab_id": user_id, "url": "chrome://settings"}, error=True)
                client.call("click", {"tab_id": user_id, "x": -1, "y": 0}, error=True)
                client.call("snapshot", {"tab_id": user_id, "unknown": True}, error=True)
                client.call("scroll", {"tab_id": user_id, "delta_y": 400})
                client.call("scroll", {"tab_id": user_id, "delta_y": -400})
                client.call("navigate", {"tab_id": user_id, "url": url + "/second"})
                client.call("click", {"tab_id": user_id, "ref": name_ref}, error=True)
                client.call("go_back", {"tab_id": user_id})
                client.call("go_forward", {"tab_id": user_id})
                client.call("go_back", {"tab_id": user_id})
                stable_ref = ref("Name", snapshot(user_id)[0])
                other = json.loads(client.call("open_tab", {"url": url + "/second"})["content"][0]["text"])
                client.call("activate_tab", {"tab_id": user_id})
                client.call("move", {"tab_id": user_id, "ref": stable_ref})
                client.call("close_tab", {"tab_id": other["tab_id"]})
                wait_for(lambda: all(tab["tab_id"] != other["tab_id"] for tab in tab_list()))
                print("MCP_SHARED_PASS real text/input/click/key events, screenshot, native cursor, shadow targets, history, validation", flush=True)

                competitor = Client(args.mcp, bus)
                clients.append(competitor)
                competitor.initialize()
                competitor.call("list_tabs", error=True)
                client.close()
                time.sleep(.3)
                competitor.call("list_tabs")
                client = competitor
                print("MCP_OWNER_PASS controller lease and disconnect recovery", flush=True)

                def window_list():
                    return json.loads(client.call("list_windows")["content"][0]["text"])["windows"]

                assert [window["window_id"] for window in window_list()] == [user["window_id"]]
                client.call("activate_window", {"window_id": "missing"}, error=True)
                client.call("open_tab", {"window_id": "missing"}, error=True)
                action("new-window")
                windows = wait_for(lambda: window_list() if len(window_list()) == 2 else None)
                second_id = next(window["window_id"] for window in windows if window["window_id"] != user["window_id"])
                selected = json.loads(client.call("activate_window", {"window_id": second_id})["content"][0]["text"])
                assert selected["selected"] and selected["tabs"]
                second_tab = json.loads(client.call("open_tab", {"url": url + "/second"})["content"][0]["text"])
                assert second_tab["window_id"] == second_id
                # Reordering GTK windows must not redirect default operations.
                client.call("activate_window", {"window_id": user["window_id"]})
                targeted = json.loads(client.call("open_tab", {"window_id": second_id, "url": url + "/second"})["content"][0]["text"])
                assert targeted["window_id"] == second_id
                assert "Second page" in snapshot()[0]["text"]
                assert next(window for window in window_list() if window["selected"])["window_id"] == second_id
                second_tabs = json.loads(client.call("list_tabs", {"window_id": second_id})["content"][0]["text"])["tabs"]
                assert second_tabs and all(tab["window_id"] == second_id for tab in second_tabs)
                client.call("activate_tab", {"tab_id": user_id})
                assert "Gnosis MCP fixture" in snapshot()[0]["text"]
                client.call("activate_window", {"window_id": second_id})
                for tab in second_tabs:
                    client.call("close_tab", {"tab_id": tab["tab_id"]})
                wait_for(lambda: all(window["window_id"] != second_id for window in window_list()))
                client.call("snapshot", {"screenshot": False}, error=True)
                client.call("open_tab", error=True)
                client.call("activate_window", {"window_id": user["window_id"]})
                print("MCP_WINDOWS_PASS native window discovery, explicit targeting, persistent selection, closed-window refusal", flush=True)

                action("ai-browsing-mode", "separate")
                wait_for(lambda: any(window["ai_window"] for window in native()))
                ai = json.loads(client.call("open_tab", {"url": url + "/?ai"})["content"][0]["text"])
                assert ai["window_id"] != user["window_id"] and ai["tab_id"] != user_id
                assert all(tab["window_id"] != user["window_id"] for tab in tab_list())
                assert all(window["window_id"] != user["window_id"] for window in window_list())
                client.call("activate_window", {"window_id": user["window_id"]}, error=True)
                client.call("snapshot", {"tab_id": user_id}, error=True)
                ai_info, _ = snapshot(ai["tab_id"])
                assert "gnosis_fixture=shared" in ai_info["text"]
                client.call("move", {"tab_id": ai["tab_id"], "ref": ref("Name", ai_info)})
                (state_file.with_name(state_file.name + ".capture")).write_text("separate")
                wait_for(lambda: not state_file.with_name(state_file.name + ".capture").exists())
                closing = json.loads(client.call("open_tab")["content"][0]["text"])
                client.call("close_tab", {"tab_id": closing["tab_id"]})
                wait_for(lambda: all(tab["tab_id"] != closing["tab_id"] for tab in tab_list()))
                action("ai-browsing-mode", "shared")
                client.call("activate_window", {"window_id": ai["window_id"]}, error=True)
                client.call("snapshot", {"tab_id": ai["tab_id"]}, error=True)
                assert all(tab["window_id"] == user["window_id"] for tab in tab_list())
                print("MCP_SEPARATE_PASS own native window/tabs, shared profile, two-way mode isolation and normal close", flush=True)

                # Off revokes a real in-flight navigation without waiting for the HTTP fixture.
                client.sequence += 1
                ident = client.sequence
                client.send({"jsonrpc": "2.0", "id": ident, "method": "tools/call", "params": {
                    "name": "navigate", "arguments": {"tab_id": user_id, "url": url + "/slow"}}})
                time.sleep(.15)
                started = time.monotonic()
                action("ai-browsing-mode", "off")
                stopped = client.receive(3)
                assert stopped["id"] == ident and stopped["result"]["isError"]
                assert time.monotonic() - started < 3
                client.call("list_tabs", error=True)
                wait_for(lambda: all(not window["cursor"] for window in native()))
                print("MCP_STOP_PASS off cancels pending work and removes native cursor", flush=True)

                # Exercise the actual native error widget's layout without killing the renderer.
                command_file = state_file.with_name(state_file.name + ".command")
                command_file.write_text("show-retry")
                retry = wait_for(lambda: next((window for window in native() if not window["ai_window"] and
                                               window["retry_visible"] and window["retry_width"] > 0), None))
                assert retry["retry_pill"] and retry["retry_centered"] and retry["retry_width"] < retry["window_width"] / 2, retry
                capture_file = state_file.with_name(state_file.name + ".capture")
                capture_file.write_text("retry")
                wait_for(lambda: not capture_file.exists())
                command_file.write_text("hide-retry")
                wait_for(lambda: not command_file.exists())
                print("MCP_MENU_RETRY_PASS hamburger mode choices; content-sized native pill retry button", flush=True)

                # Closing the actual AI window must revoke control before its child widgets are disposed.
                action("ai-browsing-mode", "separate")
                state_file.with_name(state_file.name + ".cleanup").write_text("1")
                command_file.write_text("close-ai")
                wait_for(lambda: native() and all(not window["ai_window"] for window in native()))
                assert process.poll() is None
                assert json.loads(client.call("get_state")["content"][0]["text"])["mode"] == "off"
                print("MCP_WINDOW_CLOSE_PASS native AI window close disables control without crashing the user window", flush=True)

                private_state = output / "private.json"
                for suffix in [".cleanup", ".command", ".capture"]:
                    private_state.with_name(private_state.name + suffix).unlink(missing_ok=True)
                private_log = (output / "private.log").open("w")
                private = subprocess.Popen([str(args.browser.resolve()), "--incognito-mode", "--profile=" + temp, url + "/?private"],
                                           env=dict(env, GNOSIS_MCP_TEST_STATE=str(private_state)),
                                           stdout=private_log, stderr=private_log, start_new_session=True)
                private_bus = wait_for(lambda: discover(private.pid, False), 40)
                wait_for(lambda: Fixture.private_cookies, 40)
                assert Fixture.private_cookies == [""], Fixture.private_cookies
                unavailable = gdbus(private_bus, "com.neg_zero.Gnosis.BrowserControl1.Execute", '"get_state"', '"{}"',
                                    path="/com/neg_zero/Gnosis/BrowserControl")
                assert unavailable.returncode != 0, "Incognito exposed MCP control"
                action("ai-browsing-mode", "shared")
                assert all("?private" not in tab["url"] for tab in tab_list())
                private_state.with_name(private_state.name + ".cleanup").write_text("1")
                gdbus(private_bus, "org.gtk.Actions.Activate", '"quit"', "[]", "{}")
                private.wait(timeout=15)
                assert private.returncode == 0, private.returncode
                private_log.close()
                print("MCP_PRIVATE_PASS real incognito profile has no shared cookie or MCP endpoint; private tabs excluded", flush=True)
            finally:
                for client in clients:
                    client.close()
                state_file.with_name(state_file.name + ".cleanup").write_text("1")
                if private is not None and private.poll() is None:
                    os.killpg(private.pid, signal.SIGKILL)
                    private.wait(timeout=5)
                if process.poll() is None:
                    try:
                        action("quit")
                        process.wait(timeout=15)
                    except (Exception, subprocess.TimeoutExpired):
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
                        raise
                for name in ["stderr.log", "stdout.log", "launcher-stderr.log"]:
                    source = Path(temp) / "brave" / name
                    if source.exists():
                        shutil.copyfile(source, output / ("brave-" + name))
                server.shutdown()
            assert process.returncode == 0, process.returncode
            print("MCP_UI_PASS shipping browser, real engine, disposable profiles, sandbox enabled, clean shutdown", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", required=True, type=Path)
    parser.add_argument("--mcp", required=True, type=Path)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=Path("/tmp/gnosis-mcp-check"))
    parser.add_argument("--desktop", action="store_true", help="Use the current desktop display")
    parser.add_argument("--session-bus", action="store_true", help="Keep the desktop bus; control only the disposable browser PID")
    arguments = parser.parse_args()
    if not arguments.session_bus and os.environ.get("GNOSIS_MCP_TEST_BUS") != "1":
        with tempfile.TemporaryDirectory(prefix="gnosis-mcp-bus-") as bus_dir:
            config = Path(bus_dir) / "session.conf"
            config.write_text("""<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>
            <auth>EXTERNAL</auth><policy context="default"><allow own="*"/>
            <allow send_destination="*"/><allow receive_sender="*"/></policy></busconfig>""")
            command = ["dbus-run-session", "--config-file=" + str(config), "--", sys.executable, __file__, *sys.argv[1:]]
            raise SystemExit(subprocess.call(command, env=dict(os.environ, GNOSIS_MCP_TEST_BUS="1")))
    run(arguments)
