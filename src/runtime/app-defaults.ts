/**
 * Project-default app-defaults staging.
 *
 * Xt clients (xcalc, future xclock/xfontsel/xlogo, etc.) read their
 * widget geometry/colour resources from /usr/lib/X11/app-defaults/<Class>
 * — see third-party/libXt/include/X11/IntrinsicI.h's compile-time
 * XFILESEARCHPATHDEFAULT. Without that file, our Xrm finds nothing,
 * widgets fall back to their hard-coded defaults, and Form-based UIs
 * collapse on top of themselves at (0,0).
 *
 * Rather than have every demo (or every launcher) repeat the MEMFS
 * staging dance, we keep a registry keyed by the artifact glue URL.
 * `Host.launchClient` consults the registry on every launch and auto-
 * injects a preRun hook that mkdirs /usr/lib/X11/app-defaults and
 * writes each registered file. New Xt programs need a single
 * registerAppDefaults call to participate.
 *
 * Why key by glueUrl rather than program name: the glueUrl is what
 * launchClient already gets, no extra plumbing through the API. The
 * registry stores the trailing basename of the URL (e.g. `xcalc.js`)
 * so callers can use any artifactBase without breaking the lookup.
 *
 * The ?raw imports below let Vite inline these text assets at build
 * time — the wasm doesn't have to ship them and the dev server
 * doesn't have to serve them.
 */

import type { EmscriptenModule } from '../types/emscripten.js';
// eslint-disable-next-line import/no-unresolved
import xcalcAppDefaults from '../../third-party/xcalc/app-defaults/XCalc?raw';

/** A file to stage into MEMFS before main() runs. */
export interface MemfsFile {
  path: string;
  contents: string;
}

/** Registry keyed by glueUrl basename (e.g. `xcalc.js`). */
const REGISTRY: Map<string, MemfsFile[]> = new Map();

export function registerAppDefaults(glueBasename: string, files: MemfsFile[]): void {
  REGISTRY.set(glueBasename, files);
}

export function getAppDefaultsFor(glueUrl: string): MemfsFile[] | undefined {
  const slash = glueUrl.lastIndexOf('/');
  const basename = slash >= 0 ? glueUrl.slice(slash + 1) : glueUrl;
  return REGISTRY.get(basename);
}

/** preRun hook factory: mkdir -p the app-defaults dir, write each file. */
export function makeStagingPreRun(files: MemfsFile[]): (mod: EmscriptenModule) => void {
  return (mod) => {
    const fs = mod.FS;
    if (!fs) {
      throw new Error(
        'em-x11: wasm has no FS — was it built with the default ' +
          'Emscripten filesystem support?',
      );
    }
    /* mkdir -p every parent dir we need. The set of dirs is computed
     * from the file paths so registering a file at a new location
     * (e.g. /etc/Xresources) doesn't require updating this list. */
    const dirs = new Set<string>();
    for (const f of files) {
      const parts = f.path.split('/').filter((p) => p.length > 0);
      // Drop the basename; build parents incrementally.
      for (let i = 1; i < parts.length; i++) {
        dirs.add('/' + parts.slice(0, i).join('/'));
      }
    }
    // Sort by depth so parents are created before children.
    const ordered = [...dirs].sort((a, b) => a.length - b.length);
    for (const dir of ordered) {
      try {
        fs.mkdir(dir);
      } catch (e) {
        const msg = (e as Error).message ?? '';
        if (!msg.includes('exist') && !msg.includes('EEXIST')) throw e;
      }
    }
    for (const f of files) {
      fs.writeFile(f.path, f.contents);
    }
  };
}

/* -- Built-in registrations ------------------------------------------------
 *
 * Adding a new Xt program: import its app-defaults via `?raw` and call
 * registerAppDefaults at module load. The session/single-app demos will
 * pick it up automatically as soon as they `launchClient` the artifact. */

registerAppDefaults('xcalc.js', [
  { path: '/usr/lib/X11/app-defaults/XCalc', contents: xcalcAppDefaults },
]);
