/**
 * xt-hello demo harness. Single-thread mode: the wasm runs on the main
 * JS thread alongside the em-x11 host. No workers, no OffscreenCanvas.
 */

import { createEmX11 } from '../../src/index.js';

const em = await createEmX11({ width: 1024, height: 768 });

const xtHello = em.spawn('/build/artifacts/xt-hello/xt-hello', { thisProgram: 'xt-hello' });
await xtHello.ready;
