// History: what happened on the wall, newest first; and a board's raw log.
import { ALPHABET } from '../gen/constants.js';
import { h, fill, itemList } from './dom.js';
import { getJson } from './api.js';
import { wallLayout, boardTitle, boardId } from '../model/wall.js';
import { whenText, eventText, eventBoardId } from '../model/events.js';

// `only` is a board id, for "what happened on this board".
export function historyView(app, only) {
  const head = h('div', {});
  const list = h('div', {});
  const more = h('button', { type: 'button', class: 'btn', onclick: () => older() }, 'Show older');
  const root = h('div', { class: 'view' }, head, list, h('div', { class: 'rowwrap' }, more,
    h('a', { class: 'btn quiet', href: '#log/' + (only || '') }, 'Show the raw log instead')));
  let events = [];
  let next = null;

  function refresh() {
    const wall = app.state.wall;
    const layout = wall && wallLayout(wall);
    const title = (id) => (layout ? boardTitle(layout, id) : id);
    fill(head, only && h('div', { class: 'crumbs' }, h('a', { href: '#history' }, 'History'), ' / ', title(only)),
      h('div', { class: 'head' }, h('h1', {}, only ? `What happened on ${title(only)}` : 'History')),
      h('p', { class: 'muted' }, 'What changed on the wall, newest first.'));
    const masterId = wall ? wall.master.id : '';
    const now = new Date();
    const items = [];
    for (const event of events) {
      const id = eventBoardId(event, masterId);
      if (only && id !== only) continue;
      const text = eventText(event, id == null ? 'The master' : title(id), ALPHABET);
      const known = id != null && layout && layout.lines.some((l) => l.boards.some((b) => b.id === id));
      items.push({ ...text, when: whenText(event.time, now),
                   href: known ? (event.unit ? `#unit/${id}/${event.unit}` : '#board/' + id) : null });
    }
    fill(list, items.length ? itemList(items)
      : h('p', { class: 'muted' }, events.length ? 'Nothing about this board in what was read; show older.' : 'Nothing was recorded yet.'));
    more.hidden = next == null;
  }

  async function read() {
    try {
      const page = await getJson('/api/v2/history?limit=50');
      // What was read further back stays below the fresh first page.
      const oldest = page.events.length ? page.events[page.events.length - 1].seq : 0;
      events = page.events.concat(events.filter((e) => e.seq < oldest));
      if (next == null || events.length === page.events.length) next = page.next == null ? null : page.next;
    } catch (error) { /* the last page stays */ }
    refresh();
  }
  async function older() {
    try {
      const page = await getJson(`/api/v2/history?limit=50&before=${next}`);
      events = events.concat(page.events);
      next = page.next == null ? null : page.next;
    } catch (error) { /* try again */ }
    refresh();
  }
  read();
  return { root, refresh, historyMoved: read };
}

// A board's raw log. The answer's headers say which board and which log.
export function logView(app, only) {
  const head = h('div', { class: 'head' });
  const what = h('p', { class: 'muted small' });
  const text = h('pre', { class: 'log' });
  const board = h('select', { 'aria-label': 'Board', onchange: () => read() });
  const kind = h('select', { 'aria-label': 'Which log', onchange: () => read() },
    h('option', { value: 'ram' }, 'Since it started'),
    h('option', { value: 'flash' }, 'Kept across restarts'),
    h('option', { value: 'flash&prev=1' }, 'Kept, the file before'));
  const root = h('div', { class: 'view' },
    h('div', { class: 'crumbs' }, h('a', { href: '#history' }, 'History'), ' / Raw log'),
    head, h('div', { class: 'rowwrap' }, board, kind,
      h('button', { type: 'button', class: 'btn', onclick: () => read() }, 'Read again')),
    what, text);
  let filled = false;

  function refresh() {
    fill(head, h('h1', {}, 'Raw log'));
    const wall = app.state.wall;
    if (!wall || filled) return;
    filled = true;
    const layout = wallLayout(wall);
    fill(board, wall.rows.map((row) => {
      const id = boardId(wall, row);
      return h('option', { value: row.own ? '' : row.id, selected: id === only },
        `${boardTitle(layout, id)} (${id})`);
    }));
    read();
  }

  async function read() {
    const row = board.value;
    // Only the master keeps a log file.
    for (const option of kind.options) option.disabled = row !== '' && option.value !== 'ram';
    if (row !== '' && kind.value !== 'ram') kind.value = 'ram';
    try {
      const reply = await fetch(`/api/v2/log?kind=${kind.value}` + (row ? '&row=' + encodeURIComponent(row) : ''));
      const body = await reply.text();
      if (!reply.ok) throw new Error(body);
      what.textContent = `The ${reply.headers.get('X-Log-Kind') || '?'} log of ${reply.headers.get('X-Log-Board') || '?'}.`
        + (row ? ' A row board sends its log while it is being read: read again for the rest.' : '');
      text.textContent = body || '(empty)';
    } catch (error) {
      what.textContent = 'The log was not read: ' + error.message;
    }
  }
  return { root, refresh };
}
