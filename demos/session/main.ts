/**
 * Session harness.
 *
 * Two modes:
 *   - default (`?worker=1` absent): legacy main-thread Host. All wasm
 *     clients share the main JS thread via Asyncify yields.
 *   - `?worker=1`: new Worker-based architecture. Main thread forwards
 *     DOM input to a Server Worker that owns the OffscreenCanvas and
 *     the Renderer; each wasm client gets its own Worker. See
 *     plans/tender-jingling-boot.md.
 *
 * M2 scope: only xeyes is launched under worker mode (simplest client,
 * exercises every EM_JS category). twm + xcalc fall in M3 once the
 * input-sink grab-table + subscriber routing migrate.
 */

import { Host } from '../../src/host/index.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';
import { Orchestrator } from '../../src/worker/main-thread/orchestrator.js';

const useWorker = new URLSearchParams(location.search).has('worker');

if (useWorker) {
  const canvas = document.createElement('canvas');
  canvas.style.display = 'block';
  canvas.style.margin = '0 auto';
  canvas.style.boxShadow = '0 4px 24px rgba(0, 0, 0, 0.5)';
  canvas.tabIndex = 0;
  document.body.appendChild(canvas);

  const orch = new Orchestrator({ canvas, cssWidth: 1024, cssHeight: 768 });
  /* Expose on window for console debugging. */
  (window as unknown as { __orch: Orchestrator }).__orch = orch;

  /* M2: xeyes only. */
  await orch.launchClient({
    glueUrl: '/build/artifacts/xeyes/xeyes.js',
    wasmUrl: '/build/artifacts/xeyes/xeyes.wasm',
    name: 'emx11-xeyes',
  });
  console.log('[emx11:main] worker mode booted (M2: xeyes)');
} else {
  const host = new Host();
  host.install();

  await launchTwm(host);

  await host.launchClient({
    glueUrl: '/build/artifacts/xeyes/xeyes.js',
    wasmUrl: '/build/artifacts/xeyes/xeyes.wasm',
  });

  await launchXcalc(host);
}
