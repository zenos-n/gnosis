'use strict';
// Optional real rrweb/Chromium check; the ordinary Node tests need no browser/dependencies.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const {spawn} = require('node:child_process');
const {once} = require('node:events');

test('real rrweb preserves the host DOM in a script-disabled replay', {
  skip: !process.env.GNOSIS_BRAVE_BIN || !process.env.GNOSIS_RRWEB_JS,
  timeout: 30000,
}, async () => {
  const root = path.join(__dirname, '../../src');
  const routes = {
    '/': [path.join(__dirname, 'shared-page-fixture.html'), 'text/html'],
    '/rrweb.js': [process.env.GNOSIS_RRWEB_JS, 'text/javascript'],
    '/record.js': [path.join(root, 'gnosis-share-record.js'), 'text/javascript'],
    '/geometry.js': [path.join(root, 'gnosis-share-geometry.js'), 'text/javascript'],
    '/view.js': [path.join(root, 'gnosis-share-view.js'), 'text/javascript'],
  };
  const server = http.createServer((request, response) => {
    const route = routes[new URL(request.url, 'http://localhost').pathname];
    if (!route) { response.writeHead(404).end(); return; }
    response.writeHead(200, {'Content-Type': route[1], 'Cache-Control': 'no-store'});
    response.end(fs.readFileSync(route[0]));
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'gnosis-shared-page-'));
  let child;
  let socket;
  try {
    child = spawn(process.env.GNOSIS_BRAVE_BIN, ['--headless=new', '--remote-debugging-port=0',
      '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--disable-default-apps',
      '--disable-background-networking', '--user-data-dir=' + path.join(temporary, 'profile'), 'about:blank'],
    {env: {...process.env, HOME: temporary}, detached: true, stdio: ['ignore', 'ignore', 'pipe']});
    let stderr = '';
    child.stderr.on('data', data => { stderr += data; });
    const waitFor = async predicate => {
      const until = Date.now() + 15000;
      while (!predicate()) {
        if (child.exitCode !== null) throw new Error('Headless browser exited: ' + stderr.slice(-3000));
        if (Date.now() > until) throw new Error('Headless browser timed out: ' + stderr.slice(-1000));
        await new Promise(resolve => setTimeout(resolve, 25));
      }
    };
    const portFile = path.join(temporary, 'profile/DevToolsActivePort');
    await waitFor(() => fs.existsSync(portFile));
    const [port] = fs.readFileSync(portFile, 'utf8').trim().split('\n');
    const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    socket = new WebSocket(pages.find(page => page.type === 'page').webSocketDebuggerUrl);
    await once(socket, 'open');
    let nextId = 0;
    const pending = new Map();
    socket.addEventListener('message', event => {
      const message = JSON.parse(event.data);
      if (message.id && pending.has(message.id)) {
        const {resolve, reject} = pending.get(message.id); pending.delete(message.id);
        if (message.error) reject(new Error(JSON.stringify(message.error))); else resolve(message.result);
      }
    });
    const call = (method, params) => new Promise((resolve, reject) => {
      const id = ++nextId; pending.set(id, {resolve, reject}); socket.send(JSON.stringify({id, method, params}));
    });
    await call('Page.navigate', {url: `http://127.0.0.1:${server.address().port}/`});
    let result;
    // Poll through CDP rather than interacting with the human desktop.
    for (let attempt = 0; attempt < 400; attempt++) {
      result = await call('Runtime.evaluate', {expression: 'document.getElementById("result")?.textContent', returnByValue: true});
      if (/^(PASS|FAIL):/.test(result.result.value || '')) break;
      await new Promise(resolve => setTimeout(resolve, 25));
    }
    assert.match(result.result.value || '', /^PASS:/);
  } finally {
    socket?.close();
    if (child) {
      // Nix's launcher may exit before Brave; terminate only our isolated process group.
      try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
      if (child.exitCode === null) await once(child, 'exit');
    }
    child?.stderr.destroy();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
    fs.rmSync(temporary, {recursive: true, force: true});
  }
});
