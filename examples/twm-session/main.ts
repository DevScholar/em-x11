/**
 * twm-session harness.
 *
 * Single-thread mode: twm + xeyes + xcalc all run on the main JS
 * thread alongside the em-x11 host. em.child_process.spawn is serialized — twm
 * boots first, arms SubstructureRedirectMask on root, and only then
 * xeyes / xcalc are launched so their MapRequest is intercepted by
 * twm.
 *
 * glxgears intentionally omitted: its rAF-driven animation and
 * per-frame logging drown out other demo signal.
 *
 * `globalThis.emX11` is also populated by createEmX11 for console
 * debugging (`emX11.debug.dumpWindows()`, `emX11._host` for the
 * unstable internal escape hatch).
 */

import { createEmX11 } from '../../src/index.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';
import { stageXbitmaps } from '../../src/runtime/xbitmaps-stage.js';

const emX11 = await createEmX11({ width: 1024, height: 768 });

/* Stage the shared xbm package once for the whole session: every spawn
 * inherits MEMFS via the replay hook, so any client that resolves a
 * bitmap-file-path resource (xcalc -> "calculator", xfd -> ..., etc.)
 * finds the file without per-app fixups. */
stageXbitmaps(emX11);

/* twm first so its SubstructureRedirect on root lands before subsequent
 * child_process.spawn calls; launchTwm awaits waitForSubstructureRedirect internally. */
await launchTwm(emX11);

const xeyes = emX11.child_process.spawn('/build/artifacts/xeyes/xeyes', { thisProgram: 'xeyes' });
await xeyes.ready;

await launchXcalc(emX11);

console.log('[emx11:twm-session] booted twm + xeyes + xcalc');
