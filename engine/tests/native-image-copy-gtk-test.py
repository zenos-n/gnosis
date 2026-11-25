#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check native Copy Image with WebP and SVG in an isolated GTK display and profile."""
import base64
import argparse
import http.server
import os
from pathlib import Path
import resource
import signal
import shutil
import subprocess
import tempfile
import threading

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--browser', required=True)
parser.add_argument('--probe', required=True)
parser.add_argument('--xvfb', default='Xvfb')
parser.add_argument('--xdotool', default='xdotool')
parser.add_argument('--log', default='/tmp/gnosis-image-copy.log')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        # Owned lossless 64x48 red WebP, intentionally served under a JPEG filename.
        if self.path.startswith('/picture.jpeg'):
            mime = 'image/webp'
            body = base64.b64decode('UklGRiIAAABXRUJQVlA4TBUAAAAvP8ALAAcQ/Y/+B4CE8H+9GNH/1A8A')
        elif self.path == '/picture.svg':
            mime = 'image/svg+xml'
            body = b'<svg xmlns="http://www.w3.org/2000/svg" width="64" height="48"><path fill="blue" d="M0 0h64v48H0z"/></svg>'
        else:
            mime = 'text/html'
            image = 'picture.svg' if self.path == '/embedded-svg' else 'picture.jpeg?format=webp'
            # Different origin, no CORS headers, displayed smaller than its original pixels.
            body = f'<!doctype html><img style="width:32px;height:24px" src="http://localhost:{self.server.server_port}/{image}">'.encode()
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
with tempfile.TemporaryDirectory(prefix='gnosis-image-copy-') as directory, open(args.log, 'w') as log:
    root = Path(directory)
    display = subprocess.Popen([args.xvfb, '-displayfd', '1', '-screen', '0', '1280x960x24',
                                '-nolisten', 'tcp'], stdout=subprocess.PIPE, stderr=log, text=True)
    process = None
    try:
        number = display.stdout.readline().strip()
        assert number.isdigit(), 'Xvfb failed to start'
        bus = root / 'bus.conf'
        bus.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>'
                       '<auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/>'
                       '<allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
        env = dict(os.environ, DISPLAY=f':{number}', GDK_BACKEND='x11', GSK_RENDERER='cairo',
                   GSETTINGS_BACKEND='memory', GTK_A11Y='none', GNOSIS_XDOTOOL=shutil.which(args.xdotool) or args.xdotool)
        for key, name in [('HOME', 'home'), ('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'),
                          ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')]:
            path = root / name
            path.mkdir(mode=0o700)
            env[key] = str(path)
        for key in ['WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'SESSION_MANAGER']:
            env.pop(key, None)
        command = ['dbus-run-session', f'--config-file={bus}', '--', 'env',
                   f'LD_PRELOAD={Path(args.probe).resolve()}', args.browser, f'--profile={root / "profile"}']
        origin = f'http://127.0.0.1:{server.server_port}'
        env['GNOSIS_IMAGE_COPY_ORIGIN'] = origin
        command.append(origin + '/picture.jpeg?format=webp')
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log, start_new_session=True)
        status = process.wait(timeout=50)

    finally:
        if process is not None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        display.terminate()
        display.wait(timeout=5)
        server.shutdown()
output = Path(args.log).read_text()
print('\n'.join(line for line in output.splitlines() if 'IMAGE_COPY_' in line))
assert status == 0 and 'IMAGE_COPY_PASS' in output, f'Native probe failed; see {args.log}'
