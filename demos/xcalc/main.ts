/**
 * xcalc demo harness. Single-threaded mode: xcalc runs on the main JS
 * thread alongside the Host. app-defaults/XCalc is staged into MEMFS
 * via the Emscripten preRun hook (see src/runtime/xcalc-launch.ts).
 */

import { Host } from '../../src/host/index.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';

const host = new Host({ width: 800, height: 600 });
host.install();
await launchXcalc(host);
