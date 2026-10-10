// The Wall: the picture of the wall, what needs attention, the boards, and
// what is worth knowing.
import { ALPHABET } from '../gen/constants.js';
import { h, fill, pill, itemList, segmented, statusLine } from './dom.js';
import { action } from './api.js';
import { wallLayout, attentionList, notesList, boardLine, noFlapFor, composeLines, composeText, showingText,
         composeNotes, lineText } from '../model/wall.js';
import { plural } from '../model/format.js';

// The wall as flaps. A row's name opens its board, a flap its unit; with
// `flap(board, cell, attrs)` the caller builds each flap itself.
export function wallPicture(layout, flap) {
  return h('div', { class: 'wall' }, layout.lines.map((line) =>
    h('div', {},
      h('div', { class: 'rowlabel' }, line.boards.map((board, i) =>
        h('a', { href: '#board/' + board.id },
          line.boards.length === 1 ? line.title : `${line.title}, board ${i + 1}`,
          board.down && h('b', {}, ' ' + board.down))),
        h('span', {}, plural(line.boards.reduce((n, b) => n + b.cells.length, 0), 'unit'))),
      h('div', { class: 'wallrow', style: `--cols:${layout.cols}` }, line.boards.map((board) =>
        board.cells.map((cell, i) => {
          const attrs = { class: 'flap ' + cell.cls + (board.down ? ' down' : ''),
                          style: i === 0 ? `grid-column-start:${board.col + 1}` : null,
                          'aria-label': `${line.title} unit ${cell.unit}` };
          return flap ? flap(board, cell, attrs)
            : h('a', { ...attrs, href: `#unit/${board.id}/${cell.unit}` },
                h('span', {}, cell.cls === 'bad' ? '' : cell.ch));
        }))))));
}

const DURATIONS = [['', 'Until I change it'], ['300', '5 minutes, then back'],
                   ['900', '15 minutes, then back'], ['3600', '1 hour, then back']];

// Text per row, for how long, mode, quiet, stop. Built once for a shape of
// the wall and kept across refreshes, so typing is never interrupted.
// `texts` is what the rows show when a text is up: the boxes open with it.
function composeForm(app, lines, texts) {
  const status = statusLine();
  const ask = (name, args, done) => action(name, null, args)
    .then(() => status.say(done), (error) => status.say(error.message, true));
  const inputs = lines.map((line, i) => {
    const value = (texts && texts[i]) || '';
    const count = h('span', { class: 'count' }, `${value.length} / ${line.width}`);
    const note = h('div', { class: 'why bad' });
    const input = h('input', {
      type: 'text', id: 'compose' + i, maxlength: line.width, autocomplete: 'off', value,
      placeholder: `up to ${line.width} characters`,
      oninput: () => {
        count.textContent = `${input.value.length} / ${line.width}`;
        const missing = noFlapFor(inputs.map((x) => x.input.value).join(''), ALPHABET);
        status.say(missing.length ? 'The wall has no flap for: ' + missing.join(' ') : '');
      },
    });
    return { input, note,
             row: [h('div', { class: 'field' }, h('label', { for: 'compose' + i }, line.title), input, count), note] };
  });
  const length = h('select', { 'aria-label': 'How long' },
    DURATIONS.map(([value, text]) => h('option', { value }, text)));
  const show = (event) => {
    event.preventDefault();
    const args = { text: composeText(inputs.map((x) => x.input.value)) };
    if (length.value) args.forS = Number(length.value);
    ask('show', args, 'Sent to the wall.');
  };
  const mode = segmented('Mode', [['clock', 'Clock'], ['text', 'Text']],
    (value) => ask('mode', { mode: value }, value === 'clock' ? 'Back to the clock.' : 'Showing the text.'));
  const quiet = segmented('Quiet', [[false, 'Flaps on'], [true, 'Quiet']],
    (on) => ask('quiet', { on }, on ? 'Quiet: the flaps stand still.' : 'The flaps move again.'));
  const root = h('form', { class: 'compose', onsubmit: show },
    inputs.map((x) => x.row),
    h('div', { class: 'rowwrap' }, length,
      h('button', { type: 'submit', class: 'btn primary' }, 'Show on the wall'),
      h('span', { class: 'spacer' }), mode, quiet,
      h('button', { type: 'button', class: 'btn danger',
                    onclick: () => ask('stop', null, 'Stopped and blanked.') }, 'Stop and blank')),
    status);
  root.set = (show, notes) => {
    inputs.forEach((x, i) => { x.note.textContent = notes[i] || ''; });
    if (!show) return;
    mode.set(show.mode);
    quiet.set(!!show.quiet);
  };
  return root;
}

export function wallView(app) {
  const head = h('div', { class: 'head' });
  const calibrate = h('a', { class: 'btn', href: '#calibrate', hidden: true }, 'Calibrate the wall');
  const picture = h('div', {});
  const compose = h('div', {});
  let composeShape = '';
  let form = null;
  const found = h('div', {});
  const attention = h('div', { class: 'section' });
  const boards = h('div', { class: 'section' });
  const notes = h('div', { class: 'section' });
  const root = h('div', { class: 'view' }, h('div', { class: 'rowwrap' }, head, h('span', { class: 'spacer' }), calibrate),
    picture, compose, found, attention, boards, notes);

  function refresh() {
    const wall = app.state.wall;
    if (!wall) {
      fill(head, h('h1', {}, 'Wall'), h('span', { class: 'muted small' }, 'Reading the wall…'));
      return;
    }
    const layout = wallLayout(wall);
    const needs = attentionList(wall, layout, app.state.boards, ALPHABET);
    const show = app.state.show;
    fill(head, h('h1', {}, 'Wall'), h('span', { class: 'muted small' }, showingText(show)));
    calibrate.hidden = false;
    fill(picture, wallPicture(layout));
    const lines = composeLines(layout);
    if (JSON.stringify(lines) !== composeShape) {
      composeShape = JSON.stringify(lines);
      // The clock's own text is not something to send again.
      const up = show && (show.timed || show.mode === 'text');
      form = composeForm(app, lines, up ? layout.lines.map(lineText) : null);
      fill(compose, form);
    }
    form.set(show, composeNotes(layout));
    fill(found, wall.release ? itemList([{ cls: 'note', href: '#firmware',
      title: `New release ${wall.release} found`, why: 'Firmware has what it would change, and the button.' }]) : null);
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
