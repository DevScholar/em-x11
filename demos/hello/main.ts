/**
 * hello demo harness. Single-thread mode: the wasm runs on the main
 * JS thread alongside the em-x11 host. No workers, no OffscreenCanvas.
 */

import { createEmX11 } from '../../src/index.js';

const em = await createEmX11({ width: 1024, height: 768 });

const hello = em.spawn('/build/artifacts/hello/hello');
await hello.ready;
