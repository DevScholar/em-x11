/**
 * xt-hello demo harness. Spawns the xt-hello wasm in its own Client
 * Worker via Orchestrator.
 */

import { Orchestrator } from '../../src/worker/main-thread/orchestrator.js';

const canvas = document.createElement('canvas');
canvas.style.display = 'block';
canvas.style.margin = '0 auto';
canvas.tabIndex = 0;
document.body.appendChild(canvas);

const orch = new Orchestrator({ canvas, cssWidth: 1024, cssHeight: 768 });

const base = '/build/artifacts/xt-hello';
await orch.launchClient({
  glueUrl: `${base}/xt-hello.js`,
  wasmUrl: `${base}/xt-hello.wasm`,
  name: 'emx11-xt-hello',
});
