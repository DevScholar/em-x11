/**
 * xt-hello demo harness — single-program mode.
 *
 * createEmX11 creates the Host; spread moduleOverrides into the
 * Emscripten factory. No child_process needed for a single wasm
 * program.
 */

import { createEmX11 } from '../../src/index.js';

const x11 = await createEmX11({ width: 1024, height: 768 });

const factory = (await import('/build/artifacts/xt-hello/xt-hello.js')).default;
await factory({ ...x11.moduleOverrides, thisProgram: 'xt-hello' });
