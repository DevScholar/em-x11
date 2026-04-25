/**
 * Session harness.
 *
 * Launches twm (the WM) first, then xeyes (a client). Twm is the
 * target of the SubstructureRedirect path in Host; xeyes maps its
 * shell after twm is ready to observe it. The real X.Org server
 * source under references/xserver/ is the authority we're matching
 * Host's semantics to — DIX window.c / events.c are the spec.
 */

import { Host } from '../../src/runtime/host.js';

const host = new Host();
host.install();

await host.launchClient({
  glueUrl: '/build/artifacts/twm/twm.js',
  wasmUrl: '/build/artifacts/twm/twm.wasm',
});

await host.launchClient({
  glueUrl: '/build/artifacts/xeyes/xeyes.js',
  wasmUrl: '/build/artifacts/xeyes/xeyes.wasm',
});
