// The wall as the page draws it, from GET /api/v2/wall and the boards read so
// far. Pure.
import { letterClass, unitVerdictText, boardVerdictText } from './verdict.js';
import { listOf, plural } from './format.js';

// A board's id in a path; the wall document names the master's own row "".
export function boardId(wall, row) {
  return row.own ? wall.master.id : row.id;
}

// The board reasons under which a row shows nothing of a text sent to it.
const SHOWS_NOTHING = ['lost', 'never-seen', 'rescue', 'bus-dead'];

// Rows top to bottom, boards on one grid row left to right. Every board is a
// strip of cells at its column; a cell knows its character and its unit's
// level. `cols` is the widest row.
export function wallLayout(wall) {
  const boards = wall.rows.slice().sort((x, y) => x.row - y.row || x.col - y.col).map((row) => {
    // The text the row was told to show: it changes with the stream. What the
    // master's own flaps stand at meanwhile ("showing") changes without an event.
    const text = (row.text || '').toUpperCase();
    const levels = row.unitLevels || '';
    const cells = [];
    for (let i = 0; i < row.width; i++) {
      cells.push({ unit: i + 1, ch: text[i] || ' ', cls: letterClass(levels[i]) });
    }
    // A board that cannot show a text, by what is wrong with it.
    const reason = (row.verdict || {}).reason;
    const down = SHOWS_NOTHING.includes(reason) ? boardVerdictText(row.verdict).title.toLowerCase() : null;
    return { id: boardId(wall, row), row: row.row, col: row.col, own: row.own, cells, down };
  });
  const lines = [];
  for (const board of boards) {
    const last = lines[lines.length - 1];
    if (last && last.row === board.row) last.boards.push(board);
    else lines.push({ row: board.row, boards: [board] });
  }
  lines.forEach((line, i) => { line.title = 'Row ' + (i + 1); });
  const cols = Math.max(1, ...boards.map((b) => b.col + b.cells.length));
  return { cols, lines };
}

// What a board is called on the page: its place on the wall.
export function boardTitle(layout, id) {
  for (const line of layout.lines) {
    const at = line.boards.findIndex((b) => b.id === id);
    if (at < 0) continue;
    return line.boards.length === 1 ? line.title : line.title + ', board ' + (at + 1);
  }
  return id;
}

function unitsOf(board) {
  const table = board.units || {};
  const fields = table.fields || [];
  return (table.rows || []).map((row) => Object.fromEntries(fields.map((f, i) => [f, row[i]])));
}

// The units of one board that share a reason, as one line each.
function unitLines(cls, level, id, title, units, alphabet) {
  const byReason = new Map();
  for (const unit of units) {
    if (unit.level !== level) continue;
    if (!byReason.has(unit.reason)) byReason.set(unit.reason, []);
    byReason.get(unit.reason).push(unit);
  }
  return [...byReason.values()].map((same) => {
    const text = unitVerdictText(same[0], alphabet);
    const one = same.length === 1;
    return {
      cls,
      title: `${title}: ${one ? 'unit ' + same[0].address : plural(same.length, 'unit')}, ${text.title.toLowerCase()}`,
      why: one ? text.why : 'Units ' + listOf(same.map((u) => u.address)) + '.',
      href: one ? `#unit/${id}/${same[0].address}` : '#board/' + id,
    };
  });
}

// Faults: the boards that are in trouble themselves first, then per board
// the units that share a fault on one line. `boards` maps a board id to its
// document, for those read.
export function attentionList(wall, layout, boards, alphabet) {
  const inTrouble = [];
  const units = [];
  for (const row of wall.rows) {
    const id = boardId(wall, row);
    const title = boardTitle(layout, id);
    const verdict = row.verdict;
    if (verdict && verdict.level === 'fault' && verdict.reason !== 'units-fault') {
      const text = boardVerdictText(verdict);
      inTrouble.push({ cls: 'bad', title: `${title}: ${text.title.toLowerCase()}`, why: text.why,
                       href: '#board/' + id });
      continue;  // its units' faults follow from the board's
    }
    units.push(...unitLines('bad', 'fault', id, title, unitsOf(boards[id] || {}), alphabet));
  }
  return inTrouble.concat(units);
}

// Notes: per board, the units that share a reason on one line.
export function notesList(wall, layout, boards, alphabet) {
  return wall.rows.flatMap((row) => {
    const id = boardId(wall, row);
    return unitLines('note', 'note', id, boardTitle(layout, id), unitsOf(boards[id] || {}), alphabet);
  });
}

// What the wall is showing, from the stream's "wall" topic: {mode, quiet,
// timed, until}. A text shown for a time lies over the mode and ends by itself.
export function showingText(show) {
  if (!show) return '';
  if (show.quiet) return 'Quiet: no flap moves until you switch Quiet off. The wall keeps what it shows.';
  if (show.timed) {
    const at = show.until && new Date(show.until * 1000);
    const two = (n) => String(n).padStart(2, '0');
    return `Showing a text ${at ? `until ${two(at.getHours())}:${two(at.getMinutes())}` : 'for a time'}, then `
      + `${show.mode === 'clock' ? 'the clock' : 'the text before it'} again.`;
  }
  return show.mode === 'clock' ? 'Showing the clock.' : 'Showing a text.';
}

// One line a board for the Boards list.
export function boardLine(wall, layout, row) {
  const id = boardId(wall, row);
  const text = boardVerdictText(row.verdict || { level: '', reason: 'units-unknown' });
  const facts = [];
  const rev = row.own ? wall.master.rev : row.rev;
  if (rev) facts.push('Firmware ' + rev);
  if (!row.own && row.status && row.status.rssi != null) facts.push(`WiFi ${row.status.rssi} dBm`);
  return { id, title: boardTitle(layout, id), kind: row.own ? 'master' : 'row board',
           cls: text.cls, verdict: text.title, why: [text.why].concat(facts).filter(Boolean).join(' '),
           href: '#board/' + id };
}

// The characters of `text` the drum has no flap for, each once.
export function noFlapFor(text, alphabet) {
  const missing = [];
  for (const ch of text.toUpperCase()) {
    if (!alphabet.includes(ch) && !missing.includes(ch)) missing.push(ch);
  }
  return missing;
}

// What the compose form needs to know of the wall: a line a grid row, as
// wide as the wall is there. Compared as text to see whether the form must
// be built again.
export function composeLines(layout) {
  return layout.lines.map((line) => ({
    title: line.title,
    width: Math.max(...line.boards.map((b) => b.col + b.cells.length)),
  }));
}

// For each line of the compose form, what to say about a board of that row
// that will not show the text ('' when every board can).
export function composeNotes(layout) {
  return layout.lines.map((line) => line.boards.filter((b) => b.down).map((board) => {
    const name = line.boards.length === 1 ? line.title : `${line.title}, board ${line.boards.indexOf(board) + 1}`;
    return `${name}: ${board.down}. A text does not show there.`;
  }).join(' '));
}

// The text a grid row was told to show, as the compose form takes it.
export function lineText(line) {
  const width = Math.max(...line.boards.map((b) => b.col + b.cells.length));
  const chars = new Array(width).fill(' ');
  for (const board of line.boards) board.cells.forEach((cell, i) => { chars[board.col + i] = cell.ch; });
  return chars.join('').trimEnd();
}

// The show action's text: one line a row, top to bottom, without the empty
// lines at the end.
export function composeText(values) {
  const lines = values.slice();
  while (lines.length && lines[lines.length - 1].trim() === '') lines.pop();
  return lines.join('\n');
}
