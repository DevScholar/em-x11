/*
 * Color handling stub.
 *
 * em-x11 runs on a 32-bit TrueColor canvas, so the colormap indirection
 * that X uses for PseudoColor visuals is not needed. Pixel values are
 * already 0x00RRGGBB; callers can pass them straight through.
 *
 * XAllocColor, XParseColor, XAllocNamedColor will land here in v1 proper.
 */

#include "emx11_internal.h"
