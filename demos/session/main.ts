/**
 * Session harness.
 *
 * Single-thread mode: twm + xeyes + xcalc all run on the main JS
 * thread alongside the em-x11 host. em.spawn is serialized — twm
 * boots first, arms SubstructureRedirectMask on root, and only then
 * xeyes / xcalc are launched so their MapRequest is intercepted by
 * twm.
 *
 * `globalThis.emX11` is also populated by createEmX11 for console
 * debugging (`emX11.debug.dumpWindows()`, `emX11._host` for the
 * unstable internal escape hatch).
 */

import { createEmX11 } from '../../src/index.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';

const em = await createEmX11({ width: 1024, height: 768 });

/* twm first so its SubstructureRedirect on root lands before subsequent
 * spawns; launchTwm awaits waitForSubstructureRedirect internally. */
await launchTwm(em);

const xeyes = em.spawn('/build/artifacts/xeyes/xeyes', { thisProgram: 'xeyes' });
await xeyes.ready;

await launchXcalc(em);

console.log('[emx11:session] booted twm + xeyes + xcalc');
