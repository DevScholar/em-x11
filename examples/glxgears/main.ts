/**
 * glxgears demo harness. Mirrors xeyes pattern -- single-thread,
 * wasm runs alongside the em-x11 host on the main JS thread.
 *
 * The wasm exits no main() until the user closes the window; rotation
 * proceeds inside the patched event_loop, which yields a frame at a
 * time via emscripten_sleep(0) (the animation loop yields each frame
 * through the Xlib event-polling path).
 */

import { createEmX11 } from '../../src/index.js';

const emX11 = await createEmX11({ width: 1024, height: 768 });

const glxgears = emX11.child_process.spawn('/build/artifacts/glxgears/glxgears', {
  thisProgram: 'glxgears',
});
await glxgears.ready;
