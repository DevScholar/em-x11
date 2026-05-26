/**
 * xeyes demo harness. Single-thread mode: the wasm runs on the main
 * JS thread alongside the em-x11 host. No workers, no OffscreenCanvas.
 */

import { createEmX11 } from '../../src/index.js';

const emX11 = await createEmX11({ width: 1024, height: 768 });

const xeyes = emX11.child_process.spawn('/build/artifacts/xeyes/xeyes', { thisProgram: 'xeyes' });
await xeyes.ready;
