/**
 * Combined twm + xeyes session harness.
 *
 * Step 2 proof-of-life: two independent wasm clients sharing one Host.
 * twm goes up first (convention -- matches "window manager starts
 * before clients" in a real X session); xeyes launches after. Neither
 * knows about the other yet -- there's no SubstructureRedirect wired
 * in, so xeyes's window lands unmanaged beside twm's iconmgr. Step 3
 * will let twm actually frame xeyes.
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
