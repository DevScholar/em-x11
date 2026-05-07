/**
 * MEMFS staging helper for the Host path. Built as a `preRun` hook
 * the wasm factory can run directly. Lives in runtime/ because Host
 * launchers (twm, xcalc) are the only callers.
 */

import type { EmscriptenModule } from '../types/emscripten.js';

export interface StagedFile {
  path: string;
  contents: string;
}

/** mkdir -p every parent dir, then writeFile each entry. Returns a
 *  preRun-shaped function. */
export function stagePreRun(files: StagedFile[]): (mod: EmscriptenModule) => void {
  return (mod) => {
    const fs = mod.FS;
    if (!fs) {
      throw new Error(
        'em-x11: wasm has no FS — was it built without Emscripten filesystem support?',
      );
    }
    const dirs = new Set<string>();
    for (const f of files) {
      const parts = f.path.split('/').filter((p) => p.length > 0);
      for (let i = 1; i < parts.length; i++) {
        dirs.add('/' + parts.slice(0, i).join('/'));
      }
    }
    for (const dir of [...dirs].sort((a, b) => a.length - b.length)) {
      try {
        fs.mkdir(dir);
      } catch (e) {
        /* Emscripten ErrnoError for EEXIST is errno 20; its .message
         * is literally "FS error" so we can't rely on substring. */
        const errno = (e as { errno?: number }).errno;
        if (errno === 20) continue;
        const msg = (e as Error).message ?? '';
        if (msg.includes('exist') || msg.includes('EEXIST')) continue;
        throw e;
      }
    }
    for (const f of files) {
      fs.writeFile(f.path, f.contents);
    }
  };
}
