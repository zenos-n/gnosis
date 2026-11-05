#!/usr/bin/env python3
"""Run the real Mutter/GTK touchscreen probe in an owned disposable profile."""
import argparse
import functools
import http.server
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import threading

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--gnosis', required=True, type=Path)
parser.add_argument('--probe', required=True, type=Path)
parser.add_argument('--log', type=Path, default=Path('/tmp/gnosis-native-touch.log'))
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
handler = functools.partial(http.server.SimpleHTTPRequestHandler,
                            directory=Path(__file__).resolve().parent / 'fixtures')
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='gnosis-native-touch-') as profile, args.log.open('w') as log:
        env = dict(os.environ, GSETTINGS_BACKEND='memory', LD_PRELOAD=str(args.probe.resolve()))
        process = subprocess.Popen([
            str(args.gnosis.resolve()), '--incognito-mode', '--profile=' + profile,
            f'http://127.0.0.1:{server.server_port}/native-touch.html',
        ], env=env, stdout=log, stderr=log, start_new_session=True)
        try:
            result = process.wait(timeout=140)
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
    output = args.log.read_text()
    if result or 'NATIVE_TOUCH_PASS' not in output:
        raise SystemExit(f'Native touch regression failed; see {args.log}')
    print('NATIVE_TOUCH_PASS: genuine GTK/Wayland touchscreen integration')
finally:
    server.shutdown()
    server.server_close()
