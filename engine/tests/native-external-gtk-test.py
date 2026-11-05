#!/usr/bin/env python3
"""Exercise the actual Chromium handler and native GTK prompt on isolated Xvfb."""
import argparse
import http.server
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import threading

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--gnosis', required=True, type=Path)
p.add_argument('--probe', required=True, type=Path)
p.add_argument('--xvfb', required=True, type=Path)
p.add_argument('--log', type=Path, default=Path('/tmp/gnosis-external-gtk.log'))
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'<!doctype html><title>External app test</title><a href="gnosis-external-test://open/path?value=a%20b">Open fixture</a>'
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_):
        pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='gnosis-external-') as directory, a.log.open('w') as log:
        root = Path(directory)
        apps = root / 'data/applications'
        apps.mkdir(parents=True)
        config = root / 'config'
        config.mkdir()
        handler = root / 'handler'
        handler.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> "$GNOSIS_EXTERNAL_LOG"\n')
        handler.chmod(0o755)
        (apps / 'gnosis-external-test.desktop').write_text(
            '[Desktop Entry]\nType=Application\nName=Gnosis Test Handler\n'
            f'Exec={handler} %u\nMimeType=x-scheme-handler/gnosis-external-test;\nNoDisplay=true\n')
        (config / 'mimeapps.list').write_text('[Default Applications]\nx-scheme-handler/gnosis-external-test=gnosis-external-test.desktop;\n')
        (apps / 'mimeinfo.cache').write_text('[MIME Cache]\nx-scheme-handler/gnosis-external-test=gnosis-external-test.desktop;\n')
        xvfb = subprocess.Popen([str(a.xvfb), '-displayfd', '1', '-screen', '0', '1280x960x24', '-nolisten', 'tcp'], stdout=subprocess.PIPE, stderr=log, text=True)
        bus_config = root / 'bus.conf'
        bus_config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>'
                              '<auth>EXTERNAL</auth><policy context="default">'
                              '<allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/>'
                              '</policy></busconfig>')
        process = None
        try:
            display = xvfb.stdout.readline().strip()
            if not display.isdigit():
                raise RuntimeError('Xvfb did not allocate an isolated display')
            env = dict(os.environ, DISPLAY=':' + display, GDK_BACKEND='x11', GSK_RENDERER='cairo',
                       GSETTINGS_BACKEND='memory', XDG_DATA_HOME=str(root / 'data'), XDG_CONFIG_HOME=str(config),
                       GNOSIS_EXTERNAL_LOG=str(root / 'calls'), GNOSIS_EXTERNAL_FIXTURE=f'http://127.0.0.1:{server.server_port}/',
                       LD_PRELOAD=str(a.probe.resolve()), LC_ALL='C.UTF-8')
            env.pop('WAYLAND_DISPLAY', None)
            env.pop('DBUS_SESSION_BUS_ADDRESS', None)
            # The real handler is a harmless log-only script. No human profile,
            # portal, desktop input or physical monitor is used.
            process = subprocess.Popen(['dbus-run-session', '--config-file=' + str(bus_config), '--', str(a.gnosis.resolve()), '--profile=' + str(root / 'profile'), env['GNOSIS_EXTERNAL_FIXTURE']],
                                       env=env, stdout=log, stderr=log, start_new_session=True)
            result = process.wait(timeout=75)
            print("Native GTK exit:", result, flush=True)
            if result == 0:
                # Same source profile, separate private browser process: a
                # remembered normal grant must not skip its confirmation.
                env['GNOSIS_EXTERNAL_PRIVATE_TEST'] = '1'
                process = subprocess.Popen(['dbus-run-session', '--config-file=' + str(bus_config), '--', str(a.gnosis.resolve()),
                                            '--incognito-mode', '--profile=' + str(root / 'profile'), env['GNOSIS_EXTERNAL_FIXTURE']],
                                           env=env, stdout=log, stderr=log, start_new_session=True)
                result = process.wait(timeout=75)
        finally:
            if process:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                process.wait(timeout=5)
            native_log = root / "profile/brave/stderr.log"
            if native_log.exists():
                log.write("\nNATIVE BROWSER STDERR\n" + native_log.read_text())
            xvfb.terminate()
            xvfb.wait(timeout=5)
    if result or not all(marker in a.log.read_text() for marker in ('EXTERNAL_GTK_PASS', 'EXTERNAL_PRIVATE_PASS')):
        raise SystemExit(f'External application regression failed; see {a.log}')
    print('EXTERNAL_GTK_PASS: real native dialog and disposable handler')
finally:
    server.shutdown()
    server.server_close()
