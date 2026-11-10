#!/usr/bin/env python3
"""Check web-app links with real middle/right clicks on an isolated display."""
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
p.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
p.add_argument('--log', type=Path, default=Path('/tmp/gnosis-web-app-external.log'))
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

if not a.inside:
    with tempfile.TemporaryDirectory(prefix='gnosis-app-external-') as directory, a.log.open('w') as log:
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
                       XDG_CACHE_HOME=str(root / 'cache'), GNOSIS_WEB_APP_EXTERNAL_REPORT=str(root / 'report'),
                       LD_PRELOAD=str(a.probe.resolve()), LC_ALL='C.UTF-8')
            env.pop('WAYLAND_DISPLAY', None)
            env.pop('DBUS_SESSION_BUS_ADDRESS', None)
            process = subprocess.Popen(['dbus-run-session', '--config-file=' + str(config), '--', sys.executable,
                                        str(Path(__file__).resolve()), *sys.argv[1:], '--inside'], env=env, stdout=log, stderr=log, start_new_session=True)
            result = process.wait(timeout=160)
        finally:
            if process:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                process.wait(timeout=5)
            display.terminate()
            display.wait(timeout=5)
    if result or 'WEB_APP_EXTERNAL_GTK_PASS' not in a.log.read_text():
        raise SystemExit(f'Web app link test failed; see {a.log}')
    print('WEB_APP_EXTERNAL_GTK_PASS: middle-click and native menu, cold/warm browser and app site boundaries')
    raise SystemExit(0)

class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        port = self.server.server_port
        body = (f'<!doctype html><title>Web-app external links</title>'
                f'<p><a id=external href="http://localhost:{port}/external">Outside the app</a></p>'
                f'<p><a id=internal href="http://127.0.0.1:{port}/internal">Inside the app</a></p>'
                f'<p><a id=alias href="http://127.0.0.2:{port}/alias">Configured app site</a></p>').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_): pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
alias_server = http.server.ThreadingHTTPServer(('127.0.0.2', server.server_port), Fixture)
for instance in (server, alias_server):
    threading.Thread(target=instance.serve_forever, daemon=True).start()
root = Path(os.environ['XDG_DATA_HOME'])
app_id = 'com.neg-zero.Gnosis.WebApp_externalfixture'
apps = root / 'applications'
apps.mkdir(parents=True)
port = server.server_port
home = f'http://127.0.0.1:{port}/home'
external = f'http://localhost:{port}/external'
version = Path(__file__).resolve().parents[2] / 'lib/ephy-profile-utils.h'
import re
migration = re.search(r'EPHY_PROFILE_MIGRATION_VERSION (\d+)', version.read_text())[1]
for name, url, domains in [('externalfixture', home, '127.0.0.2'),
                           ('otherfixture', external, 'localhost')]:
    profile = root / ('com.neg-zero.Gnosis.WebApp_' + name)
    profile.mkdir()
    (profile / '.app').touch()
    (profile / '.migrated').write_text(migration)
    (profile / 'web-app.ini').write_text(f'[Extensions]\nenabled=\n[Links]\nenabled=true\ndomains={domains};\n')
    (apps / (profile.name + '.desktop')).write_text('[Desktop Entry]\nType=Application\nName=External link fixture\nIcon=folder\nX-Gnosis-Declarative=true\n'
        f'Exec="{a.gnosis.resolve()}" --application-mode "--profile={profile}" {url}\n')
report = Path(os.environ['GNOSIS_WEB_APP_EXTERNAL_REPORT'])
command = root / 'command'
os.environ['GNOSIS_WEB_APP_EXTERNAL_COMMAND'] = str(command)

def reports():
    if not report.exists(): return []
    return [(int(pid), app, int(count), window, base64.b64decode(url).decode(), int(tabs))
            for pid, app, count, window, url, tabs in (line.split('\t') for line in report.read_text().splitlines())]

def wait(predicate, reason):
    end = time.monotonic() + 30
    while time.monotonic() < end:
        if predicate(): return
        time.sleep(.1)
    raise AssertionError(reason + ': ' + repr(reports()))

def browser(): return [r for r in reports() if r[1] == 'com.neg-zero.Gnosis']
def app(): return [r for r in reports() if r[1] == app_id]
def click(name): command.write_text(name)

try:
    subprocess.Popen([str(a.gnosis.resolve()), '--application-mode',
                      '--profile=' + str(root / app_id), home])
    wait(lambda: any(r[4] == home for r in app()), 'source app did not load')
    pid = app()[0][0]
    click('middle-external')
    wait(lambda: any(r[4] == external for r in browser()), 'middle-click failed cold browser handoff')
    assert all(r[2] == 1 and r[4] == home for r in app()), 'external middle-click changed app'
    assert not any(r[1].endswith('_otherfixture') for r in reports()), 'explicit browser intent was rerouted to another web app'
    click('window-external')
    wait(lambda: any(r[2] == 2 and r[4] == external for r in browser()), 'context New Window failed running browser handoff')
    assert all(r[2] == 1 for r in app()), 'context New Window created app window'
    click('tab-external')
    wait(lambda: any(r[5] == 2 for r in browser()), 'context New Tab did not reach browser')
    assert all(r[2] <= 2 for r in browser()), 'context New Tab created another browser window'
    assert all(r[2] == 1 for r in app()), 'context New Tab created app window'
    click('middle-internal')
    internal = f'http://127.0.0.1:{port}/internal'
    wait(lambda: any(r[4] == internal for r in app()), 'in-site link escaped app')
    click('middle-alias')
    alias = f'http://127.0.0.2:{port}/alias'
    wait(lambda: any(r[4] == alias for r in app()), 'configured site escaped app')
    assert all(r[0] == pid for r in app()), 'internal link changed app process'
    assert not any(r[4] in (internal, alias) for r in browser()), 'internal site leaked to browser'
    assert not any(r[1].endswith('_otherfixture') for r in reports()), 'normal-browser intent lost on remote activation'
    print('WEB_APP_EXTERNAL_GTK_PASS real middle/right clicks, cold/warm browser, explicit browser intent, same-site/configured-site retention', flush=True)
finally:
    for name in {r[1] for r in reports()}:
        subprocess.run(['gapplication', 'action', name, 'quit'], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
    for instance in (server, alias_server): instance.shutdown(); instance.server_close()
