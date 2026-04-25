/**
 * xt-hello demo harness: install em-x11 host, then load the Xt-linked wasm.
 */

import { Host } from '../../src/runtime/host.js';
import { EventBridge } from '../../src/runtime/events.js';
import { loadWasm } from '../../src/loader/wasm.js';

const host = new Host();
host.install();

const base = '/build/artifacts/xt-hello';
const module = await loadWasm({
  glueUrl: `${base}/xt-hello.js`,
  wasmUrl: `${base}/xt-hello.wasm`,
});

new EventBridge(host.canvas.element, host.compositor, module);
