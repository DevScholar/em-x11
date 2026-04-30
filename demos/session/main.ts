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
 * Probe (2026-04-29): launching xeyes first, then xcalc, to test the
 * hypothesis that twm crashes mid-way through the first reparent. If
 * xcalc's frame appears with a working titlebar, twm is alive and the
 * xeyes-only artifacts (no titlebar text, dead drag) are the first
 * window's path-specific bug. If xcalc also lacks titlebar text/drag,
 * the bug is in twm's steady-state, not in a one-shot crash.
 */

import { Host } from '../../src/host/index.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';

const host = new Host();
host.install();

await launchTwm(host);

await host.launchClient({
  glueUrl: '/build/artifacts/xeyes/xeyes.js',
  wasmUrl: '/build/artifacts/xeyes/xeyes.wasm',
});

await launchXcalc(host);

