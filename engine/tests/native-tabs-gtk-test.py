#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check background link tabs and session defaults using native middle clicks in an isolated profile."""
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
parser.add_argument('--repeat', type=int, choices=(1, 2), default=2)
parser.add_argument('--private', action='store_true')
parser.add_argument('--log', default='/tmp/gnosis-tabs.log')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.end_headers()
        self.wfile.write(b'<!doctype html><title>Tab close fixture</title><body><p>Tab close fixture</p>')

    def log_message(self, *_args):
        pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
with tempfile.TemporaryDirectory(prefix='gnosis-tabs-') as directory, open(args.log, 'w') as log:
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
        if args.private:
            command.append('--incognito-mode')
        command.append(f'http://127.0.0.1:{server.server_port}/fixture')
        if args.private:
            env['GNOSIS_TABS_PRIVATE'] = '1'
        for run in range(args.repeat):
            log.write(f'\nNATIVE_TABS_RESTART {run + 1}\n')
            log.flush()
            process = subprocess.Popen(command, env=env, stdout=log, stderr=log, start_new_session=True)
            status = process.wait(timeout=65)
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            assert status == 0, f'Native run {run + 1} failed'

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
print('\n'.join(line for line in output.splitlines() if 'NATIVE_TABS_' in line))
assert status == 0 and output.count('NATIVE_TABS_PASS') == args.repeat, f'Native probe failed; see {args.log}'
