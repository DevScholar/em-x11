/**
 * Landing page for the em-x11 dev server. Lists available demos.
 * Real application UX is per-demo under demos/<name>/.
 */

const demos = [
  { name: 'hello', description: 'Minimal window with a filled rectangle' },
  { name: 'xt-hello', description: 'libXt Shell + Core child widget' },
  { name: 'xeyes', description: 'Classic eyes-track-mouse demo (SHAPE + arc drawing)' },
  { name: 'xcalc', description: 'Xaw-based calculator (worker mode)' },
  { name: 'twm', description: 'Tab Window Manager, running as a standalone client' },
  {
    name: 'session',
    description:
      'twm + xeyes + xcalc together in worker mode. Each wasm client ' +
      'runs in its own Web Worker; the Server Worker owns the canvas and ' +
      'window tree (xorg-style multi-process model).',
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
