/**
 * Landing page for the em-x11 dev server. Lists available demos.
 * Real application UX is per-example under examples/<name>/.
 */

const demos = [
  { name: 'hello', description: 'Minimal window with a filled rectangle' },
  { name: 'xt-hello', description: 'libXt Shell + Core child widget' },
  { name: 'xeyes', description: 'Classic eyes-track-mouse demo (SHAPE + arc drawing)' },
  { name: 'xcalc', description: 'Xaw-based calculator (single-thread Host mode)' },
  {
    name: 'twm-session',
    description:
      'twm + xeyes + xcalc together in single-thread Host mode.',
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
    a.href = `/examples/${demo.name}/`;
    a.textContent = `${demo.name} — ${demo.description}`;
    li.appendChild(a);
    list.appendChild(li);
  }
  root.appendChild(list);
}
