// The page's side of editing in place. It is added to pages only by `npm run dev`.
(() => {
  if (window.__editInPlace) return;
  window.__editInPlace = true;
  const REMEMBER = 'edit-in-place';
  let on = sessionStorage.getItem(REMEMBER) === '1';

  const style = document.createElement('style');
  style.textContent = `
    .eip-toggle { position: fixed; right: 1rem; bottom: 1rem; z-index: 1000; font: 600 0.9rem/1 var(--sl-font, sans-serif);
      padding: 0.7rem 1rem; border-radius: 999px; border: 1px solid var(--sl-color-accent); cursor: pointer;
      background: var(--sl-color-black); color: var(--sl-color-white); box-shadow: 0 2px 10px rgb(0 0 0 / 0.4); }
    .eip-toggle[aria-pressed="true"] { background: var(--sl-color-accent); color: var(--sl-color-black); }
    body.eip-on :is(.sl-markdown-content :is(p, h2, h3, h4, li, td, th), [data-eip-key], .hero .actions a) { outline: 1px dashed transparent; outline-offset: 3px; cursor: text; }
    body.eip-on :is(.sl-markdown-content :is(p, h2, h3, h4, li, td, th), [data-eip-key], .hero .actions a):hover { outline-color: var(--sl-color-accent); }
    .eip-panel { position: fixed; left: 50%; bottom: 1rem; transform: translateX(-50%); z-index: 1001; width: min(52rem, calc(100vw - 2rem));
      background: var(--sl-color-gray-6); color: var(--sl-color-white); border: 1px solid var(--sl-color-accent);
      border-radius: 0.5rem; padding: 0.9rem; box-shadow: 0 6px 30px rgb(0 0 0 / 0.5); font: 0.9rem/1.4 var(--sl-font, sans-serif); }
    .eip-panel textarea { width: 100%; min-height: 8rem; resize: vertical; box-sizing: border-box; padding: 0.6rem;
      font: 0.9rem/1.5 var(--sl-font-mono, monospace); color: var(--sl-color-white); background: var(--sl-color-black);
      border: 1px solid var(--sl-color-gray-5); border-radius: 0.3rem; }
    .eip-row { display: flex; gap: 0.6rem; align-items: center; margin-top: 0.6rem; }
    .eip-row button { font: inherit; font-weight: 600; padding: 0.45rem 0.9rem; border-radius: 0.3rem; cursor: pointer;
      border: 1px solid var(--sl-color-gray-4); background: var(--sl-color-black); color: var(--sl-color-white); }
    .eip-row button.eip-save { background: var(--sl-color-accent); border-color: var(--sl-color-accent); color: var(--sl-color-black); }
    .eip-note { flex: 1; color: var(--sl-color-gray-2); }
    .eip-note.eip-bad { color: #ff8a80; }
  `;
  document.head.append(style);

  const toggle = document.createElement('button');
  toggle.className = 'eip-toggle';
  toggle.type = 'button';
  const show = () => {
    toggle.textContent = on ? 'Editing text: on' : 'Edit text';
    toggle.setAttribute('aria-pressed', String(on));
    document.body.classList.toggle('eip-on', on);
  };
  toggle.addEventListener('click', () => {
    on = !on;
    sessionStorage.setItem(REMEMBER, on ? '1' : '0');
    show();
    if (!on) close();
  });
  document.body.append(toggle);

  // The page's title and a splash page's tagline are in the file's settings, at its top.
  const BLOCKS = 'p, h2, h3, h4, li, td, th';
  const mark = () => {
    const title = document.querySelector('h1#_top');
    if (title) title.dataset.eipKey = 'title';
    const tagline = document.querySelector('.hero .tagline');
    if (tagline) tagline.dataset.eipKey = 'tagline';
  };
  const ask = async (sent) =>
    (await fetch('/__edit', { method: 'POST', body: JSON.stringify({ page: location.pathname, ...sent }) })).json();

  let panel = null;
  function close() {
    if (panel) panel.remove();
    panel = null;
  }

  async function open(target) {
    close();
    const key = target.dataset.eipKey;
    const drawn = target.innerText;
    const got = await ask(key ? { key } : { drawn });
    panel = document.createElement('div');
    panel.className = 'eip-panel';
    const area = document.createElement('textarea');
    area.value = got.text || '';
    area.disabled = Boolean(got.error);
    const row = document.createElement('div');
    row.className = 'eip-row';
    const note = document.createElement('span');
    note.className = 'eip-note' + (got.error ? ' eip-bad' : '');
    note.textContent = got.error || (key ? `The page's ${key}.` : 'Markdown. Ctrl+Enter saves, Esc cancels.');
    const cancel = document.createElement('button');
    cancel.type = 'button';
    cancel.textContent = got.error ? 'Close' : 'Cancel';
    cancel.addEventListener('click', close);
    const save = document.createElement('button');
    save.type = 'button';
    save.className = 'eip-save';
    save.textContent = 'Save';
    const send = async () => {
      save.disabled = true;
      const answer = await ask(key ? { key, text: area.value } : { drawn, was: got.text, text: area.value });
      if (answer.error) {
        note.textContent = answer.error;
        note.className = 'eip-note eip-bad';
        save.disabled = false;
        return;
      }
      note.textContent = 'Saved.';
      setTimeout(() => location.reload(), 350);
    };
    save.addEventListener('click', send);
    area.addEventListener('keydown', (event) => {
      if (event.key === 'Escape') close();
      if (event.key === 'Enter' && (event.ctrlKey || event.metaKey)) send();
    });
    row.append(note, cancel);
    if (!got.error) row.append(save);
    panel.append(area, row);
    document.body.append(panel);
    area.focus();
  }

  document.addEventListener(
    'click',
    (event) => {
      if (!on || event.target.closest('.eip-panel, .eip-toggle')) return;
      const inPage = event.target.closest('.sl-markdown-content');
      const target =
        event.target.closest('[data-eip-key]') || event.target.closest('.hero .actions a') || (inPage && event.target.closest(BLOCKS));
      if (!target) return;
      event.preventDefault();
      event.stopPropagation();
      open(target);
    },
    true,
  );

  mark();
  show();
})();
