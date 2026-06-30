/*
 * em_x11_utf8.h — shared UTF-8 encoder / character counter.
 *
 * Previously duplicated across xft_bridge.c, xim_bridge.c, Text16.c,
 * TextExt16.c, and ImText.c.  Consolidated here so a fix in one place
 * (e.g. handling 4-byte sequences or overlong encodings) applies
 * everywhere.
 */
#ifndef EM_X11_UTF8_H
#define EM_X11_UTF8_H

#include <stdint.h>

/* Encode a single Unicode codepoint (≤ 0x10FFFF) into UTF-8.
 * `out` must have room for 4 bytes.  Returns the number of bytes
 * written: 1, 2, 3, or 4.  Surrogate halves (0xD800–0xDFFF) and
 * values above 0x10FFFF are replaced with U+FFFD (3 bytes). */
static inline int em_x11_utf8_encode(uint32_t cp, unsigned char out[static 4]) {
  if (cp < 0x80) {
    out[0] = (unsigned char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (unsigned char)(0xC0 | (cp >> 6));
    out[1] = (unsigned char)(0x80 | (cp & 0x3F));
    return 2;
  }
  /* Reject surrogate halves — they're not valid Unicode scalar values. */
  if (cp >= 0xD800 && cp <= 0xDFFF)
    goto replacement;
  if (cp < 0x10000) {
    out[0] = (unsigned char)(0xE0 | (cp >> 12));
    out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (unsigned char)(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp < 0x110000) {
    out[0] = (unsigned char)(0xF0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
  }
replacement:
  /* U+FFFD REPLACEMENT CHARACTER */
  out[0] = 0xEF;
  out[1] = 0xBF;
  out[2] = 0xBD;
  return 3;
}

/* Count the number of Unicode codepoints in a UTF-8 byte sequence.
 * `byte_len` is the number of bytes to examine.  Returns the count
 * of codepoints (not bytes). */
static inline int em_x11_utf8_char_count(const char* text, int byte_len) {
  int n = 0;
  for (int i = 0; i < byte_len;) {
    unsigned char c = (unsigned char)text[i];
    if (c < 0x80)
      i += 1;
    else if ((c & 0xE0) == 0xC0)
      i += 2;
    else if ((c & 0xF0) == 0xE0)
      i += 3;
    else if ((c & 0xF8) == 0xF0)
      i += 4;
    else
      i += 1; /* skip malformed byte */
    n++;
  }
  return n;
}

#endif /* EM_X11_UTF8_H */
