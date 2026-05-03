/**
 * xcalc demo harness. Spawns xcalc inside its own Client Worker via
 * Orchestrator; app-defaults/XCalc is staged into MEMFS by launchXcalc
 * so Xt can resolve the widget geometry resources (see
 * src/runtime/xcalc-launch.ts).
 */

import { Orchestrator } from '../../src/worker/main-thread/orchestrator.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';

const canvas = document.createElement('canvas');
canvas.style.display = 'block';
canvas.style.margin = '0 auto';
canvas.tabIndex = 0;
document.body.appendChild(canvas);

const orch = new Orchestrator({ canvas, cssWidth: 800, cssHeight: 600 });
await launchXcalc(orch);
