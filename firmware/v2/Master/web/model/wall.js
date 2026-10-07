// The wall as the page draws it, from GET /api/v2/wall and the boards read so
// far. Pure.
import { letterClass, unitVerdictText, boardVerdictText } from './verdict.js';
import { listOf, volt, plural } from './format.js';

// A board's id in a path; the wall document names the master's own row "".
export function boardId(wall, row) {
  return row.own ? wall.master.id : row.id;
}

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
    return { id: boardId(wall, row), row: row.row, col: row.col, own: row.own, cells };
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

// Faults: a board that is in trouble itself, or else its units with a fault. `boards` maps a board id to its document, for those read.
export function attentionList(wall, layout, boards, alphabet) {
  const items = [];
  for (const row of wall.rows) {
    const id = boardId(wall, row);
    const title = boardTitle(layout, id);
    const verdict = row.verdict;
    if (verdict && verdict.level === 'fault' && verdict.reason !== 'units-fault') {
      const text = boardVerdictText(verdict);
      items.push({ cls: 'bad', title: `${title}: ${text.title.toLowerCase()}`, why: text.why,
                   href: '#board/' + id });
      continue;  // its units' faults follow from the board's
    }
    for (const unit of unitsOf(boards[id] || {})) {
      if (unit.level !== 'fault') continue;
      const text = unitVerdictText(unit, alphabet);
      items.push({ cls: 'bad', title: `${title}, unit ${unit.address}: ${text.title.toLowerCase()}`,
                   why: text.why, href: `#unit/${id}/${unit.address}` });
    }
  }
  return items;
}

// Notes: per board, the units that share a reason on one line; then the
// lowest supply seen on the wall.
export function notesList(wall, layout, boards, alphabet) {
  const items = [];
  let lowest = null;
  for (const row of wall.rows) {
    const id = boardId(wall, row);
    const title = boardTitle(layout, id);
    const byReason = new Map();
    for (const unit of unitsOf(boards[id] || {})) {
      if (unit.supplyMinMv != null && (!lowest || unit.supplyMinMv < lowest.mv)) {
        lowest = { mv: unit.supplyMinMv, id, title, address: unit.address };
      }
      if (unit.level !== 'note') continue;
      if (!byReason.has(unit.reason)) byReason.set(unit.reason, []);
      byReason.get(unit.reason).push(unit);
    }
    for (const units of byReason.values()) {
      const text = unitVerdictText(units[0], alphabet);
      const one = units.length === 1;
      items.push({
        cls: 'note',
        title: `${title}: ${one ? 'unit ' + units[0].address : plural(units.length, 'unit')}, ${text.title.toLowerCase()}`,
        why: one ? text.why : 'Units ' + listOf(units.map((u) => u.address)) + '.',
        href: one ? `#unit/${id}/${units[0].address}` : '#board/' + id,
      });
    }
  }
  if (lowest) {
    items.push({ cls: 'info', title: 'Lowest supply seen: ' + volt(lowest.mv),
                 why: `${lowest.title}, unit ${lowest.address}.`,
                 href: `#unit/${lowest.id}/${lowest.address}` });
  }
  return items;
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

// The show action's text: one line a row, top to bottom, without the empty
// lines at the end.
export function composeText(values) {
  const lines = values.slice();
  while (lines.length && lines[lines.length - 1].trim() === '') lines.pop();
  return lines.join('\n');
}
