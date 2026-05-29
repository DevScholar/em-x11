/**
 * Loader-level asset cache for .wasm binaries.
 *
 * The .js glue is imported directly via `import(glueUrl)` — the
 * browser's native module loader handles it.  The .wasm binary is
 * fetched through Cache Storage here so the large payload (up to
 * several MB) skips the network on repeat visits.
 *
 * Why Cache Storage and not IDBFS / localStorage / a Service Worker:
 *
 *   - localStorage is unavailable in workers and capped at ~5 MB —
 *     a single .wasm binary already exceeds that.
 *   - IDBFS would mean mounting an Emscripten FS and round-tripping
 *     bytes through it; we don't have a long-lived Module to mount
 *     into at this layer.
 *   - A Service Worker would catch everything (incl. dynamic .js
 *     imports) but adds activate / scope / HMR-fights-SW pain that's
 *     overkill when we control the only fetch sites. We keep the
 *     option open for "real PWA" later.
 *
 *   - Cache Storage is worker-friendly, sized for hundreds of MB,
 *     stores native Response objects (zero ser/de), and is tightly
 *     scoped to the URLs we fetch ourselves.
 *
 * Invalidation: the cache name is a single constant (`em-x11-loader`).
 * Editing it orphans every old cache; a one-shot `cleanupOldCaches()`
 * at boot deletes any stale `em-x11-loader-*` siblings. For deploy-time
 * content
 * changes under stable URLs, callers pass `'refresh'` to force a
 * write-through. For dev-mode iteration, the default mode is
 * `'bypass'` (set by EmX11 from `import.meta.env.DEV`).
 */

/** Cache name. Pre-alpha — no versioning yet. cleanupOldCaches() purges
 *  any older `em-x11-loader-` prefixed cache that doesn't match this
 *  exact name, so re-introducing a suffix later (or one-off invalidation
 *  for a poisoned-cache bug) is just a string edit. */
export const LOADER_CACHE_NAME = 'em-x11-loader';

/** Cache lookup behaviour.
 *  - `'use'`     — cache-first; fetch + populate on miss.
 *  - `'bypass'`  — never touch Cache Storage; plain fetch.
 *  - `'refresh'` — force a fetch and overwrite the cache entry. */
export type CacheMode = 'use' | 'bypass' | 'refresh';

/** Detect Cache Storage availability. Cache Storage requires a secure
 *  context (https or localhost) and is gated in some embedded
 *  environments; fall back gracefully when missing. */
function cachesAvailable(): boolean {
  return typeof caches !== 'undefined' && typeof caches.open === 'function';
}

/** Internal: fetch URL, optionally consulting/populating Cache Storage.
 *  Returns the bytes. Throws on non-2xx network responses. Also throws
 *  when the response Content-Type clearly contradicts the URL extension
 *  — that catches the common "preview server SPA-fell-back to
 *  index.html for a missing artifact URL" failure mode early, with a
 *  clear message instead of a downstream `Unexpected token '<'`. */
async function fetchWithCache(url: string, mode: CacheMode): Promise<Uint8Array> {
  if (mode === 'bypass' || !cachesAvailable()) {
    const r = await fetch(url);
    assertResponseOk(url, r);
    return new Uint8Array(await r.arrayBuffer());
  }
  const cache = await caches.open(LOADER_CACHE_NAME);
  if (mode === 'use') {
    const hit = await cache.match(url);
    if (hit) {
      try {
        /* Validate cache hits too, not just fresh fetches. A stale
         * entry from an older em-x11 build could be poisoned (HTML
         * fallback, partial body, etc.); if the stored Response now
         * fails the same checks we apply to fresh fetches, drop it
         * and refetch. Belt-and-braces alongside the cache-name
         * version bump. */
        assertResponseOk(url, hit);
        return new Uint8Array(await hit.arrayBuffer());
      } catch (e) {
        console.warn(
          `em-x11 loader: discarding stale cache entry for ${url}: ${(e as Error).message}`,
        );
        await cache.delete(url).catch(() => false);
        /* fall through to network fetch below */
      }
    }
  }
  /* miss, or 'refresh' explicitly asked — go to network and write back. */
  const fresh = await fetch(url);
  assertResponseOk(url, fresh);
  /* clone() is mandatory: cache.put consumes the body, and we still
   * need to read it ourselves. */
  try {
    await cache.put(url, fresh.clone());
  } catch (e) {
    /* Quota exceeded or opaque-response refusal: the fetched bytes
     * are still valid, just won't be cached. Log once and continue. */
    console.warn(`em-x11 loader: cache.put failed for ${url}:`, e);
  }
  return new Uint8Array(await fresh.arrayBuffer());
}

/** Throw a descriptive error on HTTP failure or content-type mismatch.
 *  The `.js`/`.wasm` vs `text/html` check is the load-bearing one — it
 *  catches SPA-fallback responses, where `vite preview` (or any other
 *  static host configured for SPA) returns `index.html` with HTTP 200
 *  for an unknown URL. Without this guard the failure surfaces deep
 *  inside `WebAssembly.instantiate` or `import(blob:...)` as a cryptic
 *  parse error. */
function assertResponseOk(url: string, r: Response): void {
  if (!r.ok) {
    throw new Error(`em-x11 loader: fetch ${url} (HTTP ${r.status})`);
  }
  const ctype = r.headers.get('content-type') ?? '';
  const isHtmlResponse = ctype.startsWith('text/html');
  const expectsCode = url.endsWith('.js') || url.endsWith('.wasm') || url.endsWith('.mjs');
  if (isHtmlResponse && expectsCode) {
    throw new Error(
      `em-x11 loader: ${url} returned HTML instead of code. ` +
        `This typically means the server fell back to index.html for an ` +
        `unknown URL — verify the artifact is actually being served at ` +
        `that path (Vite preview, for example, only serves files inside ` +
        `dist/; cmake outputs under build/artifacts/ must be copied into ` +
        `dist/ at build time).`,
    );
  }
}

/** Fetch the bytes at `url`, honouring the cache mode. */
export function cachedFetchBytes(url: string, mode: CacheMode): Promise<Uint8Array> {
  return fetchWithCache(url, mode);
}

/** Delete any `em-x11-loader-*` caches whose name is not the current
 *  LOADER_CACHE_NAME. Called once at host boot from EmX11.constructor.
 *  Cheap (`caches.keys()` is O(N) over a tiny list) and silent on
 *  environments without Cache Storage. */
export async function cleanupOldCaches(): Promise<void> {
  if (!cachesAvailable()) return;
  let names: string[];
  try {
    names = await caches.keys();
  } catch {
    return;
  }
  await Promise.all(
    names
      .filter((n) => n.startsWith('em-x11-loader-') && n !== LOADER_CACHE_NAME)
      .map((n) => caches.delete(n).catch(() => false)),
  );
}
