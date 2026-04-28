/*
 * Emscripten --js-library: clipboard bridge (see native/src/selection.c).
 *
 * Split-read pattern: _begin awaits navigator.clipboard.readText()
 * (Asyncify suspends C here), encodes to UTF-8, caches the bytes
 * under g.__emx11_clipbuf__, and returns the byte length (-1 on
 * error / permission denied). _fetch copies those bytes into the
 * caller's buffer and clears the cache. This avoids the need to
 * expose _malloc / _free to the JS library; C owns its own storage.
 *
 * Classic Asyncify (-s ASYNCIFY=1) requires the JS function to call
 * Asyncify.handleAsync with an `async` inner; plain `async function`
 * would return a Promise that Emscripten would coerce to an int
 * (garbage) without ever suspending the wasm.
 *
 * No xserver counterpart: real X uses Selection requests + Atom
 * properties on a proxy window; this is a browser-side shortcut so a
 * single user gesture grants the page's whole copy/paste flow.
 */

addToLibrary({
  emx11_js_clipboard_read_begin__deps: ['$Asyncify'],
  emx11_js_clipboard_read_begin__async: true,
  emx11_js_clipboard_read_begin: function () {
    return Asyncify.handleAsync(async () => {
      try {
        if (typeof navigator === 'undefined' ||
            !navigator.clipboard ||
            !navigator.clipboard.readText) {
          console.warn('[emx11] clipboard read: API unavailable');
          return -1;
        }
        var text = await navigator.clipboard.readText();
        var g = globalThis;
        g.__emx11_clipbuf__ = new TextEncoder().encode(text || '');
        return g.__emx11_clipbuf__.length;
      } catch (e) {
        console.warn('[emx11] clipboard read failed:', e);
        globalThis.__emx11_clipbuf__ = null;
        return -1;
      }
    });
  },

  emx11_js_clipboard_read_fetch: function (dstPtr, capacity) {
    var g = globalThis;
    var buf = g.__emx11_clipbuf__;
    if (!buf) return 0;
    var n = buf.length < capacity ? buf.length : capacity;
    HEAPU8.set(buf.subarray(0, n), dstPtr);
    g.__emx11_clipbuf__ = null;
    return n;
  },

  // Fire-and-forget: the C caller (XSendEvent intercept of the proxy
  // window's SelectionNotify reply) doesn't wait. If the browser
  // rejects writeText (no permission, not a user gesture, ...), log
  // and move on -- copy just fails silently from the user's perspective,
  // same as if Tk were running without a window manager.
  emx11_js_clipboard_write_utf8: function (dataPtr, len) {
    if (typeof navigator === 'undefined' ||
        !navigator.clipboard ||
        !navigator.clipboard.writeText) {
      console.warn('[emx11] clipboard write: API unavailable');
      return;
    }
    var bytes = HEAPU8.subarray(dataPtr, dataPtr + len);
    // Copy before the Promise runs: HEAPU8 is a view over wasm memory
    // and the wasm could re-enter allocation / grow_memory before the
    // microtask fires, invalidating the subarray.
    var copy = new Uint8Array(bytes);
    var text = new TextDecoder('utf-8').decode(copy);
    navigator.clipboard.writeText(text).catch(function (e) {
      console.warn('[emx11] clipboard write failed:', e);
    });
  },
});
