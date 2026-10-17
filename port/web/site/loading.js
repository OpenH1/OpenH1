/* Presentation-only art matches port/linux/src/loading_presentation.h.
   The browser's RAF clock keeps moving while the game worker is blocked. */
(function (root) {
  'use strict';
  function rectangles(elapsed) {
    const phase = (elapsed % 1200) / 100;
    return Array.from({ length: 12 }, (_, index) => {
      let distance = phase - index;
      if (distance < 0) distance += 12;
      const brightness = distance < 4 ? 1 - distance / 4 : 0;
      return { x: 112 + index * 24, y: 124, width: 16, height: 8,
        color: [.12 + brightness * .38, .22 + brightness * .58, .32 + brightness * .68] };
    });
  }
  function create() {
    let epoch = 0, active = false, started = 0, beginFrames = 0, seen = false, ended = false;
    return {
      update(state, frames, now) {
        const nextEpoch = state >>> 1;
        // A delayed proxy message must not restore an earlier map's overlay.
        if (nextEpoch < epoch) return;
        if (nextEpoch !== epoch) {
          epoch = nextEpoch;
          started = now;
          beginFrames = frames;
          seen = true;
          ended = false;
        }
        if (ended && (state & 1)) return;
        active = !!(state & 1);
        if (seen && !active) ended = true;
      },
      frame(frames, now, stopped) {
        const visible = !stopped && (active || (!seen ? frames === 0 : frames === beginFrames));
        const elapsed_ms = Math.max(0, now - started);
        return { visible, epoch, active, elapsed_ms, rectangles: rectangles(elapsed_ms) };
      },
    };
  }
  root.HaloLoading = { create, rectangles };
})(typeof globalThis === 'undefined' ? window : globalThis);
