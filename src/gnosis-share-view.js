(() => {
  'use strict';
  const geometry = globalThis.GnosisShareGeometry;
  const stage = document.getElementById('stage');
  const cursors = document.getElementById('cursors');
  if (!stage || !cursors || !geometry || !globalThis.Gnosis || !globalThis.rrweb)
    throw new Error('Shared page viewer is unavailable');

  const style = document.createElement('style');
  style.textContent = `
    html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#000}
    #stage{position:fixed;left:0;top:0;transform-origin:0 0;pointer-events:none;overflow:hidden}
    #stage .replayer-wrapper{position:absolute;inset:0;z-index:0}
    #stage iframe{border:0;pointer-events:none}
    #stage .replayer-mouse,#stage .replayer-mouse-tail{display:none!important}
    #cursors{position:fixed;inset:0;overflow:hidden;pointer-events:none;z-index:2}
    .gnosis-shared-cursor{position:absolute;left:0;top:0;white-space:nowrap;font:12px sans-serif}
    .gnosis-shared-cursor svg{display:block;width:22px;height:26px;overflow:visible;filter:drop-shadow(0 1px 1px #0008)}
    .gnosis-shared-cursor span{display:block;margin:0 0 0 10px;border-radius:4px;padding:3px 6px;color:#fff}
    .gnosis-shared-opaque{position:absolute;inset:0;overflow:hidden;z-index:1;pointer-events:none}
  `;
  document.head.appendChild(style);
  const opaqueLayer = document.createElement('div');
  opaqueLayer.className = 'gnosis-shared-opaque';
  stage.appendChild(opaqueLayer);
  const input = document.createElement('textarea');
  input.setAttribute('aria-label', 'Type in the shared page');
  input.setAttribute('autocomplete', 'off');
  input.setAttribute('autocapitalize', 'off');
  input.spellcheck = false;
  Object.assign(input.style, {position: 'fixed', width: '1px', height: '1px',
    opacity: '0', padding: '0', border: '0', resize: 'none', pointerEvents: 'none'});
  document.body.appendChild(input);

  const palette = ['#3584e4', '#9141ac', '#c64600', '#26a269', '#c01c28', '#613583'];
  const peers = new Map();
  const removers = [];
  let ended = false;
  let host = {width: 1, height: 1};
  let zoom = 1;
  let pan = {x: 0, y: 0};
  let transform;
  let replayer;
  let meta;
  let timeOffset = 0;
  let lastTimestamp = 0;
  let dragging;
  const pressed = new Map();
  let composing = false;
  let lastComposition = null;
  let compositionTimer;
  let lastPointer = 0;
  const send = payload => { if (!ended) Gnosis.postMessage('shared', payload); };
  function on(target, type, callback, options) {
    target.addEventListener(type, callback, options);
    removers.push(() => target.removeEventListener(type, callback, options));
  }
  function layout() {
    transform = geometry.transform(host, {width: innerWidth, height: innerHeight}, zoom, pan);
    if (!transform) return;
    pan = {x: transform.panX, y: transform.panY};
    stage.style.width = `${host.width}px`;
    stage.style.height = `${host.height}px`;
    stage.style.transform = `translate(${transform.left}px,${transform.top}px) scale(${transform.scale})`;
    for (const peer of peers.values()) positionCursor(peer);
  }
  function positionCursor(peer) {
    const point = geometry.toGuest(transform, peer.x, peer.y);
    if (point) peer.element.style.transform = `translate(${point.x}px,${point.y}px)`;
  }
  function setZoom(factor, x = innerWidth / 2, y = innerHeight / 2) {
    const next = geometry.zoomAt(transform, factor, x, y);
    if (!next) return;
    zoom = next.zoom;
    pan = {x: next.panX, y: next.panY};
    layout();
    send({kind: 'zoom-changed', factor: zoom});
  }
  function clearPlayer() {
    replayer?.destroy();
    replayer = null;
    opaqueLayer.replaceChildren();
  }
  function receiveEvent(event) {
    if (!event || !Number.isFinite(event.timestamp)) return;
    if (event.type === 4) {
      meta = event;
      return;
    }
    if (event.type === 2) {
      clearPlayer();
      // Keep the host clock offset out of rrweb's live scheduling.
      timeOffset = Date.now() - 80 - event.timestamp;
      lastTimestamp = event.timestamp + timeOffset;
      const baseline = meta || {type: 4, timestamp: event.timestamp,
        data: {width: host.width, height: host.height, href: ''}};
      replayer = new rrweb.Replayer([
        {...baseline, timestamp: baseline.timestamp + timeOffset},
        {...event, timestamp: lastTimestamp},
      ], {root: stage, liveMode: true, mouseTail: false, triggerFocus: false,
        showWarning: false, pauseAnimation: true, loadTimeout: 0});
      // Never enable script execution, even if an upstream default changes.
      replayer.iframe.setAttribute('sandbox', 'allow-same-origin');
      replayer.disableInteract();
      replayer.startLive(Date.now() - 80);
      layout();
    } else if (replayer) {
      const timestamp = Math.max(lastTimestamp, event.timestamp + timeOffset);
      lastTimestamp = timestamp;
      replayer.addEvent({...event, timestamp});
    }
  }
  function receiveCursor(message) {
    if (typeof message.id !== 'string' || message.id.length > 128 ||
        !Number.isFinite(message.x) || !Number.isFinite(message.y)) return;
    let peer = peers.get(message.id);
    if (!peer) {
      if (peers.size >= 32) return;
      const element = document.createElement('div');
      element.className = 'gnosis-shared-cursor';
      const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      svg.setAttribute('viewBox', '0 0 22 26');
      const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      path.setAttribute('d', 'M2,2 L5,23 L10,16 L18,15 Z');
      path.setAttribute('stroke', '#fff');
      path.setAttribute('stroke-width', '1.5');
      svg.appendChild(path);
      const label = document.createElement('span');
      element.append(svg, label);
      cursors.appendChild(element);
      peer = {element, path, label};
      peers.set(message.id, peer);
    }
    const color = palette[Number.isInteger(message.color) && message.color >= 0 ?
      message.color % palette.length : 0];
    peer.path.setAttribute('fill', color);
    peer.label.style.backgroundColor = color;
    peer.label.textContent = typeof message.name === 'string' ? message.name.slice(0, 80) : 'Guest';
    peer.x = Math.max(0, Math.min(host.width, message.x));
    peer.y = Math.max(0, Math.min(host.height, message.y));
    positionCursor(peer);
  }
  function receiveOpaque(message) {
    if (!Array.isArray(message.elements) || message.elements.length > 128 ||
        typeof message.image !== 'string' || message.image.length > 16 * 1024 * 1024 ||
        !/^[A-Za-z0-9+/]*={0,2}$/.test(message.image) ||
        message.width !== host.width || message.height !== host.height) return;
    const fragments = [];
    for (const rect of message.elements) {
      if (![rect.id, rect.x, rect.y, rect.width, rect.height].every(Number.isFinite) ||
          rect.width <= 0 || rect.height <= 0) continue;
      const fragment = document.createElement('div');
      Object.assign(fragment.style, {position: 'absolute', left: `${rect.x}px`, top: `${rect.y}px`,
        width: `${rect.width}px`, height: `${rect.height}px`,
        backgroundImage: `url("data:image/jpeg;base64,${message.image}")`,
        backgroundSize: `${message.width}px ${message.height}px`,
        backgroundPosition: `${-rect.x}px ${-rect.y}px`, backgroundRepeat: 'no-repeat'});
      fragments.push(fragment);
    }
    opaqueLayer.replaceChildren(...fragments);
  }
  function receive(message) {
    if (ended || !message || typeof message !== 'object') return;
    switch (message.kind) {
    case 'event': receiveEvent(message.event); break;
    case 'viewport':
      if (Number.isFinite(message.width) && Number.isFinite(message.height) &&
          message.width > 0 && message.height > 0 && message.width <= 32768 && message.height <= 32768) {
        host = {width: message.width, height: message.height};
        opaqueLayer.replaceChildren();
        layout();
      }
      break;
    case 'cursor': receiveCursor(message); break;
    case 'left':
      peers.get(message.id)?.element.remove();
      peers.delete(message.id);
      break;
    case 'opaque': receiveOpaque(message); break;
    case 'zoom': setZoom(message.factor); break;
    case 'ended': stop(); break;
    }
  }
  function stop() {
    if (ended) return;
    ended = true;
    clearPlayer();
    for (const remove of removers) remove();
    clearTimeout(compositionTimer);
    cursors.replaceChildren();
    peers.clear();
    pressed.clear();
    input.remove();
    style.remove();
    delete globalThis.__gnosisSharedView;
  }
  const modifierBits = event => (event.altKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) |
    (event.metaKey ? 4 : 0) | (event.shiftKey ? 8 : 0);
  const buttons = ['left', 'middle', 'right'];
  on(window, 'resize', layout);
  on(document, 'contextmenu', event => event.preventDefault());
  on(document, 'pointerdown', event => {
    const point = geometry.toHost(transform, event.clientX, event.clientY);
    if (!point || event.button > 2) return;
    event.preventDefault();
    if (event.altKey && event.button === 0) {
      dragging = {x: event.clientX, y: event.clientY, panX: pan.x, panY: pan.y};
      return;
    }
    input.style.left = `${event.clientX}px`;
    input.style.top = `${event.clientY}px`;
    input.focus({preventScroll: true});
    pressed.set(event.button, point);
    send({kind: 'mouse', type: 'mousePressed', button: buttons[event.button], ...point});
  });
  on(document, 'pointermove', event => {
    if (dragging) {
      pan = {x: dragging.panX + event.clientX - dragging.x,
        y: dragging.panY + event.clientY - dragging.y};
      layout();
      return;
    }
    const point = geometry.toHost(transform, event.clientX, event.clientY);
    const now = performance.now();
    if (point && now - lastPointer >= 33) {
      lastPointer = now;
      send({kind: 'cursor', ...point});
    }
  }, {passive: true});
  on(document, 'pointerup', event => {
    dragging = null;
    if (!pressed.has(event.button)) return;
    const point = geometry.toHost(transform, event.clientX, event.clientY, true) || pressed.get(event.button);
    pressed.delete(event.button);
    send({kind: 'mouse', type: 'mouseReleased', button: buttons[event.button], ...point});
  });
  const releaseButtons = () => {
    dragging = null;
    for (const [button, point] of pressed) send({kind: 'mouse', type: 'mouseReleased', button: buttons[button], ...point});
    pressed.clear();
  };
  on(window, 'blur', releaseButtons);
  on(document, 'pointercancel', releaseButtons);
  on(document, 'wheel', event => {
    event.preventDefault();
    if (event.ctrlKey || event.metaKey) {
      setZoom(zoom * Math.exp(-event.deltaY * 0.002), event.clientX, event.clientY);
      return;
    }
    const point = geometry.toHost(transform, event.clientX, event.clientY);
    if (!point) return;
    const unit = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? host.height : 1 / transform.scale;
    send({kind: 'wheel', ...point, deltaX: event.deltaX * unit, deltaY: event.deltaY * unit});
  }, {passive: false});
  on(document, 'keydown', event => {
    if (event.isComposing || composing) return;
    if ((event.ctrlKey || event.metaKey) && ['+', '=', '-', '0'].includes(event.key)) {
      event.preventDefault();
      setZoom(event.key === '0' ? 1 : zoom * (event.key === '-' ? 1 / 1.2 : 1.2));
      return;
    }
    const editingKeys = ['Backspace', 'Delete', 'Enter', 'Tab', 'Escape', 'ArrowLeft', 'ArrowRight',
      'ArrowUp', 'ArrowDown', 'Home', 'End', 'PageUp', 'PageDown'];
    // AltGr printable input is text; do not turn Ctrl+Alt into host shortcuts.
    if (editingKeys.includes(event.key) || ((event.ctrlKey || event.metaKey) && !event.altKey &&
        /^[acvxzy]$/i.test(event.key))) {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'v') return;
      event.preventDefault();
      send({kind: 'key', key: event.key, code: event.code, modifiers: modifierBits(event)});
    }
  });
  on(input, 'compositionstart', () => { composing = true; lastComposition = null; });
  on(input, 'compositionend', event => {
    composing = false;
    const text = event.data || input.value;
    if (text) send({kind: 'text', text});
    input.value = '';
    lastComposition = text;
    clearTimeout(compositionTimer);
    compositionTimer = setTimeout(() => { lastComposition = null; }, 0);
  });
  on(input, 'input', event => {
    if (composing || event.isComposing) return;
    const text = input.value || event.data || '';
    input.value = '';
    if (lastComposition !== null && text === lastComposition) { lastComposition = null; return; }
    if (text) send({kind: 'text', text});
  });
  on(Gnosis, 'shared', event => receive(event.detail));
  globalThis.__gnosisSharedView = {
    stop,
    requestResize(width, height) {
      if (Number.isInteger(width) && Number.isInteger(height) && width >= 320 && height >= 240 &&
          width <= 16384 && height <= 16384) send({kind: 'resize-request', width, height});
    },
  };
  layout();
  send({kind: 'ready'});
})();
