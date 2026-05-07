/**
 * xcalc demo harness. Single-thread mode: xcalc runs on the main JS
 * thread alongside the em-x11 host. app-defaults/XCalc is staged into
 * em.fs and replayed into the Module's MEMFS at spawn time
 * (see src/runtime/xcalc-launch.ts).
 */

import { createEmX11 } from '../../src/index.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';

const em = await createEmX11({ width: 800, height: 600 });
await launchXcalc(em);
