/**
 * glxgears demo harness — single-program mode.
 *
 * createEmX11 creates the Host; spread moduleOverrides into the
 * Emscripten factory. glxgears spins its own animation loop via
 * emscripten_sleep(0); the JSPI flag in the cmake build handles
 * the suspend/resume.
 */

import { createEmX11 } from '../../src/index.js';

const x11 = await createEmX11({ width: 1024, height: 768 });

const factory = (await import('/build/artifacts/glxgears/glxgears.js')).default;
await factory({ ...x11.moduleOverrides, thisProgram: 'glxgears' });
