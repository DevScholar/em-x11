/**
 * xcalc launcher.
 *
 * The vendored xcalc in third-party/ is a verbatim X.Org tarball that
 * gets fetched at setup time and is NOT git-tracked. Anything em-x11
 * specific lives on the runtime side.
 *
 * xcalc requires /usr/lib/X11/app-defaults/XCalc staged into MEMFS
 * before main() runs — without it XtGetResources finds nothing, widgets
 * realize with their compile-time defaults (0x0 Forms, stacked Commands),
 * and the calculator is unreadable. Stage via emX11.fs and let the
 * spawn-time replay drop it into the new Module's MEMFS.
 */

// eslint-disable-next-line
import xcalcAppDefaults from '../../third-party/xcalc/app-defaults/XCalc?raw';
import type { EmX11 } from '../api/emx11.js';
import type { Process } from '../api/types.js';

export interface LaunchXcalcOptions {
  /** Build artifact directory containing xcalc.js / xcalc.wasm. */
  artifactBase?: string;
}

export async function launchXcalc(
  emX11: EmX11,
  options: LaunchXcalcOptions = {},
): Promise<Process> {
  const base = options.artifactBase ?? '/build/artifacts/xcalc';
  emX11.fs.writeFileSync('/usr/lib/X11/app-defaults/XCalc', xcalcAppDefaults);
  /* `XCalc.iconPixmap: calculator` in the app-defaults pushes the
   * string "calculator" through XmuCvtStringToBitmap, which probes
   * /usr/include/X11/bitmaps/. The session demo stages the whole
   * xbitmaps package up front via stageXbitmaps() so the lookup
   * succeeds without an app-specific fixup here. */
  const p = emX11.spawn(`${base}/xcalc`, { thisProgram: 'xcalc' });
  await p.ready;
  return p;
}
