// Wall settings: the rows as the operator arranges them, the time zone by
// its name, the broker in a line. Pure.
import { boardId } from './wall.js';
import { plural } from './format.js';

// A line a board, top to bottom and left to right. Row and column count from
// 1, as the page shows them; `id` is the board's name in an action ("" for
// the master's own row).
export function rowsDraft(wall) {
  return wall.rows.slice().sort((x, y) => x.row - y.row || x.col - y.col).map((row) => ({
    id: row.own ? '' : row.id, name: boardId(wall, row), own: !!row.own,
    row: row.row + 1, col: row.col + 1, width: row.width,
  }));
}

// Why the rows cannot be saved as entered, '' when they can. Two boards may
// share a row side by side, or show the same text at the same place.
export function arrangeProblem(draft) {
  for (const line of draft) {
    for (const [what, value] of [['row', line.row], ['column', line.col]]) {
      if (!Number.isInteger(value) || value < 1) return `${line.name}: the ${what} is a whole number from 1.`;
    }
  }
  for (const a of draft) {
    for (const b of draft) {
      if (a === b || a.row !== b.row || (a.col === b.col && a.width === b.width)) continue;
      if (a.col < b.col + b.width && b.col < a.col + a.width) {
        return `${a.name} and ${b.name} would overlap on row ${a.row}.`;
      }
    }
  }
  return '';
}

// The arrange action's args: rows counted from 0 without a gap, in the order
// of the numbers entered; columns from 0.
export function arrangeArgs(draft) {
  const order = [...new Set(draft.map((line) => line.row))].sort((a, b) => a - b);
  return { rows: draft.map((line) => (
    { id: line.id, row: order.indexOf(line.row), col: line.col - 1, width: line.width })) };
}

// The zone to show for the rule the wall keeps (zones: name -> rule). Many
// zones share a rule: the browser's own zone when it has this one, else the
// first that has. '' when no zone has it.
export function zoneFor(zones, rule, browserZone) {
  if (zones[browserZone] === rule) return browserZone;
  return Object.keys(zones).find((name) => zones[name] === rule) || '';
}

// The Home Assistant line. `connected` is undefined until the master's own
// document was read.
export function brokerText(mqtt, connected) {
  if (!mqtt.host) {
    return { cls: 'note', title: 'Not set up', why: 'No broker is set: Home Assistant does not know this wall.' };
  }
  const where = `${mqtt.host}, port ${mqtt.port}` + (mqtt.user ? `, user ${mqtt.user}` : '');
  if (connected == null) return { cls: 'note', title: 'Checking', why: where };
  return connected ? { cls: 'ok', title: 'Connected', why: where }
                   : { cls: 'bad', title: 'Not connected', why: where };
}

// A row board a search found, as a line.
export function foundLine(board) {
  return { title: board.id,
           why: [board.units ? plural(board.units, 'unit') : null,
                 board.rev ? 'firmware ' + board.rev : null, 'at ' + board.address].filter(Boolean).join(', ') };
}
