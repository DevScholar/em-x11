/**
 * twm demo harness: install em-x11 host, then launch twm as the sole wasm
 * client. Phase 0 is a "does it even boot" smoke test -- twm with no
 * managed clients just grabs the root and sits in its event loop.
 *
 * twm reads our staged twmrc from MEMFS via `-f`; see twmLaunch.ts.
 */

import { Host } from '../../src/runtime/host.js';
import { launchTwm } from '../../src/runtime/twmLaunch.js';

const host = new Host();
host.install();

await launchTwm(host);
