/**
 * twm demo harness: install em-x11 host, then launch twm as the sole wasm
 * client. Phase 0 is a "does it even boot" smoke test -- twm with no
 * managed clients just grabs the root and sits in its event loop.
 */

import { Host } from '../../src/runtime/host.js';

const host = new Host();
host.install();

const base = '/build/artifacts/twm';
await host.launchClient({
  glueUrl: `${base}/twm.js`,
  wasmUrl: `${base}/twm.wasm`,
});
