/**
 * twm-session demo harness — Layer 3 (multi-process).
 *
 * This is the ONLY example that uses createEmX11 + child_process.spawn.
 * Multiple X clients (twm, xeyes, xcalc) share one display; twm boots
 * first and arms SubstructureRedirectMask on root so subsequent
 * MapRequests are intercepted by the window manager.
 *
 * For single-program use-cases (hello, xeyes, xcalc standalone, etc.)
 * prefer Layer 1 (emcc -sUSE_EMX11) or Layer 2 (initEmX11).
 */

import { createEmX11 } from '../../src/index.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';
import { stageXbitmaps } from '../../src/runtime/xbitmaps-stage.js';

const emX11 = await createEmX11({ width: 1024, height: 768 });

/* Stage the shared xbm package once — every spawn inherits MEMFS via the
 * replay hook, so any client that resolves a bitmap-file-path resource
 * finds the file without per-app fixups. */
stageXbitmaps(emX11);

/* twm first: launchTwm awaits waitForSubstructureRedirect internally so
 * subsequent spawns' MapRequests land on an armed WM. */
await launchTwm(emX11);

const xeyes = emX11.child_process.spawn('/build/artifacts/xeyes/xeyes', { thisProgram: 'xeyes' });
await xeyes.ready;

await launchXcalc(emX11);

console.log('[emx11:twm-session] booted twm + xeyes + xcalc');
