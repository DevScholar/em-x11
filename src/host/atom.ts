/**
 * Atom table. Mirrors xserver/dix/atom.c: a single global name↔id map
 * shared across all clients, so XInternAtom from any wasm module
 * resolves the same name to the same id.
 *
 * Predefined atoms 1..68 are still resolved locally in C (native/src/
 * atom.c) for zero round-trip cost on hot paths. Anything beyond 68
 * goes through this manager so cross-module interning agrees -- the
 * fix for the WM_PROTOCOLS / WM_DELETE_WINDOW divergence we saw with
 * per-module tables. Counter starts at 69 (one past the last
 * predefined, which is XA_WM_TRANSIENT_FOR = 68).
 */

export class AtomManager {
  private readonly atomsByName = new Map<string, number>();
  private readonly atomsById = new Map<number, string>();
  private nextAtom = 69;

  /** XInternAtom for atoms beyond the predefined 1..68 range (which C
   *  resolves locally). Allocates a fresh id on first sight of a name;
   *  subsequent calls from any connection return the same id. Returns
   *  0 (None) when onlyIfExists is true and the name has never been
   *  seen before. */
  intern(name: string, onlyIfExists: boolean): number {
    const hit = this.atomsByName.get(name);
    if (hit !== undefined) return hit;
    if (onlyIfExists) return 0;
    const id = this.nextAtom++;
    this.atomsByName.set(name, id);
    this.atomsById.set(id, name);
    return id;
  }

  /** XGetAtomName for Host-allocated atoms (id >= 69). Returns null for
   *  unknown ids; caller (bindings/atom.js) surfaces that as a NULL
   *  return from XGetAtomName, which Xlib docs define as BadAtom. */
  nameOf(atom: number): string | null {
    return this.atomsById.get(atom) ?? null;
  }
}
