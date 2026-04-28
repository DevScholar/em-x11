/*
 * Emscripten --js-library: pixmap bridges + drawable-to-drawable copies.
 * Mirrors xserver/dix/pixmap.c (CreatePixmap, FreePixmap), the core
 * CopyArea / CopyPlane / PutImage requests, and the Shape extension's
 * ShapeMask combine operation.
 */

addToLibrary({
  // Pixmap lifecycle: each allocation gets its own OffscreenCanvas.
  // Drawing calls that target this id land on the pixmap's ctx via
  // the same emx11_js_fill_rect / fill_arc bindings (the host
  // dispatches).
  emx11_js_pixmap_create: function (id, width, height, depth) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onPixmapCreate(id, width, height, depth);
  },

  emx11_js_pixmap_destroy: function (id) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onPixmapDestroy(id);
  },

  // XShapeCombineMask(dest_window, src_pixmap): read the pixmap's
  // ImageData, convert opaque cells into row strips, and install the
  // resulting region on the destination window.
  emx11_js_shape_combine_mask: function (destId, srcId, xOff, yOff, op) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onShapeCombineMask(destId, srcId, xOff, yOff, op);
  },

  // XCopyArea -- Host dispatches by (src, dst) pixmap/window identity.
  // The C side just flattens the Xlib call into this bridge.
  emx11_js_copy_area: function (srcId, dstId, srcX, srcY, w, h, dstX, dstY) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onCopyArea(
        srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY,
      );
  },

  // XCopyPlane -- simplified to the (depth-1 pixmap source) path Xaw and
  // Tk actually use. `applyBg` reflects gc->function + plane_mask
  // semantics that collapse to "paint unset bits with bg".
  emx11_js_copy_plane: function (srcId, dstId, srcX, srcY, w, h, dstX, dstY, plane, fg, bg, applyBg) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onCopyPlane(
        srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY,
        plane >>> 0, fg >>> 0, bg >>> 0, applyBg !== 0,
      );
  },

  // XPutImage -- C side memcpys the XImage->data slice into `dataPtr`
  // (length = bytesPerLine * height) and passes format/depth so Host
  // can pick XYBitmap vs ZPixmap decoding.
  emx11_js_put_image: function (dstId, dstX, dstY, w, h, format, depth, bytesPerLine, dataPtr, dataLen, fg, bg) {
    if (!globalThis.__EMX11__) return;
    var data =
      dataLen > 0 && dataPtr !== 0
        ? HEAPU8.slice(dataPtr, dataPtr + dataLen)
        : new Uint8Array(0);
    globalThis.__EMX11__.onPutImage(
      dstId >>> 0, dstX, dstY, w, h, format, depth, bytesPerLine,
      data, fg >>> 0, bg >>> 0,
    );
  },
});
