/**
 * hello demo harness — single-program mode.
 *
 * createEmX11 creates the Host; spread moduleOverrides into the
 * Emscripten factory so the C-side bridges find it on
 * Module['emx11Host']. No child_process, no fs staging.
 */

import { createEmX11 } from '../../src/index.js';

const x11 = await createEmX11({ width: 1024, height: 768 });

const factory = (await import('/build/artifacts/hello/hello.js')).default;
await factory({ ...x11.moduleOverrides });
