#!/usr/bin/env python3
"""Real GTK -> nested Wayland -> Brave typing, without desktop input or focus."""
import argparse
import http.server
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import tempfile
import threading
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--gnosis', required=True, type=Path)
p.add_argument('--probe', required=True, type=Path)
p.add_argument('--mutter', required=True, type=Path)
p.add_argument('--log', type=Path, default=Path('/tmp/gnosis-keyboard-layout.log'))
p.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
if not a.inside:
    with tempfile.TemporaryDirectory(prefix='gnosis-keyboard-') as directory, a.log.open('w') as log:
        root = Path(directory)
        runtime = root / 'runtime'
        runtime.mkdir(mode=0o700)
        config = root / 'bus.conf'
        config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>'
                          '<policy context="default"><allow send_destination="*"/><allow receive_sender="*"/>'
                          '<allow own="*"/></policy></busconfig>')
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_DATA_HOME=str(root / 'data'), XDG_CACHE_HOME=str(root / 'cache'),
                   GSETTINGS_BACKEND='memory', GTK_A11Y='none', GDK_BACKEND='wayland',
                   WAYLAND_DISPLAY='keyboard-test', XKB_DEFAULT_LAYOUT='pl,de',
                   XKB_DEFAULT_OPTIONS='grp:alt_shift_toggle')
        for key in ('DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'LD_PRELOAD'):
            env.pop(key, None)
        proc = subprocess.Popen(['dbus-run-session', '--config-file=' + str(config), '--', sys.executable,
                                 str(Path(__file__).resolve()), *sys.argv[1:], '--inside'],
                                env=env, stdout=log, stderr=log, start_new_session=True)
        try:
            result = proc.wait(timeout=100)
        finally:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            proc.wait(timeout=5)
    if result or 'KEYBOARD_LAYOUT_PASS' not in a.log.read_text():
        raise SystemExit(f'Keyboard-layout regression failed; see {a.log}')
    print('KEYBOARD_LAYOUT_PASS: native Wayland AltGr and layout switching')
    raise SystemExit(0)

compositor = subprocess.Popen([str(a.mutter), '--headless', '--wayland', '--no-x11',
                               '--virtual-monitor', '1024x768', '--wayland-display', 'keyboard-test'],
                              env=dict(os.environ, LD_PRELOAD=str(a.probe.resolve())))
class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'<!doctype html><title>Keyboard layout fixture</title><input autofocus><script>const keys=[];addEventListener("keydown",e=>keys.push({key:e.key,trusted:e.isTrusted}));</script>'
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_):
        pass
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    for _ in range(100):
        assert compositor.poll() is None, 'Nested Mutter exited'
        if (Path(os.environ['XDG_RUNTIME_DIR']) / 'keymap-ready').exists():
            break
        time.sleep(.1)
    else:
        raise AssertionError('Nested Wayland display unavailable')
    env = dict(os.environ, LD_PRELOAD=str(a.probe.resolve()))
    subprocess.run([str(a.gnosis.resolve()), '--incognito-mode', '--profile=' + str(Path(os.environ['XDG_DATA_HOME']) / 'profile'),
                    f'http://127.0.0.1:{server.server_port}/keyboard-layout.html'],env=env,check=True,timeout=75)
finally:
    server.shutdown()
    compositor.terminate()
    compositor.wait(timeout=5)
