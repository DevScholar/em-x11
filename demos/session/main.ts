/**
 * Session harness.
 *
 * M3: worker mode is the default. Main thread is a thin DOM-input
 * forwarder + orchestrator; Server Worker owns OffscreenCanvas + Host;
 * each wasm (twm, xcalc, xeyes) runs in its own Client Worker. Mirrors
 * xorg's server-process + client-process model.
 *
 * Legacy main-thread `Host` path still exists (see src/host/) and is
 * exercised by pyodide-tk + wacl-tk; it's no longer the default for
 * the demo session harness. Set `?legacy=1` to fall back.
 */

import { Orchestrator } from '../../src/worker/main-thread/orchestrator.js';
import { launchTwm, launchTwmWorker } from '../../src/runtime/twm-launch.js';
import { launchXcalc, launchXcalcWorker } from '../../src/runtime/xcalc-launch.js';
import { Host } from '../../src/host/index.js';

const useLegacy = new URLSearchParams(location.search).has('legacy');

if (useLegacy) {
  const host = new Host();
  host.install();

  await launchTwm(host);
  await host.launchClient({
    glueUrl: '/build/artifacts/xeyes/xeyes.js',
    wasmUrl: '/build/artifacts/xeyes/xeyes.wasm',
  });
  await launchXcalc(host);
  console.log('[emx11:main] legacy main-thread mode booted');
} else {
  const canvas = document.createElement('canvas');
  canvas.style.display = 'block';
  canvas.style.margin = '0 auto';
  canvas.style.boxShadow = '0 4px 24px rgba(0, 0, 0, 0.5)';
  canvas.tabIndex = 0;
  document.body.appendChild(canvas);

  const orch = new Orchestrator({ canvas, cssWidth: 1024, cssHeight: 768 });
  /* Expose on window for console debugging. */
  (window as unknown as { __orch: Orchestrator }).__orch = orch;

  /* twm first so its SubstructureRedirect lands before subsequent
   * launches; launchTwmWorker awaits waitForSubstructureRedirect
   * internally. */
  await launchTwmWorker(orch);

  await orch.launchClient({
    glueUrl: '/build/artifacts/xeyes/xeyes.js',
    wasmUrl: '/build/artifacts/xeyes/xeyes.wasm',
    name: 'emx11-xeyes',
  });

  await launchXcalcWorker(orch);

  console.log('[emx11:main] worker mode booted (twm + xeyes + xcalc)');
}
