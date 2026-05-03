/**
 * twm-only demo. Useful for inspecting how the window manager itself
 * paints: root weave, icon manager frame, default menu. No managed
 * clients are launched, so every window on screen belongs to twm.
 */

import { Orchestrator } from '../../src/worker/main-thread/orchestrator.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';

const canvas = document.createElement('canvas');
canvas.style.display = 'block';
canvas.style.margin = '0 auto';
canvas.tabIndex = 0;
document.body.appendChild(canvas);

const orch = new Orchestrator({ canvas, cssWidth: 1024, cssHeight: 768 });
await launchTwm(orch);
