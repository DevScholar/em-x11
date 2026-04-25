/**
 * hello demo harness: install the em-x11 host, then load the hello wasm.
 */

import { Host } from '../../src/runtime/host.js';
import { EventBridge } from '../../src/runtime/events.js';
import { loadWasm } from '../../src/loader/wasm.js';

const host = new Host();
host.install();

const base = '/build/artifacts/hello';
const module = await loadWasm({
  glueUrl: `${base}/hello.js`,
  wasmUrl: `${base}/hello.wasm`,
});

new EventBridge(host.canvas.element, host.compositor, module);
