/**
 * Loader-level asset cache.
 *
 * em.spawn fetches a `.js` glue and a `.wasm` binary every time it
 * runs. On the second visit those bytes don't have to come from the
 * network — both fetches go through the Cache Storage API here, keyed
 * by URL, under a versioned cache name.
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
 * Invalidation: the cache name embeds a version (`em-x11-loader-v1`).
 * Bumping the version constant orphans every old cache; a one-shot
 * `cleanupOldCaches()` at boot deletes them. For deploy-time content
 * changes under stable URLs, callers pass `'refresh'` to force a
 * write-through. For dev-mode iteration, the default mode is
 * `'bypass'` (set by EmX11 from `import.meta.env.DEV`).
 */

/** Cache name. Bump the suffix when the loader's stored representation
 *  changes shape. cleanupOldCaches() purges any older `em-x11-loader-`
 *  prefixed cache it finds. */
export const LOADER_CACHE_NAME = 'em-x11-loader-v1';

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
 *  Returns the bytes. Throws on non-2xx network responses. */
async function fetchWithCache(url: string, mode: CacheMode): Promise<Uint8Array> {
  if (mode === 'bypass' || !cachesAvailable()) {
    const r = await fetch(url);
    if (!r.ok) throw new Error(`em-x11 loader: fetch ${url} (HTTP ${r.status})`);
    return new Uint8Array(await r.arrayBuffer());
  }
  const cache = await caches.open(LOADER_CACHE_NAME);
  if (mode === 'use') {
    const hit = await cache.match(url);
    if (hit) return new Uint8Array(await hit.arrayBuffer());
  }
  /* miss, or 'refresh' explicitly asked — go to network and write back. */
  const fresh = await fetch(url);
  if (!fresh.ok) {
    throw new Error(`em-x11 loader: fetch ${url} (HTTP ${fresh.status})`);
  }
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

/** Fetch the bytes at `url`, honouring the cache mode. */
export function cachedFetchBytes(url: string, mode: CacheMode): Promise<Uint8Array> {
  return fetchWithCache(url, mode);
}

/** Fetch a JS glue file and return a Blob URL safe to dynamically
 *  import. The glue is text, but we keep one byte path so cacheable
 *  URLs flow through Cache Storage uniformly. The returned URL is
 *  an `URL.createObjectURL(blob)` — short-lived; the caller is
 *  expected to import() it immediately. We don't revoke automatically
 *  (the import keeps the module live as long as its handle is held;
 *  the blob URL becomes garbage-collectable on tab unload anyway). */
export async function cachedFetchAsBlobUrl(url: string, mode: CacheMode): Promise<string> {
  const bytes = await fetchWithCache(url, mode);
  /* Cast through `BlobPart`: TS narrows `Uint8Array<ArrayBufferLike>` and
   * refuses the more permissive runtime BlobPart type. The bytes are a
   * regular ArrayBuffer in practice (from response.arrayBuffer()). */
  const blob = new Blob([bytes as BlobPart], { type: 'text/javascript' });
  return URL.createObjectURL(blob);
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
