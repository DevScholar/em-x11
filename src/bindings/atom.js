/*
 * Emscripten --js-library: atom table bridges. Mirrors xserver/dix/atom.c:
 * Host owns ids >= 69; the C side (native/src/atom.c) still resolves the
 * predefined atoms 1..68 locally to avoid a JS round-trip on hot paths.
 * Anything past the predefined range goes through these bridges so
 * cross-module interning agrees on a single id per name -- the fix for
 * the WM_PROTOCOLS / WM_DELETE_WINDOW divergence we used to see with
 * per-module tables.
 */

addToLibrary({
  emx11_js_intern_atom: function (namePtr, onlyIfExists) {
    if (!globalThis.__EMX11__ || namePtr === 0) return 0;
    var name = UTF8ToString(namePtr);
    return globalThis.__EMX11__.internAtom(name, onlyIfExists !== 0) >>> 0;
  },

  // Returns a malloc'd C string that the caller releases via XFree/free,
  // matching Xlib's ownership contract. Returns 0 (NULL) for unknown
  // ids; atom.c surfaces that as NULL which Xlib docs define as BadAtom.
  emx11_js_get_atom_name__deps: ['$stringToNewUTF8'],
  emx11_js_get_atom_name: function (atom) {
    if (!globalThis.__EMX11__) return 0;
    var name = globalThis.__EMX11__.getAtomName(atom >>> 0);
    if (name === null) return 0;
    return stringToNewUTF8(name);
  },
});
