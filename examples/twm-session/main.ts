import { createEmX11 } from '../../src/index.js';
import { launchTwm } from '../../src/runtime/twm-launch.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';
import { stageXbitmaps } from '../../src/runtime/xbitmaps-stage.js';

const emX11 = await createEmX11({ width: 1024, height: 768 });

stageXbitmaps(emX11);

await launchTwm(emX11);

const xeyes = emX11.child_process.spawn('/artifacts/xeyes/xeyes', { thisProgram: 'xeyes' });
await xeyes.ready;

await launchXcalc(emX11);

console.log('[emX11:twm-session] booted twm + xeyes + xcalc');
