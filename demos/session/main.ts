/**
 * Session harness.
 *
 * Launches twm (the WM) first, then xeyes (a client). Twm is the
 * target of the SubstructureRedirect path in Host; xeyes maps its
 * shell after twm is ready to observe it. The real X.Org server
 * source under references/xserver/ is the authority we're matching
 * Host's semantics to — DIX window.c / events.c are the spec.
 *
 * twm gets a custom twmrc with RandomPlacement enabled, staged via
 * MEMFS preRun. The vendored twm source isn't git-tracked, so any
 * config tweak that twm needs to behave well in our headless single-
 * thread world has to come from the runtime side. See twm-launch.ts.
 */

import { Host } from '../../src/host/index.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';

const host = new Host();
host.install();

await launchTwm(host);

await host.launchClient({
  glueUrl: '/build/artifacts/xeyes/xeyes.js',
  wasmUrl: '/build/artifacts/xeyes/xeyes.wasm',
});
