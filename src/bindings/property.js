/*
 * Emscripten --js-library: property bridges. Property storage lives
 * Host-side (xserver/dix/property.c layout: keyed by XID + atom,
 * cross-client readable). These four bridges mirror the Xlib entry
 * points we support: XChangeProperty, XGetWindowProperty (split into
 * meta + data for caller-allocated buffers), XDeleteProperty,
 * XListProperties.
 */

addToLibrary({
  // XChangeProperty. mode: 0=Replace, 1=Prepend, 2=Append (X.h).
  // Returns 1 on success, 0 on BadMatch (format/type mismatch with
  // existing entry under Prepend/Append), -1 on BadWindow (unknown XID).
  emx11_js_change_property: function (w, atom, type, format, mode, dataPtr, nelements) {
    if (!globalThis.__EMX11__) return -1;
    var unit = format === 8 ? 1 : format === 16 ? 2 : format === 32 ? 4 : 0;
    if (unit === 0) return 0;
    var bytes = unit * (nelements | 0);
    var data = bytes > 0
      ? HEAPU8.slice(dataPtr, dataPtr + bytes)
      : new Uint8Array(0);
    var ok = globalThis.__EMX11__.changeProperty(
      w >>> 0, atom >>> 0, type >>> 0, format | 0, mode | 0, data,
    );
    return ok ? 1 : 0;
  },

  // XGetWindowProperty -- two-call pattern so C can allocate the
  // return buffer itself. First call returns meta; second call fills
  // a caller-provided byte buffer. delete_flag is honoured on the
  // second call (when bytesAfter == 0 and longOffset == 0).
  //
  // metaPtr is an int[8]:
  //   [0] found (0/1)       [1] actual_type   [2] actual_format
  //   [3] nitems_returned   [4] bytes_after   [5] data_bytes
  //   [6] valid (1 if window exists, 0 = BadWindow)
  //   [7] reserved
  // When valid=0 the caller returns BadWindow. When valid=1 && found=0,
  // the atom isn't set on that window (Success with type=None).
  emx11_js_get_property_meta: function (w, atom, reqType, longOffset, longLength, metaPtr) {
    var base = metaPtr >> 2;
    for (var i = 0; i < 8; i++) HEAP32[base + i] = 0;
    if (!globalThis.__EMX11__) return;
    var r = globalThis.__EMX11__.peekProperty(
      w >>> 0, atom >>> 0, reqType >>> 0,
      longOffset | 0, longLength | 0, false,
    );
    if (r === null) {
      /* BadWindow */
      HEAP32[base + 6] = 0;
      return;
    }
    HEAP32[base + 6] = 1;
    HEAP32[base + 0] = r.found ? 1 : 0;
    HEAP32[base + 1] = r.type | 0;
    HEAP32[base + 2] = r.format | 0;
    HEAP32[base + 3] = r.nitems | 0;
    HEAP32[base + 4] = r.bytesAfter | 0;
    HEAP32[base + 5] = r.data.length | 0;
  },

  // Second half of the two-call. Writes up to `capacity` bytes of the
  // sliced property data into dstPtr, and deletes the entry if
  // deleteFlag and the slice covers the whole entry (matches
  // dix/property.c ProcGetProperty semantics).
  emx11_js_get_property_data: function (w, atom, reqType, longOffset, longLength, deleteFlag, dstPtr, capacity) {
    if (!globalThis.__EMX11__) return;
    var r = globalThis.__EMX11__.peekProperty(
      w >>> 0, atom >>> 0, reqType >>> 0,
      longOffset | 0, longLength | 0, deleteFlag !== 0,
    );
    if (!r || !r.found || r.data.length === 0) return;
    var n = Math.min(r.data.length, capacity | 0);
    HEAPU8.set(r.data.subarray(0, n), dstPtr);
  },

  emx11_js_delete_property: function (w, atom) {
    globalThis.__EMX11__ && globalThis.__EMX11__.deleteProperty(w >>> 0, atom >>> 0);
  },

  // Returns property count; caller then calls fetch with a buffer
  // sized accordingly.
  emx11_js_list_properties_count: function (w) {
    if (!globalThis.__EMX11__) return 0;
    return globalThis.__EMX11__.listProperties(w >>> 0).length;
  },

  emx11_js_list_properties_fetch: function (w, dstPtr, capacity) {
    if (!globalThis.__EMX11__) return 0;
    var atoms = globalThis.__EMX11__.listProperties(w >>> 0);
    var n = Math.min(atoms.length, capacity | 0);
    var base = dstPtr >> 2;
    for (var i = 0; i < n; i++) HEAPU32[base + i] = atoms[i] >>> 0;
    return n;
  },
});
