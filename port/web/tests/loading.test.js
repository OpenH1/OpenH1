'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
require('../site/loading.js');

test('art moves without a simulation update and repeats after 1200 ms', () => {
  const first = HaloLoading.rectangles(0);
  assert.equal(first.length, 12);
  assert.deepEqual({ ...first[0], color: first[0].color.map(value => Math.round(value * 255)) },
    { x: 112, y: 124, width: 16, height: 8, color: [128, 204, 255] });
  assert.notDeepEqual(HaloLoading.rectangles(100), first);
  assert.deepEqual(HaloLoading.rectangles(1200), first);
});

test('startup, explicit epochs, delayed messages and fatal state', () => {
  const loading = HaloLoading.create();
  assert.equal(loading.frame(0, 0, false).visible, true);
  assert.equal(loading.frame(1, 1, false).visible, false);
  loading.update(3, 10, 100);
  assert.equal(loading.frame(10, 200, false).visible, true);
  assert.notDeepEqual(loading.frame(10, 200, false).rectangles, loading.frame(10, 300, false).rectangles);
  loading.update(2, 10, 300);
  assert.equal(loading.frame(10, 300, false).visible, true);
  loading.update(3, 10, 301); // queued begin arrives after shared end was seen
  assert.equal(loading.frame(11, 302, false).visible, false);
  loading.update(5, 11, 400);
  loading.update(2, 10, 401); // old epoch cannot hide the new map
  assert.equal(loading.frame(11, 402, false).visible, true);
  assert.equal(loading.frame(11, 403, true).visible, false);
});

test('real page RAF reads explicit worker state even when game frames stop', () => {
  const source = fs.readFileSync(path.join(__dirname, '../site/app.js'), 'utf8');
  const drawSource = source.slice(source.indexOf('function watchFrames()'), source.indexOf("$('play').addEventListener"));
  const callbacks = [], paints = [];
  const context = { clearRect() {}, fillRect(...values) { paints.push([this.fillStyle, ...values]); } };
  const elements = { 'loading-art': { getContext: () => context }, starting: {}, 'starting-text': {} };
  const heap = new Uint32Array(new SharedArrayBuffer(200));
  const sandbox = { gameStopped: false, game: { HEAPU32: heap, _web_state: () => 0 }, Atomics,
    loadingPresentation: HaloLoading.create(), $: id => elements[id], requestAnimationFrame: fn => callbacks.push(fn) };
  vm.runInNewContext(drawSource + '\nwatchFrames();', sandbox);
  heap[0] = 5;
  callbacks.shift()(0);
  assert.equal(elements.starting.hidden, true);
  Atomics.store(heap, 49, 3);
  callbacks.shift()(16);
  assert.equal(elements.starting.hidden, false);
  const first = paints.slice(-12);
  callbacks.shift()(116); // worker has made no progress
  assert.notDeepEqual(paints.slice(-12), first);
  Atomics.store(heap, 49, 2);
  heap[0] = 6;
  callbacks.shift()(132);
  assert.equal(elements.starting.hidden, true);
  sandbox.gameStopped = true;
  callbacks.shift()(148);
  assert.equal(callbacks.length, 0);
});
