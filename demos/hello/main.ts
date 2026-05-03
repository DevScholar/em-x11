/**
 * hello demo harness. Spawns the hello wasm in its own Client Worker
 * via Orchestrator.
 */

import { Orchestrator } from '../../src/worker/main-thread/orchestrator.js';

const canvas = document.createElement('canvas');
canvas.style.display = 'block';
canvas.style.margin = '0 auto';
canvas.tabIndex = 0;
document.body.appendChild(canvas);

const orch = new Orchestrator({ canvas, cssWidth: 1024, cssHeight: 768 });

const base = '/build/artifacts/hello';
await orch.launchClient({
  glueUrl: `${base}/hello.js`,
  wasmUrl: `${base}/hello.wasm`,
  name: 'emx11-hello',
});
