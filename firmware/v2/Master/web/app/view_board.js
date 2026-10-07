// A board: its units, their supply, what the board says about itself, the
// jobs on its row, and (the master) its settings.
import { ALPHABET } from '../gen/constants.js';
import { h, fill, pill, statusLine, segmented } from './dom.js';
import { getJson } from './api.js';
import { runJob, targetRow } from './jobs.js';
import { wallLayout, boardTitle } from '../model/wall.js';
import { boardVerdictText, levelClass } from '../model/verdict.js';
import { boardUnits, tileLines, supplyBars, sparkPoints, boardFacts, startMarks,
         SUPPLY_FLOOR_MV, SUPPLY_TOP_MV } from '../model/board.js';
import { volt } from '../model/format.js';

const SVG = 'http://www.w3.org/2000/svg';

function spark(values, label) {
  const svg = document.createElementNS(SVG, 'svg');
  svg.setAttribute('class', 'spark');
  svg.setAttribute('viewBox', '0 0 300 40');
  svg.setAttribute('preserveAspectRatio', 'none');
  svg.setAttribute('role', 'img');
  svg.setAttribute('aria-label', label);
  const line = document.createElementNS(SVG, 'polyline');
  line.setAttribute('points', sparkPoints(values, 300, 40));
  line.setAttribute('fill', 'none');
  line.setAttribute('stroke', 'currentColor');
  line.setAttribute('stroke-width', '1.5');
  line.setAttribute('vector-effect', 'non-scaling-stroke');
  svg.append(line);
  return svg;
}

export function factGroups(groups, extra) {
  return h('div', { class: 'facts' }, groups.map((group) =>
    h('dl', {}, h('h3', {}, group.title),
      group.rows.map(([label, value, why]) => [
        h('dt', {}, label), h('dd', {}, value), why && h('div', { class: 'why' }, why)]),
      extra && extra[group.title])));
}

// The master's own settings. Kept across refreshes while it is being edited.
function settingsForm(id, settings) {
  const status = statusLine();
  const name = h('input', { type: 'text', id: 'boardName', value: settings.name,
                            placeholder: id, maxlength: 31, autocomplete: 'off' });
  const fixed = h('input', { type: 'number', id: 'unitCount', min: 1, max: 16,
                             value: settings.unitCount || '', 'aria-label': 'Number of units' });
  const counting = segmented('Number of units', [[true, 'Count them'], [false, 'Fix at']],
    (on) => { counting.set(on); fixed.disabled = on; });
  counting.set(!settings.unitCount);
  fixed.disabled = !settings.unitCount;
  const save = async (event) => {
    event.preventDefault();
    const body = { name: name.value.trim(), unitCount: fixed.disabled ? 0 : Number(fixed.value) };
    try {
      const reply = await fetch('/api/v2/settings/board/' + encodeURIComponent(id), {
        method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      const answer = await reply.json().catch(() => ({}));
      if (!reply.ok) throw new Error(answer.error || `The master refused (${reply.status}).`);
      status.say(answer.restart ? 'Saved. It takes effect when the board restarts.' : 'Saved.');
    } catch (error) {
      status.say(error.message, true);
    }
  };
  return h('details', {}, h('summary', {}, 'Settings for this board'),
    h('form', { class: 'in', onsubmit: save },
      h('div', { class: 'field' }, h('label', { for: 'boardName' }, 'Name'), name),
      h('div', { class: 'rowwrap' }, h('span', {}, 'Number of units'), counting, fixed),
      h('div', { class: 'rowwrap' }, h('button', { type: 'submit', class: 'btn' }, 'Save')),
      status));
}

export function boardView(app, id) {
  const top = h('div', {});
  const body = h('div', { class: 'view-part' });
  const status = statusLine();
  const actions = h('div', { class: 'rowwrap' });
  const settings = h('div', {});
  const root = h('div', { class: 'view' }, top, body, actions, status, settings);
  let settingsFor = null;

  const job = (doing, name, sure) => () => {
    if (sure && !window.confirm(sure)) return;
    runJob(status, doing, name, targetRow(app, id));
  };

  function refresh() {
    const wall = app.state.wall;
    const board = app.state.boards[id];
    const title = wall ? boardTitle(wallLayout(wall), id) : id;
    const crumbs = h('div', { class: 'crumbs' }, h('a', { href: '#wall' }, 'Wall'), ' / ', title);
    if (!board) {
      fill(top, crumbs, h('div', { class: 'head' }, h('h1', {}, title),
        h('span', { class: 'muted small' }, app.state.boardMissing === id ? 'This wall has no such board.' : 'Reading the board…')));
      return;
    }
    const verdict = boardVerdictText(board.verdict || { level: '', reason: 'units-unknown' });
    fill(top, crumbs,
      h('div', { class: 'head' }, h('h1', {}, title), pill(verdict.cls, verdict.title),
        h('span', { class: 'muted small' },
          [board.id, board.kind === 'master' ? 'master' : 'row board', board.address].filter(Boolean).join(', '))),
      h('p', { class: 'muted' }, verdict.why));

    const units = boardUnits(board);
    const supply = supplyBars(units);
    // A place of the ring that was not filled yet reads 0.
    const rssi = ((board.stats && board.stats.hist && board.stats.hist.rssi) || []).filter((v) => v !== 0);
    const marks = startMarks(board);
    fill(body,
      h('div', { class: 'section' }, h('h2', {}, 'Units'),
        units.length
          ? h('div', { class: 'tiles' }, units.map((unit) =>
              h('a', { class: 'tile ' + levelClass(unit.level), href: `#unit/${id}/${unit.address}` },
                h('b', {}, 'Unit ' + unit.address),
                tileLines(unit, ALPHABET).map((line) => h('span', {}, line)))))
          : h('p', { class: 'muted small' }, 'The master has no unit facts of this board.')),
      supply.bars.length > 0 && h('div', { class: 'section' }, h('h2', {}, 'Supply voltage per unit'),
        h('div', { class: 'bars' }, supply.bars.map((bar) =>
          h('i', { class: bar.lowest ? 'low' : null, style: `height:${bar.percent}%`,
                   title: `Unit ${bar.address}: lowest ${volt(bar.mv)}` }))),
        h('div', { class: 'axis' },
          h('span', {}, 'Unit ' + supply.bars[0].address),
          h('span', {}, `Lowest seen since each unit started, ${volt(SUPPLY_FLOOR_MV)} to ${volt(SUPPLY_TOP_MV)}. Lowest here ${volt(supply.lowestMv)}, marked dark.`),
          h('span', {}, 'Unit ' + supply.bars[supply.bars.length - 1].address))),
      factGroups(boardFacts(board, units), {
        Connection: rssi.length > 1 && h('div', { class: 'why' },
          spark(rssi, 'WiFi signal, last 10 minutes'),
          h('div', { class: 'axis' },
            h('span', {}, `10 min ago, ${Math.min(...rssi)} to ${Math.max(...rssi)} dBm`), h('span', {}, 'now'))),
        Running: marks.length > 0 && [
          h('dt', {}, `Last ${marks.length} starts`),
          h('dd', {}, h('span', { class: 'boots' }, marks.map((m) => h('i', { class: m.cls, title: m.title })))),
          h('div', { class: 'why' }, 'Green: restarted by software. Amber: power was cut. Red: anything else.')],
      }));

    const busy = !!board.jobRunning;
    fill(actions,
      h('button', { type: 'button', class: 'btn', disabled: busy,
                    onclick: job('Finding home on all units', 'home-all') }, 'Find home on all units'),
      h('button', { type: 'button', class: 'btn', disabled: busy,
                    onclick: job('Updating the units that need it', 'update-units',
                      'Update every unit of this row that runs other firmware? The row shows nothing meanwhile.') },
        'Update units'),
      h('button', { type: 'button', class: 'btn',
                    onclick: job('Restarting the board', 'restart', 'Restart this board?') }, 'Restart board'),
      h('a', { class: 'btn quiet', href: '#history/' + id }, 'What happened on this board'),
      h('a', { class: 'btn quiet', href: '#log/' + id }, 'Raw log'),
      busy && h('span', { class: 'muted small' }, 'A job is running on this row.'));

    if (board.kind === 'master' && board.settings && settingsFor !== id) {
      settingsFor = id;
      fill(settings, settingsForm(id, board.settings));
    }
  }

  // Read again whenever the board's part of the wall changes, and once now.
  async function read() {
    try {
      app.state.boards[id] = await getJson('/api/v2/board/' + encodeURIComponent(id));
    } catch (error) {
      app.state.boardMissing = id;
    }
    refresh();
  }
  read();

  return { root, refresh, reread: read };
}
