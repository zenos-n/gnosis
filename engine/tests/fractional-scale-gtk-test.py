#!/usr/bin/env python3
"""Check actual Brave raster buffers on a private fractional-scale Mutter display."""
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
p.add_argument('--log', type=Path, default=Path('/tmp/gnosis-fractional-scale.log'))
p.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
if not a.inside:
    with tempfile.TemporaryDirectory(prefix='gnosis-fractional-') as directory, a.log.open('w') as log:
        root = Path(directory)
        (root / 'runtime').mkdir(mode=0o700)
        (root / 'home').mkdir(mode=0o700)
        config = root / 'bus.conf'
        config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>'
                          '<policy context="default"><allow send_destination="*"/><allow receive_sender="*"/>'
                          '<allow own="*"/></policy></busconfig>')
        env = dict(os.environ, HOME=str(root / 'home'), XDG_RUNTIME_DIR=str(root / 'runtime'),
                   XDG_CONFIG_HOME=str(root / 'config'), XDG_DATA_HOME=str(root / 'data'),
                   XDG_CACHE_HOME=str(root / 'cache'), GSETTINGS_BACKEND='keyfile',
                   GDK_BACKEND='wayland', GTK_A11Y='none', WAYLAND_DISPLAY='fractional-test')
        for key in ('DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'LD_PRELOAD'):
            env.pop(key, None)
        proc = subprocess.Popen(['dbus-run-session', '--config-file=' + str(config), '--', sys.executable,
                                 str(Path(__file__).resolve()), *sys.argv[1:], '--inside'],
                                env=env, stdout=log, stderr=log, start_new_session=True)
        try:
            result = proc.wait(timeout=120)
        finally:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            proc.wait(timeout=5)
    if result or 'FRACTIONAL_SCALE_PASS' not in a.log.read_text():
        raise SystemExit(f'Fractional-scale regression failed; see {a.log}')
    print('FRACTIONAL_SCALE_PASS: actual buffers at100/166.67/200/125/100%, live transitions')
    raise SystemExit(0)

compositor = subprocess.Popen([str(a.mutter), '--headless', '--wayland', '--no-x11',
                               '--virtual-monitor', '3840x2160', '--wayland-display', 'fractional-test'])
class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'<!doctype html><meta charset=utf-8><title>Fractional scaling fixture</title><style>body{font:16px sans-serif;background:white;color:black}button,input,select{font:inherit}</style><h1>Native resolution</h1><p>Sharp text at fractional scale: ABC abc 0123456789.</p><input value="Text input"><button>Button</button><select><option>Native menu</option><option>Second option</option></select>'
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_): pass
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    for _ in range(100):
        assert compositor.poll() is None, 'Private Mutter exited'
        if (Path(os.environ['XDG_RUNTIME_DIR']) / 'fractional-test').is_socket():
            break
        time.sleep(.1)
    else:
        raise AssertionError('Private Wayland display unavailable')
    env = dict(os.environ, LD_PRELOAD=str(a.probe.resolve()), GNOSIS_FRACTIONAL_TEST='1')
    subprocess.run([str(a.gnosis.resolve()), '--incognito-mode',
                    '--profile=' + str(Path(os.environ['XDG_DATA_HOME']) / 'profile'),
                    f'http://127.0.0.1:{server.server_port}/fractional.html'], env=env, check=True, timeout=100)
finally:
    server.shutdown()
    compositor.terminate()
    compositor.wait(timeout=5)
