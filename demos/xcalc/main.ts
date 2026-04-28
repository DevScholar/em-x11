/**
 * xcalc demo harness: install em-x11 host, then launch the Xaw calculator.
 *
 * app-defaults/XCalc is staged into MEMFS by launchXcalc so Xt can resolve
 * the widget geometry resources. See src/runtime/xcalc-launch.ts.
 */

import { Host } from '../../src/host/index.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';

const host = new Host();
host.install();

await launchXcalc(host);
