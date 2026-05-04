/**
 * Session harness.
 *
 * Worker-mode architecture: main thread is a thin DOM-input forwarder
 * + orchestrator; Server Worker owns OffscreenCanvas + Host; each wasm
 * (twm, xcalc, xeyes) runs in its own Client Worker. Mirrors xorg's
 * server-process + client-process model.
 */

import { Orchestrator } from '../../src/worker/main-thread/orchestrator.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';

const canvas = document.createElement('canvas');
canvas.style.display = 'block';
canvas.style.margin = '0 auto';
canvas.style.boxShadow = '0 4px 24px rgba(0, 0, 0, 0.5)';
canvas.tabIndex = 0;
document.body.appendChild(canvas);

const orch = new Orchestrator({ canvas, cssWidth: 1024, cssHeight: 768 });
/* Expose on window for console debugging. */
(window as unknown as { __orch: Orchestrator }).__orch = orch;

/* twm first so its SubstructureRedirect on root lands before subsequent
 * launches; launchTwm awaits waitForSubstructureRedirect internally. */
await launchTwm(orch);

await orch.launchClient({
  glueUrl: '/build/artifacts/xeyes/xeyes.js',
  wasmUrl: '/build/artifacts/xeyes/xeyes.wasm',
  thisProgram: 'xeyes',
  name: 'emx11-xeyes',
});

await launchXcalc(orch);

console.log('[emx11:main] worker mode booted (twm + xeyes + xcalc)');
