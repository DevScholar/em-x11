/**
 * Bulk-stage X.Org's xbitmaps shared data package into MEMFS.
 *
 * Many Xaw clients (xcalc, xterm, xfd, ...) declare resources whose
 * value is a bitmap *name* like "calculator" or "wide_weave". Xt's
 * resource type converter XmuCvtStringToBitmap then resolves the name
 * by probing a bitmap-file-path -- by default /usr/include/X11/bitmaps.
 * In a real installation that directory belongs to the xbitmaps
 * package, distributed independently of any one app.
 *
 * We mirror that distribution model: third-party/xbitmaps/ is the
 * verbatim X.Org tarball (fetched by scripts/fetch-third-party.sh).
 * This module imports every file under that tree at build time and
 * writes the actual xbm bitmaps into MEMFS at the canonical path
 * before any spawned process reads them.
 *
 * The autoconf detritus (Makefile.am, ChangeLog, configure, ...) is
 * filtered by content sniff: real xbm files always start with the
 * `#define <name>_width` line, so we drop anything that doesn't.
 */

import type { EmX11 } from '../api/emx11.js';

// eslint-disable-next-line @typescript-eslint/consistent-type-assertions
const xbmFiles = import.meta.glob('../../third-party/xbitmaps/*', {
  query: '?raw',
  import: 'default',
  eager: true,
}) as Record<string, string>;

const TARGET_DIR = '/usr/include/X11/bitmaps';

export function stageXbitmaps(emX11: EmX11): number {
  let count = 0;
  for (const [path, contents] of Object.entries(xbmFiles)) {
    if (!contents.startsWith('#define')) continue;
    const name = path.slice(path.lastIndexOf('/') + 1);
    emX11.fs.writeFileSync(`${TARGET_DIR}/${name}`, contents);
    count++;
  }
  return count;
}
