(emit => {
  'use strict';
  globalThis.__gnosisSharedRecord?.stop();
  if (typeof emit !== 'function' || typeof globalThis.rrweb?.record !== 'function')
    throw new Error('Shared page recorder is unavailable');

  let stopped = false;
  let stopRecord;
  let lastViewport = '';
  let lastCursor = 0;
  const listeners = [];
  const send = payload => { if (!stopped) emit(payload); };
  const on = (target, type, callback, options) => {
    target.addEventListener(type, callback, options);
    listeners.push(() => target.removeEventListener(type, callback, options));
  };
  function viewport(force = false) {
    const data = {kind: 'viewport', width: innerWidth, height: innerHeight,
      dpr: devicePixelRatio, url: location.href, title: document.title};
    const key = JSON.stringify(data);
    if (force || key !== lastViewport) {
      lastViewport = key;
      send(data);
    }
  }
  function opaque() {
    const elements = [];
    for (const element of document.querySelectorAll('canvas,video,iframe')) {
      const id = rrweb.record.mirror.getId(element);
      if (id < 0) continue;
      const rect = element.getBoundingClientRect();
      if (rect.width <= 0 || rect.height <= 0 || rect.bottom <= 0 || rect.right <= 0 ||
          rect.top >= innerHeight || rect.left >= innerWidth) continue;
      const style = getComputedStyle(element);
      if (style.visibility === 'hidden' || style.display === 'none' || style.opacity === '0') continue;
      elements.push({id, x: rect.x, y: rect.y, width: rect.width, height: rect.height});
      if (elements.length === 128) break;
    }
    send({kind: 'opaque', elements, width: innerWidth, height: innerHeight});
    viewport();
  }

  viewport(true);
  stopRecord = rrweb.record({
    emit: event => send({kind: 'event', event}),
    recordAfter: 'DOMContentLoaded',
    checkoutEveryNms: 30000,
    recordCanvas: false,
    recordCrossOriginIframes: false,
    maskInputOptions: {password: true},
    inlineStylesheet: true,
    sampling: {mousemove: false, mouseInteraction: false, scroll: 50},
  });
  on(window, 'resize', () => viewport(), {passive: true});
  on(document, 'pointermove', event => {
    const now = performance.now();
    if (now - lastCursor < 33) return;
    lastCursor = now;
    send({kind: 'cursor', x: event.clientX, y: event.clientY});
  }, {passive: true});
  const opaqueTimer = setInterval(opaque, 250);
  // rrweb's checkout threshold is event-driven; idle pages also need a bounded baseline.
  const snapshotTimer = setInterval(() => snapshot(), 30000);
  function snapshot() {
    if (stopped) return;
    viewport(true);
    rrweb.record.takeFullSnapshot(true);
    opaque();
  }
  const api = {
    snapshot,
    stop() {
      if (stopped) return;
      stopped = true;
      stopRecord?.();
      clearInterval(opaqueTimer);
      clearInterval(snapshotTimer);
      for (const remove of listeners) remove();
      if (globalThis.__gnosisSharedRecord === api) delete globalThis.__gnosisSharedRecord;
    },
  };
  globalThis.__gnosisSharedRecord = api;
  return api;
})
