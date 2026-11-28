/* CSS-pixel geometry: the host's devicePixelRatio never changes page layout. */
((root) => {
  'use strict';
  const clamp = (value, min, max) => Math.min(max, Math.max(min, value));
  const positive = value => Number.isFinite(value) && value > 0;

  function transform(host, client, zoom = 1, pan = {}) {
    if (![host.width, host.height, client.width, client.height].every(positive))
      return null;
    const factor = clamp(Number.isFinite(zoom) ? zoom : 1, 1, 8);
    const scale = Math.min(client.width / host.width, client.height / host.height) * factor;
    const width = host.width * scale;
    const height = host.height * scale;
    const limitX = Math.max(0, (width - client.width) / 2);
    const limitY = Math.max(0, (height - client.height) / 2);
    const panX = clamp(Number.isFinite(pan.x) ? pan.x : 0, -limitX, limitX);
    const panY = clamp(Number.isFinite(pan.y) ? pan.y : 0, -limitY, limitY);
    return {
      scale, width, height, zoom: factor, panX, panY,
      left: (client.width - width) / 2 + panX,
      top: (client.height - height) / 2 + panY,
      hostWidth: host.width, hostHeight: host.height,
      clientWidth: client.width, clientHeight: client.height,
    };
  }

  function toHost(t, x, y, bounded = false) {
    if (!t || !Number.isFinite(x) || !Number.isFinite(y)) return null;
    const hostX = (x - t.left) / t.scale;
    const hostY = (y - t.top) / t.scale;
    if (!bounded && (x < 0 || y < 0 || x >= t.clientWidth || y >= t.clientHeight ||
        hostX < 0 || hostY < 0 || hostX >= t.hostWidth || hostY >= t.hostHeight)) return null;
    return {x: clamp(hostX, 0, t.hostWidth - 0.001), y: clamp(hostY, 0, t.hostHeight - 0.001)};
  }

  function toGuest(t, x, y) {
    if (!t || !Number.isFinite(x) || !Number.isFinite(y)) return null;
    return {x: t.left + x * t.scale, y: t.top + y * t.scale};
  }

  function zoomAt(t, factor, x, y) {
    if (!t) return null;
    const point = toHost(t, x, y, true);
    const host = {width: t.hostWidth, height: t.hostHeight};
    const client = {width: t.clientWidth, height: t.clientHeight};
    const next = transform(host, client, factor);
    return transform(host, client, factor, {
      x: x - point.x * next.scale - next.left,
      y: y - point.y * next.scale - next.top,
    });
  }

  const geometry = Object.freeze({transform, toHost, toGuest, zoomAt});
  if (typeof module !== 'undefined' && module.exports) module.exports = geometry;
  else root.GnosisShareGeometry = geometry;
})(globalThis);
