#!/usr/bin/env python3
"""Check nested HDR negotiation on a private Mutter display with simulated output capabilities."""
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
p.add_argument('--ffmpeg', required=True, type=Path)
p.add_argument('--mutter', required=True, type=Path)
p.add_argument('--log', type=Path, default=Path('/tmp/gnosis-hdr.log'))
p.add_argument('--scale', type=float, default=1, choices=(1, 5/3), help='Private virtual display scale')
p.add_argument('--real-feedback', action='store_true', help='Check native120Hz cadence without synthetic refresh injection')
p.add_argument('--crop-probe', type=Path, help='Optional native GTK fractional crop regression executable')
p.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
if not a.inside:
    with tempfile.TemporaryDirectory(prefix='gnosis-hdr-') as directory, a.log.open('w') as log:
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
                   GDK_BACKEND='wayland', GDK_DEBUG='offload', GTK_A11Y='none', WAYLAND_DISPLAY='hdr-test')
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
    output = a.log.read_text()
    if result or 'HDR_GTK_PASS' not in output:
        raise SystemExit(f'HDR regression failed; see {a.log}')
    phases = output.split('HDR_GTK_WINDOWED_PASS', 1)
    assert len(phases) == 2, 'Missing windowed HDR pixels'
    assert any('Attaching' in line and 'rec2100-pq' in line for line in phases[0].splitlines()), 'Windowed HDR never attached'
    phases = phases[1].split('HDR_GTK_FULLSCREEN_PASS', 1)
    assert len(phases) == 2 and any('Attaching' in line and 'rec2100-pq' in line for line in phases[0].splitlines()), 'Fullscreen HDR never attached'
    print(f'HDR_GTK_PASS scale={a.scale}: SDR/HDR transitions, image/video highlights, windowed/fullscreen PQ attachments and presentation feedback')
    raise SystemExit(0)

compositor = subprocess.Popen([str(a.mutter), '--headless', '--wayland', '--no-x11',
                               '--virtual-monitor', '3840x2160@120', '--wayland-display', 'hdr-test'])
# These bytes are generated test media, never a human file or profile.
import struct, zlib
fixture_dir = Path(os.environ['XDG_RUNTIME_DIR']).parent / 'fixtures'
assert Path(os.environ['HOME']) == fixture_dir.parent / 'home'
assert fixture_dir.parent.name.startswith('gnosis-hdr-')
fixture_dir.mkdir()
def chunk(kind, data):
    return struct.pack('!I',len(data))+kind+data+struct.pack('!I',zlib.crc32(kind+data))
def pq(nits):
    value=(nits/10000)**(2610/16384)
    return round(((3424/4096+2413/128*value)/(1+2392/128*value))**(2523/32)*65535)
def png(row):
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',64,64,16,2,0,0,0))+chunk(b'cICP',bytes([9,16,0,1]))+chunk(b'IDAT',zlib.compress((b'\x00'+row)*64))+chunk(b'IEND',b'')
image=png(b''.join(struct.pack('!HHH',*[pq(203 if x < 32 else 1000)]*3) for x in range(64)))
source = fixture_dir / 'source.png'
source.write_bytes(png(struct.pack('!HHH',pq(1000),pq(203),0)*64))
video_path = fixture_dir / 'hdr.webm'
subprocess.run([str(a.ffmpeg),'-v','error','-threads','1','-filter_threads','1','-loop','1','-i',str(source),'-t','2','-r','15',
                '-vf','scale=1920:1080:out_color_matrix=bt2020:out_range=tv','-c:v','libvpx-vp9','-profile:v','2',
                '-pix_fmt','yuv420p10le','-color_primaries','bt2020','-color_trc','smpte2084','-colorspace','bt2020nc',
                '-deadline','realtime','-cpu-used','8','-threads','1','-y',str(video_path)],check=True,timeout=20)
video=video_path.read_bytes()
class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/hdr.png':
            body, mime = image, 'image/png'
        elif self.path == '/hdr.webm':
            body, mime = video, 'video/webm'
        else:
            body=b'<!doctype html><title>HDR fixture</title><style>body{margin:0;background:linear-gradient(to right,#000 0% 20%,#404040 20% 40%,#808080 40% 60%,#c0c0c0 60% 80%,#fff 80% 100%);color:#ddd}</style><h1>SDR document</h1><div id=anim style="position:fixed;bottom:0;width:10px;height:10px;background:red"></div><script>let i=0;window.frameDeltas=[];let last;function tick(t){if(last)frameDeltas.push(t-last);last=t;if(frameDeltas.length>120)frameDeltas.shift();const a=document.getElementById("anim");if(a)a.style.transform="translateX("+(i++%100)+"px)";requestAnimationFrame(tick)}requestAnimationFrame(tick)</script>'
            mime='text/html; charset=utf-8'
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_): pass
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    for _ in range(100):
        assert compositor.poll() is None, 'Private Mutter exited'
        if (Path(os.environ['XDG_RUNTIME_DIR']) / 'hdr-test').is_socket():
            break
        time.sleep(.1)
    else:
        raise AssertionError('Private Wayland display unavailable')
    if a.crop_probe:
        subprocess.run([str(a.crop_probe.resolve())], env=dict(os.environ, GDK_DEBUG='offload,force-offload', G_DEBUG='fatal-criticals'), check=True, timeout=10)
    env = dict(os.environ, LD_PRELOAD=str(a.probe.resolve()), GNOSIS_HDR_TEST='1', GNOSIS_HDR_TEST_SCALE=str(a.scale))
    if a.real_feedback: env['GNOSIS_HDR_REAL_FEEDBACK'] = '1'
    subprocess.run([str(a.gnosis.resolve()), '--incognito-mode',
                    '--profile=' + str(Path(os.environ['XDG_DATA_HOME']) / 'profile'),
                    f'http://127.0.0.1:{server.server_port}/fractional.html'], env=env, check=True, timeout=110)
finally:
    server.shutdown()
    compositor.terminate()
    compositor.wait(timeout=5)
