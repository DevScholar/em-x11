/**
 * glxgears demo harness — Layer 2 (single program).
 *
 * initEmX11 creates the Host; we spread moduleOverrides into the
 * Emscripten factory.  glxgears spins its own animation loop via
 * emscripten_sleep(0); the ASYNCIFY flag in the cmake build handles
 * the unwind.
 */

import { initEmX11 } from '../../src/index.js';

const x11 = await initEmX11({ width: 1024, height: 768 });

const factory = (await import('/build/artifacts/glxgears/glxgears.js')).default;
await factory({ ...x11.moduleOverrides, thisProgram: 'glxgears' });
