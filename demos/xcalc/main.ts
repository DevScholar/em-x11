/**
 * xcalc demo harness: install em-x11 host, then launch the Xaw calculator.
 *
 * app-defaults/XCalc is staged into MEMFS by launchXcalc so Xt can resolve
 * the widget geometry resources. See src/runtime/xcalcLaunch.ts.
 */

import { Host } from '../../src/runtime/host.js';
import { launchXcalc } from '../../src/runtime/xcalcLaunch.js';

const host = new Host();
host.install();

await launchXcalc(host);
