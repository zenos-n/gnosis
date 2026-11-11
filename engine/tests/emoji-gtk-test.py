#!/usr/bin/env python3
"""Exercise native emoji chooser and browser editing on an isolated display and profile."""
import argparse
import http.server
import threading
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--gnosis', required=True, type=Path)
p.add_argument('--probe', required=True, type=Path)
p.add_argument('--xvfb', required=True, type=Path)
p.add_argument('--capture-tool', type=Path, help='Optional ImageMagick import for whole-window screenshots')
p.add_argument('--log', type=Path, default=Path('/tmp/gnosis-emoji.log'))
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'<!doctype html><title>Emoji input fixture</title>\n<style>body{margin:24px 40px}body>input,textarea,#editable,#shadow,iframe{display:block;margin:0 0 20px}#input{width:320px;height:30px}#editable{width:400px;border:1px solid;padding:6px}iframe{width:310px;height:60px}</style><input id=input value="hello world"><textarea id=textarea>a\nb</textarea><div id=editable contenteditable>hello</div>\n<input id=readonly readonly><div id=shadow></div>\n<iframe srcdoc="<input>"></iframe>\n<script>shadow.attachShadow({mode:\'open\'}).innerHTML=\'<input>\';</script>'
        if self.path == '/frame':
            body = b'<!doctype html><input><script>addEventListener("message", () => { const i=document.querySelector("input"); i.focus(); const r=i.getBoundingClientRect(); parent.postMessage({bounds:[r.x,r.y,r.width,r.height]}, "*"); }); document.querySelector("input").addEventListener("input", event => parent.postMessage({emoji:event.target.value}, "*"));</script>'
        else:
            body += f'<iframe id=cross src="http://localhost:{self.server.server_port}/frame"></iframe><script>addEventListener("message",e=>{{if(e.data.emoji!==undefined)window.crossValue=e.data.emoji;if(e.data.bounds)window.crossBounds=e.data.bounds}})</script>'.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_): pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
with tempfile.TemporaryDirectory(prefix='gnosis-emoji-') as directory, a.log.open('w') as log:
    root = Path(directory)
    config = root / 'bus.conf'
    config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>'
                      '<policy context="default"><allow send_destination="*"/><allow receive_sender="*"/>'
                      '<allow own="*"/></policy></busconfig>')
    display = subprocess.Popen([str(a.xvfb), '-displayfd', '1', '-screen', '0', '1100x800x24', '-nolisten', 'tcp'],
                               stdout=subprocess.PIPE, stderr=log, text=True)
    proc = None
    try:
        number = display.stdout.readline().strip()
        assert number.isdigit(), 'No isolated display'
        env = dict(os.environ, DISPLAY=':' + number, GDK_BACKEND='x11', GSK_RENDERER='gl', GTK_A11Y='none',
                   GSETTINGS_BACKEND='memory', XDG_DATA_HOME=str(root / 'data'), XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_CACHE_HOME=str(root / 'cache'), LD_PRELOAD=str(a.probe.resolve()), LC_ALL='C.UTF-8')
        if a.capture_tool:
            env['GNOSIS_CAPTURE_TOOL'] = str(a.capture_tool.absolute())
        env.pop('WAYLAND_DISPLAY', None)
        env.pop('DBUS_SESSION_BUS_ADDRESS', None)
        proc = subprocess.Popen(['dbus-run-session', '--config-file=' + str(config), '--', str(a.gnosis.resolve()),
                                 '--profile=' + str(root / 'profile'), f'http://127.0.0.1:{server.server_port}/'],
                                env=env, stdout=log, stderr=log, start_new_session=True)
        result = proc.wait(timeout=100)
    finally:
        if proc:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            proc.wait(timeout=5)
        display.terminate()
        display.wait(timeout=5)
server.shutdown()
server.server_close()
if result or 'EMOJI_GTK_PASS' not in a.log.read_text():
    raise SystemExit(f'Emoji native regression failed; see {a.log}')
print('EMOJI_GTK_PASS: native shortcut, chooser, Unicode selection, undo and cancellation')
