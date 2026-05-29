/**
 * xt-hello demo harness — Layer 2 (single program).
 *
 * initEmX11 creates the Host; we spread moduleOverrides into the
 * Emscripten factory.  No child_process needed for a single wasm
 * program.
 */

import { initEmX11 } from '../../src/index.js';

const x11 = await initEmX11({ width: 1024, height: 768 });

const factory = (await import('/build/artifacts/xt-hello/xt-hello.js')).default;
await factory({ ...x11.moduleOverrides, thisProgram: 'xt-hello' });
