/**
 * Session harness.
 *
 * Launches twm (the WM) first, then a sequence of clients. Twm is the
 * target of the SubstructureRedirect path in Host; the clients map
 * their shells after twm is ready to observe them. The real X.Org
 * server source under references/xserver/ is the authority we're
 * matching Host's semantics to — DIX window.c / events.c are the spec.
 *
 * twm gets a custom twmrc with RandomPlacement enabled, staged via
 * MEMFS preRun. The vendored twm source isn't git-tracked, so any
 * config tweak that twm needs to behave well in our headless single-
 * thread world has to come from the runtime side. See twm-launch.ts.
 *
 * Two modes:
 *   - default (`?worker=1` absent): legacy main-thread Host. All wasm
 *     clients share the main JS thread via Asyncify yields.
 *   - `?worker=1`: new Worker-based architecture. Main thread forwards
 *     DOM input to a Server Worker that owns the OffscreenCanvas and
 *     the Renderer; each wasm client gets its own Worker. See
 *     plans/tender-jingling-boot.md. M1 is a smoke test (no wasm yet).
 */

import { Host } from '../../src/host/index.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';
import { spawnServerWorker } from '../../src/worker/main-thread/server-proxy.js';
import { attachInputForwarder } from '../../src/worker/main-thread/input-forwarder.js';

const useWorker = new URLSearchParams(location.search).has('worker');

if (useWorker) {
  /* M1 smoke-test: Server Worker spins up, paints a green rect, logs
   * mouse events it receives from the input forwarder. No wasm clients
   * yet -- those land in M2. */
  const canvas = document.createElement('canvas');
  canvas.style.display = 'block';
  canvas.style.margin = '0 auto';
  canvas.style.boxShadow = '0 4px 24px rgba(0, 0, 0, 0.5)';
  canvas.tabIndex = 0;
  document.body.appendChild(canvas);

  const handle = spawnServerWorker({ canvas, cssWidth: 1024, cssHeight: 768 });
  attachInputForwarder({ canvas, channel: handle.channel });
  console.log('[emx11:main] worker mode booted (M1 smoke test)');
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
