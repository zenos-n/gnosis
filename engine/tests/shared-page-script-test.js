'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const geometry = require('../../src/gnosis-share-geometry.js');
const source = name => fs.readFileSync(path.join(__dirname, '../../src/', name), 'utf8');

class Target {
  listeners = new Map();
  addEventListener(type, callback) {
    if (!this.listeners.has(type)) this.listeners.set(type, new Set());
    this.listeners.get(type).add(callback);
  }
  removeEventListener(type, callback) { this.listeners.get(type)?.delete(callback); }
  dispatch(type, values = {}) {
    const event = {type, preventDefault() { this.defaultPrevented = true; }, ...values};
    for (const callback of this.listeners.get(type) || []) callback(event);
    return event;
  }
}
class Element extends Target {
  constructor(tag) { super(); this.tagName = tag; this.style = {}; this.attributes = {}; this.children = []; this.value = ''; }
  setAttribute(key, value) { this.attributes[key] = value; }
  appendChild(child) { child.parent = this; this.children.push(child); return child; }
  append(...children) { children.forEach(child => this.appendChild(child)); }
  replaceChildren(...children) { this.children = []; this.append(...children); }
  remove() { if (this.parent) this.parent.children = this.parent.children.filter(child => child !== this); }
  focus() { this.focused = true; }
}
const plain = value => JSON.parse(JSON.stringify(value));
function viewer() {
  const stage = new Element('div');
  const cursors = new Element('div');
  const document = new Target();
  Object.assign(document, {head: new Element('head'), body: new Element('body'),
    getElementById: id => ({stage, cursors}[id]),
    createElement: tag => new Element(tag), createElementNS: (_ns, tag) => new Element(tag)});
  const bridge = new Target();
  const messages = [];
  bridge.postMessage = (name, payload) => { assert.equal(name, 'shared'); messages.push(plain(payload)); };
  const window = new Target();
  const players = [];
  class Replayer {
    constructor(events, options) { this.events = events; this.options = options; this.iframe = new Element('iframe'); players.push(this); }
    disableInteract() { this.disabled = true; }
    startLive(time) { this.liveTime = time; }
    addEvent(event) { this.events.push(event); }
    destroy() { this.destroyed = true; }
  }
  const context = vm.createContext({document, Gnosis: bridge, rrweb: {Replayer},
    GnosisShareGeometry: geometry, window, innerWidth: 1280, innerHeight: 1024,
    performance: {now: () => 1000}, Date: {now: () => 1000000}, setTimeout, clearTimeout});
  vm.runInContext(source('gnosis-share-view.js'), context);
  return {context, document, stage, cursors, messages, players, window,
    input: document.body.children[0], receive: message => bridge.dispatch('shared', {detail: message})};
}
const meta = {type: 4, timestamp: 4000, data: {width: 1920, height: 1080, href: 'https://example.test'}};
const full = {type: 2, timestamp: 4001, data: {node: {}, initialOffset: {left: 0, top: 0}}};

test('viewer uses host CSS layout and a script-disabled live replay with normalized timestamps', () => {
  const v = viewer();
  assert.deepEqual(v.messages, [{kind: 'ready'}]);
  v.receive({kind: 'viewport', width: 1920, height: 1080, dpr: 2.5});
  v.receive({kind: 'event', event: meta});
  v.receive({kind: 'event', event: full});
  const player = v.players[0];
  assert.equal(player.options.liveMode, true);
  assert.equal(player.options.triggerFocus, false);
  assert.equal(player.options.UNSAFE_replayCanvas, undefined);
  assert.equal(player.iframe.attributes.sandbox, 'allow-same-origin');
  assert.equal(player.disabled, true);
  assert.equal(player.events[1].timestamp, 999920);
  assert.equal(player.liveTime, 999920);
  assert.equal(v.stage.style.width, '1920px');
  assert.match(v.stage.style.transform, /translate\(0px,152px\) scale\(0.6666666666666666\)/);
  v.receive({kind: 'zoom', factor: 2});
  const transform = v.stage.style.transform;
  v.receive({kind: 'event', event: {...full, timestamp: 34001}});
  assert.equal(player.destroyed, true);
  assert.equal(v.players.length, 2);
  assert.equal(v.stage.style.transform, transform);
  v.receive({kind: 'ended'});
});

test('pointer and wheel map to host CSS pixels; letterbox clicks and local zoom are not forwarded', () => {
  const v = viewer();
  v.receive({kind: 'viewport', width: 1920, height: 1080, dpr: 3});
  v.document.dispatch('pointerdown', {button: 0, clientX: 640, clientY: 10});
  assert.equal(v.messages.length, 1);
  v.document.dispatch('pointerdown', {button: 0, clientX: 640, clientY: 512});
  v.document.dispatch('pointerup', {button: 0, clientX: 640, clientY: 512});
  assert.deepEqual(v.messages.slice(1), [
    {kind: 'mouse', type: 'mousePressed', button: 'left', x: 960, y: 540},
    {kind: 'mouse', type: 'mouseReleased', button: 'left', x: 960, y: 540},
  ]);
  v.document.dispatch('wheel', {clientX: 640, clientY: 512, deltaX: 0, deltaY: 10, deltaMode: 0});
  assert.equal(v.messages.at(-1).deltaY, 15);
  v.document.dispatch('wheel', {ctrlKey: true, clientX: 640, clientY: 512, deltaX: 0, deltaY: -50});
  assert.equal(v.messages.at(-1).kind, 'zoom-changed');
  const count = v.messages.length;
  v.document.dispatch('pointerdown', {button: 0, altKey: true, clientX: 640, clientY: 512});
  v.document.dispatch('pointermove', {clientX: 690, clientY: 512});
  v.document.dispatch('pointerup', {button: 0, clientX: 690, clientY: 512});
  assert.equal(v.messages.length, count);
  v.receive({kind: 'ended'});
});

test('typing handles Unicode and IME without forwarding text twice', () => {
  const v = viewer();
  v.input.value = 'ł';
  v.input.dispatch('input', {data: 'ł'});
  assert.deepEqual(v.messages.at(-1), {kind: 'text', text: 'ł'});
  v.input.dispatch('compositionstart');
  v.input.value = 'に';
  v.input.dispatch('input', {data: 'に', isComposing: true});
  assert.equal(v.messages.length, 2);
  v.input.dispatch('compositionend', {data: '日本'});
  v.input.value = '日本';
  v.input.dispatch('input', {data: '日本'});
  assert.equal(v.messages.filter(message => message.text === '日本').length, 1);
  v.document.dispatch('keydown', {key: 'ArrowLeft', code: 'ArrowLeft', shiftKey: true});
  assert.deepEqual(v.messages.at(-1), {kind: 'key', key: 'ArrowLeft', code: 'ArrowLeft', modifiers: 8});
  const count = v.messages.length;
  v.receive({kind: 'ended'});
  v.input.value = 'after stop';
  v.input.dispatch('input');
  v.document.dispatch('pointermove', {clientX: 10, clientY: 10});
  assert.equal(v.messages.length, count);
});

test('cursors use inert text and palette colors, opaque images stay clipped to registered regions', () => {
  const v = viewer();
  v.receive({kind: 'viewport', width: 1920, height: 1080});
  v.receive({kind: 'cursor', id: 'guest', name: '<img src=x onerror=alert(1)>', color: 'red;bad', x: 960, y: 540});
  const cursor = v.cursors.children[0];
  assert.equal(cursor.children[1].textContent, '<img src=x onerror=alert(1)>');
  assert.equal(cursor.children[1].style.backgroundColor, '#3584e4');
  assert.equal(cursor.children[0].children[0].attributes.d, 'M2,2 L5,23 L10,16 L18,15 Z');
  v.receive({kind: 'opaque', image: 'YWJj', width: 1920, height: 1080,
    elements: [{id: 42, x: 100, y: 50, width: 200, height: 100}]});
  const layer = v.stage.children[0];
  assert.equal(layer.children.length, 1);
  assert.equal(layer.children[0].style.width, '200px');
  assert.equal(layer.children[0].style.backgroundPosition, '-100px -50px');
  v.receive({kind: 'opaque', image: 'bad")url(https://bad.test)', width: 1920, height: 1080, elements: []});
  assert.equal(layer.children.length, 1);
  v.receive({kind: 'left', id: 'guest'});
  assert.equal(v.cursors.children.length, 0);
  v.receive({kind: 'ended'});
});

test('recorder masks passwords, excludes unsafe recording and cleans up periodic snapshots/listeners', () => {
  const document = new Target();
  const canvas = {getBoundingClientRect: () => ({x: 4, y: 5, width: 20, height: 30, right: 24, bottom: 35})};
  Object.assign(document, {title: 'Signed in', querySelectorAll: () => [canvas]});
  const window = new Target();
  const messages = [];
  const timers = new Map();
  let recorderOptions;
  let snapshots = 0;
  let stopped = 0;
  function record(options) { recorderOptions = options; return () => { stopped++; }; }
  record.mirror = {getId: () => 123};
  record.takeFullSnapshot = () => { snapshots++; };
  const context = vm.createContext({document, window, rrweb: {record}, innerWidth: 1920, innerHeight: 1080,
    devicePixelRatio: 1.6667, location: {href: 'https://example.test'}, performance: {now: () => 1000},
    getComputedStyle: () => ({visibility: 'visible', display: 'block', opacity: '1'}),
    setInterval: (callback, interval) => { timers.set(interval, callback); return interval; },
    clearInterval: id => timers.delete(id)});
  const start = vm.runInContext(source('gnosis-share-record.js'), context);
  const api = start(message => messages.push(plain(message)));
  assert.equal(recorderOptions.recordCanvas, false);
  assert.equal(recorderOptions.recordCrossOriginIframes, false);
  assert.equal(recorderOptions.maskInputOptions.password, true);
  assert.equal(recorderOptions.checkoutEveryNms, 30000);
  assert.equal(messages[0].dpr, 1.6667);
  timers.get(250)();
  assert.deepEqual(messages.at(-1).elements, [{id: 123, x: 4, y: 5, width: 20, height: 30}]);
  timers.get(30000)();
  assert.equal(snapshots, 1);
  api.snapshot();
  assert.equal(snapshots, 2);
  api.stop();
  api.stop();
  assert.equal(stopped, 1);
  assert.equal(timers.size, 0);
  const count = messages.length;
  document.dispatch('pointermove', {clientX: 20, clientY: 20});
  recorderOptions.emit({type: 3, timestamp: 10, data: {}});
  assert.equal(messages.length, count);
  assert.equal(context.__gnosisSharedRecord, undefined);
});
