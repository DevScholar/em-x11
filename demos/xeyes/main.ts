/**
 * xeyes demo harness: install em-x11 host, then launch the Xt-linked wasm.
 */

import { Host } from '../../src/runtime/host.js';

const host = new Host();
host.install();

const base = '/build/artifacts/xeyes';
await host.launchClient({
  glueUrl: `${base}/xeyes.js`,
  wasmUrl: `${base}/xeyes.wasm`,
});
