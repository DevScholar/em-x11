/**
 * Landing page for the em-x11 dev server. Lists available demos.
 * Real application UX is per-demo under demos/<name>/.
 */

const demos = [{ name: 'hello', description: 'Minimal window with a filled rectangle' }];

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
