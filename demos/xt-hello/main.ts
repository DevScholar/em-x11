/**
 * xt-hello demo harness: install em-x11 host, then launch the Xt-linked wasm.
 */

import { Host } from '../../src/host/index.js';

const host = new Host();
host.install();

const base = '/build/artifacts/xt-hello';
await host.launchClient({
  glueUrl: `${base}/xt-hello.js`,
  wasmUrl: `${base}/xt-hello.wasm`,
});
