/**
 * twm demo harness: install em-x11 host, then load twm as the sole wasm
 * client. Phase 0 is a "does it even boot" smoke test -- twm with no
 * managed clients just grabs the root and sits in its event loop.
 */

import { Host } from '../../src/runtime/host.js';
import { EventBridge } from '../../src/runtime/events.js';
import { loadWasm } from '../../src/loader/wasm.js';

const host = new Host();
host.install();

const base = '/build/artifacts/twm';
const module = await loadWasm({
  glueUrl: `${base}/twm.js`,
  wasmUrl: `${base}/twm.wasm`,
});

new EventBridge(host.canvas.element, host.compositor, module);
