/**
 * xt-hello demo harness. Single-threaded mode: the wasm runs on the main
 * JS thread alongside the Host. No workers, no OffscreenCanvas.
 */

import { Host } from '../../src/host/index.js';

const host = new Host({ width: 1024, height: 768 });
host.install();

const base = '/build/artifacts/xt-hello';
await host.launchClient({
  glueUrl: `${base}/xt-hello.js`,
  wasmUrl: `${base}/xt-hello.wasm`,
});
