/**
 * xaw-hello demo harness: install em-x11 host, then load the Xaw-linked wasm.
 */

import { Host } from '../../src/runtime/host.js';
import { EventBridge } from '../../src/runtime/events.js';
import { loadWasm } from '../../src/loader/wasm.js';

const host = new Host();
host.install();

const base = '/build/artifacts/xaw-hello';
const module = await loadWasm({
  glueUrl: `${base}/xaw-hello.js`,
  wasmUrl: `${base}/xaw-hello.wasm`,
});

new EventBridge(host.canvas.element, host.compositor, module);
