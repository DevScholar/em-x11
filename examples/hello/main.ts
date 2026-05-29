/**
 * hello demo harness — Layer 2 (single program).
 *
 * initEmX11 creates the Host; we spread moduleOverrides into the
 * Emscripten factory so the C-side bridges find it on
 * Module['emx11Host'].  No child_process, no fs staging — this is
 * the standard Emscripten pattern for a single wasm program.
 */

import { initEmX11 } from '../../src/index.js';

const x11 = await initEmX11({ width: 1024, height: 768 });

const factory = (await import('/build/artifacts/hello/hello.js')).default;
await factory({ ...x11.moduleOverrides });
