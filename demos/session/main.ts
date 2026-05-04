/**
 * Session harness.
 *
 * Single-threaded mode: twm + xeyes + xcalc all run on the main JS
 * thread alongside the Host. launchClient is serialized — twm boots
 * first, arms SubstructureRedirectMask on root, and only then xeyes /
 * xcalc are launched so their MapRequest is intercepted by twm.
 */

import { Host } from '../../src/host/index.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';

const host = new Host({ width: 1024, height: 768 });
host.install();
/* Expose on window for console debugging. */
(window as unknown as { __host: Host }).__host = host;

/* twm first so its SubstructureRedirect on root lands before subsequent
 * launches; launchTwm awaits waitForSubstructureRedirect internally. */
await launchTwm(host);

await host.launchClient({
  glueUrl: '/build/artifacts/xeyes/xeyes.js',
  wasmUrl: '/build/artifacts/xeyes/xeyes.wasm',
  thisProgram: 'xeyes',
});

await launchXcalc(host);

console.log('[emx11:main] single-thread mode booted (twm + xeyes + xcalc)');
