#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real GTK/Brave shared-session boundary tests on a private Xvfb display.

Run in the project's development environment. --build is the Meson build used by
--browser; its compilation database supplies the preload's exact native headers.
The fixture includes the private Session implementation only in the test library.
No production hooks, human profiles, display input, or external server are used.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import resource
import runpy
import shlex
import signal
import shutil
import subprocess
import tempfile
import time
from aiohttp import web

HERE = Path(__file__).resolve().parent


def compile_probe(build, output, rrweb, library):
    entries = json.loads((build / 'compile_commands.json').read_text())
    entry = next(e for e in entries if e['file'].endswith('/gnosis-shared.cpp'))
    flags = shlex.split(entry['command'])
    flags = flags[:flags.index('-MD')]
    flags = [v for v in flags if not v.startswith('-DGNOSIS_RRWEB_FILE=')]
    subprocess.run(flags + ['-shared', f'-DGNOSIS_RRWEB_FILE="{rrweb}"',
                            str(HERE / 'shared-session-native-probe.cpp'), '-Wl,--no-as-needed',
                            str(library), f'-Wl,-rpath,{library.parent}',
                            '-o', str(output)],
                   cwd=entry['directory'], check=True)


class Browser:
    def __init__(self, directory, process):
        self.directory, self.process = directory, process
        self.lock = asyncio.Lock()

    async def wait_ready(self):
        def ready():
            assert self.process.poll() is None, f'Browser exited {self.process.returncode} at startup; see logs'
            return (self.directory / 'ready').exists()
        await until(ready, 'native probe startup', timeout=45)

    async def rpc(self, op, **args):
        async with self.lock:
            request, response = self.directory / 'command.json', self.directory / 'reply.json'
            response.unlink(missing_ok=True)
            temporary = self.directory / 'command.tmp'
            temporary.write_text(json.dumps(dict(op=op, **args)))
            temporary.rename(request)
            deadline = time.monotonic() + 12
            while not response.exists():
                if self.process.poll() is not None:
                    raise AssertionError(f'Browser exited {self.process.returncode}; see logs')
                assert time.monotonic() < deadline, f'Native command timed out: {op}'
                await asyncio.sleep(.015)
            value = json.loads(response.read_text())
            assert not isinstance(value, dict) or 'error' not in value, value
            return value

    async def evaluate(self, expression):
        result = await self.rpc('eval', expression=expression)
        assert 'exceptionDetails' not in result, result
        return result.get('result', {}).get('value')

    async def cdp(self, method, **params):
        return await self.rpc('cdp', method=method, params=json.dumps(params))


async def until(predicate, description, timeout=15):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        value = predicate()
        last = await value if hasattr(value, '__await__') else value
        if last:
            return last
        await asyncio.sleep(.05)
    raise AssertionError(f'Timeout waiting for {description}; last={last!r}')


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--rrweb', type=Path, required=True)
    parser.add_argument('--xvfb', default='Xvfb')
    parser.add_argument('--p2p', action='store_true', help='Also require local direct P2P negotiation; external STUN is disabled')
    parser.add_argument('--log-dir', type=Path, default=Path('/tmp/gnosis-shared-native-results'))
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    args.log_dir.mkdir(parents=True, exist_ok=True)
    asset_requests, origin_requests = [], []
    async def fixture(request):
        origin_requests.append(request.path)
        if request.path == '/private.svg':
            asset_requests.append(request.headers.get('Cookie', ''))
            assert request.cookies.get('fixture') == 'host', 'Guest reached the authenticated origin'
            return web.Response(body=b'<svg xmlns="http://www.w3.org/2000/svg" width="48" height="32"><path fill="red" d="M0 0h48v32H0z"/></svg>', content_type='image/svg+xml', headers={'Cache-Control': 'private,max-age=3600'})
        html = '''<!doctype html><title>Native sharing fixture</title><style>body{margin:0;font:20px sans-serif;height:2400px}button{width:200px;height:80px}img{width:48px;height:32px}</style>
<h1 id="secret">host-only-secret</h1><button id="target">Clicks: 0</button><img id="asset" src="/private.svg"><p id="mutation">initial</p>
<script>window.fixtureRuns=1;window.clicks=0;target.onclick=e=>{if(e.isTrusted)target.textContent='Clicks: '+(++clicks)};</script>'''
        response = web.Response(text=html, content_type='text/html')
        response.set_cookie('fixture', 'host', httponly=True)
        return response
    site_app = web.Application()
    site_app.router.add_get('/{path:.*}', fixture)
    site_runner = web.AppRunner(site_app, access_log=None)
    await site_runner.setup()
    site = web.TCPSite(site_runner, '127.0.0.1', 0)
    await site.start()
    origin = f'http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}'
    relay_runner = web.AppRunner(runpy.run_path(str(args.server))['create_app'](), access_log=None)
    await relay_runner.setup()
    relay = web.TCPSite(relay_runner, '127.0.0.1', 0)
    await relay.start()
    relay_url = f'ws://127.0.0.1:{relay._server.sockets[0].getsockname()[1]}'
    processes, logs = [], []
    display = None
    with tempfile.TemporaryDirectory(prefix='gnosis-shared-native-', dir='/tmp') as temporary:
        root = Path(temporary)
        probe = root / 'probe.so'
        library = Path(args.browser).resolve().parent.parent / 'lib/gnosis/libephymain.so'
        compile_probe(args.build.resolve(), probe, args.rrweb.resolve(), library.resolve())
        shutil.copyfile(probe, args.log_dir / "probe.so")
        try:
            display = subprocess.Popen([args.xvfb, '-displayfd', '1', '-screen', '0', '1600x1200x24', '-nolisten', 'tcp'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            number = display.stdout.readline().strip()
            assert number.isdigit(), 'Private Xvfb startup failed'
            def launch(role, uri):
                private = root / role
                private.mkdir()
                env = dict(os.environ, DISPLAY=f':{number}', GDK_BACKEND='x11', GSK_RENDERER='cairo', GTK_A11Y='none', GSETTINGS_BACKEND='memory', GNOSIS_SHARE_DISABLE_P2P='1', GNOSIS_SHARED_NATIVE_DIRECTORY=str(private), LANG='C.UTF-8')
                for key, name in [('HOME', 'home'), ('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'), ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')]:
                    path = private / name
                    path.mkdir(mode=0o700)
                    env[key] = str(path)
                if args.p2p:
                    env.pop('GNOSIS_SHARE_DISABLE_P2P', None)
                    env['GNOSIS_SHARE_STUN_SERVER'] = ''
                for key in ('WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'SESSION_MANAGER'):
                    env.pop(key, None)
                log = open(args.log_dir / f'{role}.log', 'w')
                logs.append(log)
                process = subprocess.Popen(['dbus-run-session', '--', 'env', f'LD_PRELOAD={probe}', args.browser, f'--profile={private / "profile"}', uri], env=env, stdout=log, stderr=log, start_new_session=True)
                processes.append(process)
                return Browser(private, process)
            host = launch('host', origin + '/host')
            guest = launch('guest', 'about:blank')
            await asyncio.gather(host.wait_ready(), guest.wait_ready())
            await until(lambda: host.evaluate('document.querySelector("#asset")?.naturalWidth===48'), 'host fixture loaded')
            feedback = await host.rpc('host', server=relay_url)
            assert feedback['visible'] and feedback['feedbackMs'] < 400, feedback
            print(f'SHARED_NATIVE_PASS Start Sharing painted feedback {feedback["feedbackMs"]:.1f}ms', flush=True)
            async def host_ready():
                value = await host.rpc('state')
                return value if value and value['ready'] else None
            state = await until(host_ready, 'host rendezvous')
            invitation = state['invite']
            await host.rpc('present')
            await host.rpc('close-dialog')
            await asyncio.sleep(.3)
            feedback = await guest.rpc('join', invite=invitation)
            assert feedback['visible'] and feedback['feedbackMs'] < 400, feedback
            print(f'SHARED_NATIVE_PASS Join Session painted feedback {feedback["feedbackMs"]:.1f}ms', flush=True)
            async def pending():
                state = await host.rpc('state')
                return state['members'] if state and state['members'] else None
            members = await until(pending, 'pending host approval')
            peer = members[0]['id']
            async def viewer_ready():
                state = await guest.rpc('state')
                return state and state['viewerReady']
            await until(viewer_ready, 'trusted guest viewer')
            await guest.cdp('Network.setCookie', name='fixture', value='guest', url=origin, httpOnly=True)
            await asyncio.sleep(.4)
            assert not await guest.evaluate('!!document.querySelector("#stage iframe")'), 'Content arrived before host approval'
            assert not members[0]['approved']
            print('SHARED_NATIVE_PASS host approval gates content', flush=True)
            await host.rpc('present')
            await host.rpc('close-dialog')
            await asyncio.sleep(.3)
            await host.rpc('approve', peer=peer)
            replay_doc = 'document.querySelector("#stage iframe")?.contentDocument'
            async def replay_ready():
                return await guest.evaluate(f'!!({replay_doc})?.querySelector("#secret")')
            await until(replay_ready, 'actual rrweb DOM replay')
            if args.p2p:
                async def direct_ready():
                    state = await guest.rpc('state')
                    return state and state['direct']
                await until(direct_ready, 'direct P2P data channel')
                print('SHARED_NATIVE_PASS real direct P2P negotiation with responsive GTK', flush=True)
            assert await guest.evaluate(f'({replay_doc}).querySelector("#secret").textContent') == 'host-only-secret'
            assert await guest.evaluate(f'({replay_doc}).defaultView.fixtureRuns') is None, 'Mirrored site script executed'
            await host.evaluate('document.querySelector("#mutation").textContent="live mutation"')
            await until(lambda: guest.evaluate(f'({replay_doc})?.querySelector("#mutation")?.textContent==="live mutation"'), 'live DOM mutation')
            await until(lambda: guest.evaluate(f'({replay_doc})?.querySelector("#asset")?.naturalWidth===48'), 'host cached resource')
            assert await guest.evaluate(f'fetch({json.dumps(origin + "/not-cached")}).then(()=>false,()=>true)')
            assert '/not-cached' not in origin_requests, origin_requests
            assert asset_requests == ['fixture=host'], asset_requests
            print('SHARED_NATIVE_PASS live DOM, scripts inert, host cached authenticated resource; zero guest origin requests', flush=True)
            dimensions = await host.evaluate('({width:innerWidth,height:innerHeight,scroll:scrollY})')
            geometry = await guest.evaluate('(()=>{const s=document.querySelector("#stage"),r=s.getBoundingClientRect();return{width:s.style.width,height:s.style.height,x:r.x,y:r.y,w:r.width,h:r.height,outerW:innerWidth,outerH:innerHeight}})()')
            assert geometry['width'] == f'{dimensions["width"]}px' and geometry['height'] == f'{dimensions["height"]}px', (dimensions, geometry)
            assert geometry['x'] >= -1 and geometry['y'] >= -1 and geometry['w'] <= geometry['outerW']+1 and geometry['h'] <= geometry['outerH']+1, geometry
            assert geometry['x'] > 1 or geometry['y'] > 1, f'Expected actual letterbox: {geometry}'
            await guest.rpc('zoom', factor=2)
            await asyncio.sleep(.15)
            assert await host.evaluate('({width:innerWidth,height:innerHeight,scroll:scrollY})') == dimensions
            zoomed = await guest.evaluate('document.querySelector("#stage").getBoundingClientRect().width')
            assert zoomed > geometry['w'] * 1.5, (zoomed, geometry)
            print('SHARED_NATIVE_PASS host logical viewport, letterbox, guest zoom leaves host width/scroll unchanged', flush=True)
            await guest.rpc('zoom', factor=1)
            async def guest_click():
                return await guest.evaluate('''(()=>{const s=document.querySelector('#stage'),f=s.querySelector('iframe'),b=f.contentDocument.querySelector('#target').getBoundingClientRect(),m=new DOMMatrix(getComputedStyle(s).transform);const p=new DOMPoint(b.x+b.width/2,b.y+b.height/2).matrixTransform(m);for(const type of ['pointerdown','pointerup'])document.dispatchEvent(new PointerEvent(type,{clientX:p.x,clientY:p.y,button:0,bubbles:true}));return true})()''')
            await guest_click()
            await asyncio.sleep(.3)
            assert await host.evaluate('clicks') == 0, 'View-only input controlled the host'
            await host.rpc('control', peer=peer, enabled=True)
            await asyncio.sleep(.1)
            await guest_click()
            await until(lambda: host.evaluate('clicks===1'), 'approved control trusted click')
            await host.rpc('control', peer=peer, enabled=False)
            await guest_click()
            await asyncio.sleep(.25)
            assert await host.evaluate('clicks') == 1, 'Revoked input reached host'
            print('SHARED_NATIVE_PASS view-only, granted trusted control, revoked control', flush=True)
            feedback, _ = await asyncio.gather(host.rpc('resize-watch'), guest.rpc('resize-request', width=800, height=500))
            assert feedback['visible'] and feedback['feedbackMs'] < 400, feedback
            print(f'SHARED_NATIVE_PASS Resize prompt painted feedback {feedback["feedbackMs"]:.1f}ms', flush=True)
            assert await host.evaluate('innerWidth') == dimensions['width'], 'Resize happened before approval'
            await until(lambda: host.rpc('resize-reply', allow=False), 'native resize refusal prompt')
            assert await host.evaluate('innerWidth') == dimensions['width']
            await guest.rpc('resize-request', width=800, height=500)
            await until(lambda: host.rpc('resize-reply', allow=True), 'native resize approval prompt')
            await until(lambda: host.evaluate('innerWidth===800&&innerHeight===500'), 'approved logical host resize')
            print('SHARED_NATIVE_PASS host size unchanged before approval/after denial, approved resize applied', flush=True)
            for browser in (host, guest):
                heartbeat = await browser.rpc('state')
                assert heartbeat['heartbeatMaxMs'] < 400 and heartbeat['heartbeats'] > 10, heartbeat
                print(f'SHARED_NATIVE_PASS pending-work GTK heartbeat max {heartbeat["heartbeatMaxMs"]:.1f}ms', flush=True)
            start = time.monotonic()
            feedback = await guest.rpc('stop')
            assert feedback['visible'] and feedback['feedbackMs'] < 400, feedback
            assert await guest.rpc('state') is None
            elapsed = (time.monotonic()-start)*1000
            assert elapsed < 400, elapsed
            print(f'SHARED_NATIVE_PASS Stop Viewing visible feedback {elapsed:.1f}ms', flush=True)
            await until(lambda: no_members(host), 'host removes disconnected viewer')
            feedback = await guest.rpc('join', invite=invitation)
            assert feedback['visible'] and feedback['feedbackMs'] < 400, feedback
            print(f'SHARED_NATIVE_PASS Join Session painted feedback {feedback["feedbackMs"]:.1f}ms', flush=True)
            members = await until(pending, 'second pending viewer')
            await host.rpc('approve', peer=members[0]['id'])
            await until(viewer_ready, 'second viewer ready')
            await until(replay_ready, 'second DOM replay')
            start = time.monotonic()
            feedback = await host.rpc('stop')
            assert feedback['visible'] and feedback['feedbackMs'] < 400, feedback
            assert await host.rpc('state') is None
            elapsed = (time.monotonic()-start)*1000
            assert elapsed < 400, elapsed
            async def guest_stopped():
                value = await guest.rpc('state')
                return value is None or value['stopped']
            await until(guest_stopped, 'host ends remote viewing', timeout=2)
            print(f'SHARED_NATIVE_PASS Stop Sharing visible feedback {elapsed:.1f}ms; remote viewing ended', flush=True)
            assert asset_requests == ['fixture=host'], asset_requests
            print('SHARED_NATIVE_ALL_PASS', flush=True)
        finally:
            for process in processes:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            for process in processes:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)
            for log in logs:
                log.close()
            if display:
                display.terminate()
                display.wait(timeout=5)
            await site_runner.cleanup()
            await relay_runner.cleanup()


async def no_members(browser):
    state = await browser.rpc('state')
    return state and not state['members']


if __name__ == '__main__':
    asyncio.run(main())
