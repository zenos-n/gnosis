#!/usr/bin/env python3
"""Exercise actual bookmarks widgets/animation on an isolated display and profile."""
import argparse
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
p.add_argument('--log', type=Path, default=Path('/tmp/gnosis-bookmarks.log'))
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='gnosis-bookmarks-') as directory, a.log.open('w') as log:
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
                                 '--profile=' + str(root / 'profile'), 'about:blank'],
                                env=env, stdout=log, stderr=log, start_new_session=True)
        result = proc.wait(timeout=65)
    finally:
        if proc:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            proc.wait(timeout=5)
        display.terminate()
        display.wait(timeout=5)
if result or 'BOOKMARKS_GTK_PASS' not in a.log.read_text():
    raise SystemExit(f'Bookmarks native regression failed; see {a.log}')
print('BOOKMARKS_GTK_PASS: native navigation/animation, row content, search and editing')
