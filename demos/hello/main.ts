/**
 * hello demo harness: install the em-x11 host, then launch the hello wasm.
 */

import { Host } from '../../src/host/index.js';

const host = new Host();
host.install();

const base = '/build/artifacts/hello';
await host.launchClient({
  glueUrl: `${base}/hello.js`,
  wasmUrl: `${base}/hello.wasm`,
});
