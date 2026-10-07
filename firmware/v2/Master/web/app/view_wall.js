// The Wall: the picture of the wall, what needs attention, the boards, and
// what is worth knowing.
import { ALPHABET } from '../gen/constants.js';
import { h, fill, pill, itemList } from './dom.js';
import { wallLayout, attentionList, notesList, boardLine } from '../model/wall.js';
import { wallVerdictText } from '../model/verdict.js';
import { plural } from '../model/format.js';

// The wall as flaps. A row's name opens its board, a flap its unit.
export function wallPicture(layout) {
  return h('div', { class: 'wall' }, layout.lines.map((line) =>
    h('div', {},
      h('div', { class: 'rowlabel' }, line.boards.map((board, i) =>
        h('a', { href: '#board/' + board.id },
          line.boards.length === 1 ? line.title : `${line.title}, board ${i + 1}`)),
        h('span', {}, plural(line.boards.reduce((n, b) => n + b.cells.length, 0), 'unit'))),
      h('div', { class: 'wallrow', style: `--cols:${layout.cols}` }, line.boards.map((board) =>
        board.cells.map((cell, i) =>
          h('a', { class: 'flap ' + cell.cls, href: `#unit/${board.id}/${cell.unit}`,
                   style: i === 0 ? `grid-column-start:${board.col + 1}` : null,
                   'aria-label': `${line.title} unit ${cell.unit}` },
            h('span', {}, cell.cls === 'bad' ? '' : cell.ch))))))));
}

// state.show is the stream's "wall" topic: {mode, quiet, rows}.
function showingText(show) {
  if (!show) return '';
  if (show.quiet) return 'Quiet: the flaps stand still.';
  return show.mode === 'clock' ? 'Showing the clock.' : 'Showing text.';
}

export function wallView(app) {
  const head = h('div', { class: 'head' });
  const picture = h('div', {});
  const attention = h('div', { class: 'section' });
  const boards = h('div', { class: 'section' });
  const notes = h('div', { class: 'section' });
  const root = h('div', { class: 'view' }, head, picture, attention, boards, notes);

  function refresh() {
    const wall = app.state.wall;
    if (!wall) {
      fill(head, h('h1', {}, 'Wall'), h('span', { class: 'muted small' }, 'Reading the wall…'));
      return;
    }
    const layout = wallLayout(wall);
    const needs = attentionList(wall, layout, app.state.boards, ALPHABET);
    const verdict = wallVerdictText(wall.verdict, needs.length);
    fill(head, h('h1', {}, 'Wall'), pill(verdict.cls, verdict.title),
      h('span', { class: 'muted small' },
        showingText(app.state.show)));
    fill(picture, wallPicture(layout));
    fill(attention, needs.length ? [h('h2', {}, 'Needs attention'), itemList(needs)] : null);
    fill(boards, h('h2', {}, 'Boards'), itemList(wall.rows.map((row) => {
      const line = boardLine(wall, layout, row);
      return { cls: line.cls, href: line.href, why: line.why, pill: pill(line.cls, line.verdict),
               title: [line.title, ' ', h('span', { class: 'muted plain' }, line.kind)] };
    })));
    const worth = notesList(wall, layout, app.state.boards, ALPHABET);
    fill(notes, worth.length ? [h('h2', {}, 'Worth knowing'), itemList(worth)] : null);
  }

  return { root, refresh, boardsWanted: () => (app.state.wall ? app.state.wall.rows : []) };
}
