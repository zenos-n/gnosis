'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const geometry = require('../../src/gnosis-share-geometry.js');
const near = (a, b) => assert.ok(Math.abs(a - b) < 1e-7, `${a} != ${b}`);

test('letterboxes in CSS pixels and ignores host/client DPR', () => {
  const host = {width: 1920, height: 1080, dpr: 1.6667};
  const client = {width: 1280, height: 1024, dpr: 2};
  const t = geometry.transform(host, client);
  near(t.scale, 2 / 3);
  near(t.top, 152);
  near(t.left, 0);
  assert.equal(geometry.toHost(t, 100, 151), null);
  assert.equal(geometry.toHost(t, 100, 872), null);
  assert.deepEqual(geometry.toHost(t, 640, 512), {x: 960, y: 540});
  assert.deepEqual(t, geometry.transform({...host, dpr: 3}, {...client, dpr: 1}));
});

test('portrait guests retain host layout with side/corner rejection', () => {
  const t = geometry.transform({width: 2560, height: 1440}, {width: 600, height: 1000});
  near(t.width, 600);
  near(t.height, 337.5);
  assert.equal(geometry.toHost(t, -1, 500), null);
  assert.equal(geometry.toHost(t, 600, 500), null);
  assert.equal(geometry.toHost(t, 400, 1), null);
  near(geometry.toHost(t, 300, 500).x, 1280);
});

test('zoom anchors a host point and clamps panning to the content bounds', () => {
  const t = geometry.transform({width: 1920, height: 1080}, {width: 960, height: 540});
  const next = geometry.zoomAt(t, 2, 300, 200);
  assert.deepEqual(geometry.toHost(next, 300, 200), geometry.toHost(t, 300, 200));
  const extreme = geometry.transform({width: 1920, height: 1080}, {width: 960, height: 540}, 2,
    {x: 1e6, y: -1e6});
  near(extreme.left, 0);
  near(extreme.top + extreme.height, 540);
  const reset = geometry.zoomAt(extreme, 1, 400, 200);
  near(reset.panX, 0);
  near(reset.panY, 0);
});

test('host to guest round trips at fractional scales and presentation zooms', () => {
  for (const host of [{width: 3440, height: 1440}, {width: 3840 / 1.6667, height: 2160 / 1.6667}]) {
    for (const factor of [1, 1.2, 2, 8]) {
      const t = geometry.transform(host, {width: 1377, height: 899}, factor, {x: 30, y: -20});
      for (const point of [{x: 10, y: 20}, {x: host.width / 2, y: host.height / 2}]) {
        const guest = geometry.toGuest(t, point.x, point.y);
        const back = geometry.toHost(t, guest.x, guest.y, true);
        near(back.x, point.x);
        near(back.y, point.y);
      }
    }
  }
});

test('rejects invalid viewports and clamps unsafe zoom/coordinates', () => {
  assert.equal(geometry.transform({width: 0, height: 100}, {width: 100, height: 100}), null);
  assert.equal(geometry.transform({width: Infinity, height: 1}, {width: 1, height: 1}), null);
  const t = geometry.transform({width: 100, height: 100}, {width: 100, height: 100}, 1e9);
  assert.equal(t.zoom, 8);
  assert.equal(geometry.toHost(t, NaN, 10), null);
  const outside = geometry.toHost(t, -1e9, 1e9, true);
  assert.deepEqual(outside, {x: 0, y: 99.999});
});
