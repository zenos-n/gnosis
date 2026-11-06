#!/usr/bin/env python3
"""Check XDG-style URL dispatch to a real cold/running app, without desktop focus."""
import argparse
import base64
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
p.add_argument('--xvfb', required=True, type=Path)
p.add_argument('--legacy-gnosis', type=Path, help='Optional older installed launcher for remote dispatch regression')
p.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
p.add_argument('--log', type=Path, default=Path('/tmp/gnosis-web-app-links.log'))
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

if not a.inside:
    with tempfile.TemporaryDirectory(prefix='gnosis-app-links-') as directory, a.log.open('w') as log:
        root = Path(directory)
        config = root / 'bus.conf'
        config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>'
                          '<policy context="default"><allow send_destination="*"/><allow receive_sender="*"/>'
                          '<allow own="*"/></policy></busconfig>')
        display = subprocess.Popen([str(a.xvfb), '-displayfd', '1', '-screen', '0', '1280x960x24', '-nolisten', 'tcp'], stdout=subprocess.PIPE, stderr=log, text=True)
        process = None
        try:
            number = display.stdout.readline().strip()
            assert number.isdigit(), 'No isolated display'
            env = dict(os.environ, DISPLAY=':' + number, GDK_BACKEND='x11', GSK_RENDERER='cairo', GTK_A11Y='none',
                       GSETTINGS_BACKEND='memory', XDG_DATA_HOME=str(root / 'data'), XDG_CONFIG_HOME=str(root / 'config'),
                       XDG_CACHE_HOME=str(root / 'cache'), GNOSIS_WEB_APP_LINK_REPORT=str(root / 'report'),
                       LD_PRELOAD=str(a.probe.resolve()), LC_ALL='C.UTF-8')
            env.pop('WAYLAND_DISPLAY', None)
            env.pop('DBUS_SESSION_BUS_ADDRESS', None)
            process = subprocess.Popen(['dbus-run-session', '--config-file=' + str(config), '--', sys.executable,
                                        str(Path(__file__).resolve()), *sys.argv[1:], '--inside'], env=env, stdout=log, stderr=log, start_new_session=True)
            result = process.wait(timeout=100)
        finally:
            if process:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                process.wait(timeout=5)
            display.terminate()
            display.wait(timeout=5)
    if result or 'WEB_APP_LINKS_GTK_PASS' not in a.log.read_text():
        raise SystemExit(f'Web app link test failed; see {a.log}')
    print('WEB_APP_LINKS_GTK_PASS: native controls, cold/running app, launcher activation, fallback and private isolation')
    raise SystemExit(0)

class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'<!doctype html><title>Web app links fixture</title><h1>Owned fixture</h1>'
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_):
        pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
root = Path(os.environ['XDG_DATA_HOME'])
app_id = 'com.neg-zero.Gnosis.WebApp_linkfixture'
profile = root / app_id
profile.mkdir(parents=True)
(profile / '.app').touch()
version = Path(__file__).resolve().parents[2] / 'lib/ephy-profile-utils.h'
import re
(profile / '.migrated').write_text(re.search(r'EPHY_PROFILE_MIGRATION_VERSION (\d+)', version.read_text())[1])
(profile / 'web-app.ini').write_text('[Extensions]\nenabled=\n[Links]\nenabled=true\ndomains=localhost;\n')
apps = root / 'applications'
apps.mkdir()
home = f'http://127.0.0.1:{server.server_port}/home'
(apps / (app_id + '.desktop')).write_text('[Desktop Entry]\nType=Application\nName=Link fixture\nIcon=folder\nX-Gnosis-Declarative=true\n'
                                          f'Exec="{a.gnosis.resolve()}" --application-mode "--profile={profile}" {home}\n')
report = Path(os.environ['GNOSIS_WEB_APP_LINK_REPORT'])

def reports():
    if not report.exists():
        return []
    return [(int(pid), app, int(windows), base64.b64decode(url).decode()) for pid, app, windows, url in
            (line.split('\t') for line in report.read_text().splitlines())]

def wait(predicate, description):
    end = time.monotonic() + 25
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.1)
    raise AssertionError(description + ': ' + repr(reports()))

def launch(*options, transient=True):
    command = [str(a.gnosis.resolve()), *options]
    if transient:
        subprocess.run(command, check=True, timeout=25)
    else:
        subprocess.Popen(command)

try:
    first = f'http://localhost:{server.server_port}/first?value=a%20b'
    second = f'http://localhost:{server.server_port}/second#anchor'
    launch(first)
    wait(lambda: any(r[1] == app_id and r[3] == first for r in reports()), 'cold link did not reach app')
    pid = next(r[0] for r in reports() if r[1] == app_id)
    assert all(r[1] == app_id for r in reports()), 'browser window opened for associated URI'
    launch('--application-mode', '--profile=' + str(profile), home)
    time.sleep(.5)
    assert [r for r in reports() if r[1] == app_id][-1][3] == first, 'launcher reset existing app'
    launch(second)
    wait(lambda: any(r[1] == app_id and r[3] == second for r in reports()), 'running app ignored deep link')
    assert all(r[0] == pid and r[2] == 1 for r in reports() if r[1] == app_id), 'app duplicated window/process'
    launch(home, transient=False)
    wait(lambda: any(r[1] != app_id and r[3] == home for r in reports()), 'unmatched URI did not stay in browser')
    private = f'http://localhost:{server.server_port}/private'
    launch('--incognito-mode', private, transient=False)
    wait(lambda: any(r[1] != app_id and r[3] == private for r in reports()), 'private URI leaked to app')
    assert [r for r in reports() if r[1] == app_id][-1][3] == second, 'normal app navigated for private URL'
    current = second
    if a.legacy_gnosis:
        current = f'http://localhost:{server.server_port}/old-launcher'
        subprocess.run([str(a.legacy_gnosis.resolve()), current], check=True, timeout=25)
        wait(lambda: any(r[1] == app_id and r[3] == current for r in reports()), 'updated receiver did not route old launcher URL')
        assert [r for r in reports() if r[1] != app_id and r[3] != private][-1][3] == home, 'forwarded link navigated regular browser'
    (profile / 'web-app.ini').write_text('[Extensions]\nenabled=\n[Links]\nenabled=false\ndomains=localhost;\n')
    launch(first)
    wait(lambda: any(r[1] != app_id and r[3] == first for r in reports()), 'disabled association did not fall back')
    (profile / 'web-app.ini').write_text('[Extensions]\nenabled=\n[Links]\nenabled=true\ndomains=localhost;\n')
    duplicate = root / (app_id + '2')
    duplicate.mkdir()
    (duplicate / '.app').touch()
    (duplicate / 'web-app.ini').write_bytes((profile / 'web-app.ini').read_bytes())
    (apps / (app_id + '2.desktop')).write_bytes((apps / (app_id + '.desktop')).read_bytes())
    launch(second)
    wait(lambda: any(r[1] != app_id and r[3] == second for r in reports()), 'ambiguous associations picked an arbitrary app')
    assert [r for r in reports() if r[1] == app_id][-1][3] == current, 'disabled/ambiguous link changed app'

    print('WEB_APP_LINKS_GTK_PASS cold app, same-window deep link, untouched launcher activation, fallback, private isolation, disabled/ambiguous associations', flush=True)
finally:
    for name in {r[1] for r in reports()}:
        subprocess.run(['gapplication', 'action', name, 'quit'], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
    server.shutdown()
