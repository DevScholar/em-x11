/**
 * xaw-hello demo harness: install em-x11 host, then launch the Xaw-linked wasm.
 */

import { Host } from '../../src/runtime/host.js';

const host = new Host();
host.install();

const base = '/build/artifacts/xaw-hello';
await host.launchClient({
  glueUrl: `${base}/xaw-hello.js`,
  wasmUrl: `${base}/xaw-hello.wasm`,
});
