/**
 * App-defaults staging utilities.
 *
 * Xt clients (xcalc, xclock, xfontsel, etc.) read their widget geometry/colour
 * resources from /usr/lib/X11/app-defaults/<Class> — see libXt's compile-time
 * XFILESEARCHPATHDEFAULT. Without that file, Xrm finds nothing, widgets fall
 * back to their hard-coded defaults, and Form-based UIs collapse at (0,0).
 *
 * Each launcher is responsible for staging its own app-defaults into MEMFS
 * before main() runs. Use makeStagingPreRun to build the preRun hook and pass
 * it to host.launchClient:
 *
 *   import myDefaults from '../app-defaults/MyApp?raw';
 *   host.launchClient({
 *     ...,
 *     preRun: [makeStagingPreRun([
 *       { path: '/usr/lib/X11/app-defaults/MyApp', contents: myDefaults },
 *     ])],
 *   });
 *
 * The ?raw import lets Vite inline the text asset at build time — the wasm
 * doesn't ship it and the dev server doesn't need to serve it separately.
 */

import type { EmscriptenModule } from '../types/emscripten.js';

/** A file to stage into MEMFS before main() runs. */
export interface MemfsFile {
  path: string;
  contents: string;
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

