/**
 * em.fs — Linux-style staging manifest.
 *
 * em-x11 is a JS library; it doesn't own its own Emscripten Module,
 * so it has no MEMFS of its own. Instead `em.fs` is a manifest:
 * writeFile / mkdir / mount calls record entries in memory, and
 * every spawned process replays them into its own MEMFS during the
 * preRun hook. This means callers can stage the X11 base layout
 * (twmrc, app-defaults, font config, library tarballs) once at boot
 * and have every wasm child see the same `/usr`, `/etc`, `/opt`.
 *
 * Default mounts established at construction time:
 *   /tmp    /usr    /etc    /opt    /var      → MEMFS (manifest only)
 *   /home                                     → MEMFS today; IDBFS-backed
 *                                               persistence is a future
 *                                               extension.
 *
 * `mount({type: 'tar', source})` decompresses a POSIX ustar archive
 * eagerly into the manifest, no streaming. Tar source can be a URL
 * (fetch'd here), an ArrayBuffer, or a Uint8Array. Gzipped tars are
 * NOT auto-decompressed — pre-extract with DecompressionStream at the
 * call site if needed (kept out of em.fs to avoid pulling a streaming
 * impl into the bundle for every consumer).
 */

import type { EmscriptenModule } from '../types/emscripten.js';
import type { EmX11FS, MountSpec } from './types.js';

interface ManifestEntry {
  /** Absolute path. Always begins with '/'. */
  path: string;
  /** null marks a directory; Uint8Array marks a regular file. */
  data: Uint8Array | null;
}

const DEFAULT_DIRS = ['/tmp', '/usr', '/etc', '/opt', '/var', '/home'];

export class FSNamespace implements EmX11FS {
  /** Ordered insertion. Replay walks this in order so directory
   *  creation precedes any file under it. Map preserves insertion
   *  order, which is what we want. */
  private readonly entries = new Map<string, ManifestEntry>();

  constructor() {
    for (const d of DEFAULT_DIRS) {
      this.entries.set(d, { path: d, data: null });
    }
  }

  writeFileSync(path: string, data: string | Uint8Array): void {
    const abs = normalize(path);
    this.ensureParents(abs);
    const bytes = typeof data === 'string' ? new TextEncoder().encode(data) : data;
    this.entries.set(abs, { path: abs, data: bytes });
  }

  readFileSync(path: string): Uint8Array | null {
    const abs = normalize(path);
    const e = this.entries.get(abs);
    return e?.data ?? null;
  }

  mkdirSync(path: string, opts?: { recursive?: boolean }): void {
    const abs = normalize(path);
    if (opts?.recursive) {
      this.ensureParents(abs);
    }
    if (!this.entries.has(abs)) {
      this.entries.set(abs, { path: abs, data: null });
    }
  }

  readdirSync(path: string): string[] {
    const abs = normalize(path);
    const prefix = abs === '/' ? '/' : abs + '/';
    const names: string[] = [];
    for (const key of this.entries.keys()) {
      if (key === abs) continue;
      if (!key.startsWith(prefix)) continue;
      const rest = key.slice(prefix.length);
      if (rest.includes('/')) continue;
      names.push(rest);
    }
    return names;
  }

  existsSync(path: string): boolean {
    return this.entries.has(normalize(path));
  }

  rmSync(path: string, opts?: { recursive?: boolean }): void {
    const abs = normalize(path);
    if (opts?.recursive) {
      const prefix = abs + '/';
      for (const key of [...this.entries.keys()]) {
        if (key === abs || key.startsWith(prefix)) this.entries.delete(key);
      }
    } else {
      this.entries.delete(abs);
    }
  }

  async mount(spec: MountSpec): Promise<void> {
    switch (spec.type) {
      case 'memfs':
      case 'idbfs':
        /* Both kinds collapse to "make sure the directory exists in
         * the manifest"; idbfs persistence is a future extension. */
        this.mkdirSync(spec.target, { recursive: true });
        return;
      case 'tar': {
        const buf = await loadTarBytes(spec.source);
        extractTar(buf, spec.target, this);
        return;
      }
    }
  }

  /** Build a preRun hook that replays the manifest into the spawned
   *  Module's MEMFS. Called by Process during spawn — not part of the
   *  public EmX11FS interface.
   *
   *  The manifest is best-effort: when the spawned wasm has no FS
   *  runtime (Emscripten's auto-pruner drops it for programs that
   *  never touch the filesystem, e.g. xeyes), the replay silently
   *  skips. Files staged by other processes shouldn't crash a wasm
   *  that doesn't care about them. We log a one-shot warning if the
   *  manifest has user content so the case is at least visible —
   *  helpful when a developer EXPECTED their program to read the
   *  staged files but built it without `-s FORCE_FILESYSTEM=1`. */
  buildPreRun(): (mod: EmscriptenModule) => void {
    const snapshot = [...this.entries.values()];
    const hasUserContent = snapshot.some(
      (e) => e.data !== null || !DEFAULT_DIRS.includes(e.path),
    );
    return (mod) => {
      /* Nothing staged -> don't even touch mod.FS. Accessing it on a
       * wasm built without FS support calls abort() inside Emscripten's
       * getter, which sets ABORT = true on the runtime BEFORE throwing
       * -- so try/catch saves the JS exception but the wasm runtime is
       * already poisoned and run() bails before main(). */
      if (!hasUserContent) return;
      const fs = mod.FS;
      if (!fs) {
        if (!this.warnedNoFs) {
          this.warnedNoFs = true;
          console.warn(
            'em-x11: spawned wasm has no FS runtime; skipping em.fs replay. ' +
              'If this program needs the staged files, rebuild it with ' +
              '`-s FORCE_FILESYSTEM=1` so Emscripten keeps the FS runtime.',
          );
        }
        return;
      }
      for (const e of snapshot) {
        if (e.data === null) {
          tryMkdir(fs, e.path);
        } else {
          this.ensureFsParents(fs, e.path);
          fs.writeFile(e.path, e.data);
        }
      }
    };
  }

  private warnedNoFs = false;

  private ensureParents(abs: string): void {
    const parts = abs.split('/').filter((p) => p.length > 0);
    for (let i = 1; i < parts.length; i++) {
      const dir = '/' + parts.slice(0, i).join('/');
      if (!this.entries.has(dir)) {
        this.entries.set(dir, { path: dir, data: null });
      }
    }
  }

  private ensureFsParents(fs: NonNullable<EmscriptenModule['FS']>, abs: string): void {
    const parts = abs.split('/').filter((p) => p.length > 0);
    for (let i = 1; i < parts.length; i++) {
      tryMkdir(fs, '/' + parts.slice(0, i).join('/'));
    }
  }
}

function normalize(path: string): string {
  if (!path.startsWith('/')) {
    throw new Error(`em-x11: em.fs paths must be absolute, got "${path}"`);
  }
  /* Strip trailing slash except for root. */
  if (path.length > 1 && path.endsWith('/')) return path.slice(0, -1);
  return path;
}

function tryMkdir(fs: NonNullable<EmscriptenModule['FS']>, dir: string): void {
  try {
    fs.mkdir(dir);
  } catch (e) {
    /* Emscripten's MEMFS throws an ErrnoError for "already exists";
     * its `.message` is literally "FS error" (no useful string), so
     * we have to read `.errno`. EEXIST is 20 in Emscripten's libc.
     * Anything else (ENOTDIR=54, EACCES=2, ...) is a real failure. */
    const errno = (e as { errno?: number }).errno;
    if (errno === 20) return;
    /* Fall back on substring match too — if Emscripten ever changes
     * the message string, both branches still cover us. */
    const msg = (e as Error).message ?? '';
    if (msg.includes('exist') || msg.includes('EEXIST')) return;
    throw e;
  }
}

async function loadTarBytes(source: string | ArrayBuffer | Uint8Array): Promise<Uint8Array> {
  if (typeof source === 'string') {
    const r = await fetch(source);
    if (!r.ok) {
      throw new Error(`em-x11: tar fetch failed: ${source} (HTTP ${r.status})`);
    }
    return new Uint8Array(await r.arrayBuffer());
  }
  if (source instanceof Uint8Array) return source;
  return new Uint8Array(source);
}

/* ------------------------------------------------------------------------
 * Minimal POSIX ustar parser. Handles regular files (typeflag '0' or
 * '\0') and directories ('5'). 512-byte header blocks; data padded to
 * 512-byte boundaries. Symlinks, hard links, longname (PaxHeader, GNU
 * 'L') are NOT handled — em-x11's tarballs ship pre-resolved.
 * ------------------------------------------------------------------------ */

function extractTar(buf: Uint8Array, target: string, fs: FSNamespace): void {
  const decoder = new TextDecoder('utf-8');
  let offset = 0;
  const baseTarget = normalize(target);
  while (offset + 512 <= buf.length) {
    const hdr = buf.subarray(offset, offset + 512);
    /* Two consecutive zero blocks mark end-of-archive; bail on first
     * empty header. */
    if (hdr[0] === 0) break;
    const name = readCString(hdr, 0, 100, decoder);
    const sizeOctal = readCString(hdr, 124, 12, decoder);
    const typeflag = String.fromCharCode(hdr[156] || 0x30);
    const prefix = readCString(hdr, 345, 155, decoder);
    const size = parseInt(sizeOctal.trim() || '0', 8);
    const fullName = prefix ? `${prefix}/${name}` : name;
    const abs = joinTarget(baseTarget, fullName);
    offset += 512;
    if (typeflag === '5') {
      fs.mkdirSync(abs, { recursive: true });
    } else if (typeflag === '0' || typeflag === '\0') {
      const data = buf.slice(offset, offset + size);
      fs.writeFileSync(abs, data);
    }
    /* Skip the padded data chunk. */
    offset += Math.ceil(size / 512) * 512;
  }
}

function readCString(buf: Uint8Array, start: number, len: number, decoder: TextDecoder): string {
  let end = start;
  const limit = start + len;
  while (end < limit && buf[end] !== 0) end++;
  return decoder.decode(buf.subarray(start, end));
}

function joinTarget(base: string, name: string): string {
  /* Strip any leading slash from the tar entry — tar paths are
   * relative. Also collapse './' which GNU tar emits at the front. */
  const cleaned = name.replace(/^\.\/+/, '').replace(/^\/+/, '');
  if (cleaned.length === 0) return base;
  return base === '/' ? '/' + cleaned : base + '/' + cleaned;
}
