/**
 * Landing page for the em-x11 dev server. Lists available demos.
 * Real application UX is per-demo under demos/<name>/.
 */

const demos = [
  { name: 'hello', description: 'Minimal window with a filled rectangle' },
  { name: 'xt-hello', description: 'libXt Shell + Core child widget' },
  { name: 'xaw-hello', description: 'libXaw Label widget on top of Xt' },
  { name: 'xeyes', description: 'Classic eyes-track-mouse demo (SHAPE + arc drawing)' },
  {
    name: 'twm',
    description:
      'Tab Window Manager — BUGGY. Loads and registers redirect on root, ' +
      'but managed-client framing has known gaps (Expose timing across ' +
      'connections, atom divergence, no GXxor); see README.',
  },
  {
    name: 'session',
    description:
      'twm + xeyes together — also BUGGY (same gaps as the twm demo). ' +
      'Useful to see redirect dispatch, frame-of-xeyes attempt, and the ' +
      'remaining DIX-alignment gaps in action.',
  },
];

const root = document.getElementById('app');
if (root) {
  const title = document.createElement('h1');
  title.textContent = 'em-x11 demos';
  root.appendChild(title);

  const list = document.createElement('ul');
  for (const demo of demos) {
    const li = document.createElement('li');
    const a = document.createElement('a');
    a.href = `/demos/${demo.name}/`;
    a.textContent = `${demo.name} — ${demo.description}`;
    li.appendChild(a);
    list.appendChild(li);
  }
  root.appendChild(list);
}
