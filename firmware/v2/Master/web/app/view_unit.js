// A unit: what it shows, what it says about itself, and the jobs on it.
import { ALPHABET, OFFSET_LIMIT_STEPS } from '../gen/constants.js';
import { h, fill, pill, statusLine } from './dom.js';
import { getJson } from './api.js';
import { runJob, targetRow } from './jobs.js';
import { factGroups } from './view_board.js';
import { wallLayout, boardTitle } from '../model/wall.js';
import { unitVerdictText } from '../model/verdict.js';
import { flapName } from '../model/format.js';
import { showsText, unitFacts, correctedOffset, selfTestText } from '../model/unit.js';

// One turn of the drum, in steps: also the furthest an offset can go.
const STEPS_PER_TURN = OFFSET_LIMIT_STEPS;

function flapSelect(label, selected) {
  return h('select', { 'aria-label': label }, [...ALPHABET].map((ch, place) =>
    h('option', { value: place, selected: place === selected }, flapName(ALPHABET, place))));
}

export function unitView(app, id, addressText) {
  const address = Number(addressText);
  const top = h('div', {});
  const facts = h('div', {});
  const status = statusLine();
  const lineUp = h('div', {});
  let unit = null;
  let missing = false;

  const target = () => targetRow(app, id, address);
  const job = (doing, name, args, sure) => {
    if (sure && !window.confirm(sure)) return Promise.resolve(null);
    return runJob(status, doing, name, target(), args);
  };

  async function selfTest() {
    const before = app.state.jobs.length ? Math.max(...app.state.jobs.map((j) => j.op)) : 0;
    const ended = await job('Running the self-test, about two turns of the drum', 'self-test');
    if (!ended) return;
    // The job's result data is in its own document.
    const mine = app.state.jobs.filter((j) => j.op > before && j.name === 'self-test' && j.unit === address).pop();
    if (!mine) return;
    try {
      const op = await getJson('/api/v2/op/' + mine.op);
      status.say(selfTestText(op.data), ended.state !== 'done');
    } catch (error) { /* the line of the job table stays */ }
  }

  async function correct(shouldPlace, showsPlace) {
    const drum = unit.drum || {};
    if (drum.offset == null) return status.say('Its offset is not known yet; try again in a minute.', true);
    const next = correctedOffset(drum.offset, shouldPlace, showsPlace, ALPHABET.length,
                                 STEPS_PER_TURN, OFFSET_LIMIT_STEPS);
    if (next.error) return status.say(next.error, true);
    if (next.offset === drum.offset) return status.say('That is what it shows already: nothing to correct.');
    const set = await job(`Setting its offset to ${next.offset}`, 'set-offset', { offset: next.offset });
    if (!set || set.state !== 'done') return;
    const home = await job('Finding home with the new offset', 'home');
    if (home && home.state === 'done') {
      status.say(`Offset ${drum.offset} → ${next.offset}. It found home and turned back to the flap its row shows.`);
    }
  }

  const number = (id_, min, max, value) =>
    h('input', { type: 'number', id: id_, min, max, value, required: true });
  const steps = number('jogSteps', -127, 127, 10);
  const offset = number('newOffset', -OFFSET_LIMIT_STEPS, OFFSET_LIMIT_STEPS, 0);
  const newAddress = number('newAddress', 1, 126, address);
  const ask = (input, run) => (event) => {
    event.preventDefault();
    run(Number(input.value));
  };
  const service = h('details', {}, h('summary', {}, 'Service'), h('div', { class: 'in' },
    h('form', { class: 'rowwrap', onsubmit: ask(steps, (n) => job(`Nudging it by ${n} steps`, 'jog', { steps: n })) },
      h('label', { for: 'jogSteps' }, 'Nudge by'), steps, h('span', { class: 'muted small' }, 'steps'),
      h('button', { type: 'submit', class: 'btn' }, 'Nudge')),
    h('form', { class: 'rowwrap', onsubmit: ask(offset, (n) => job(`Setting its offset to ${n}`, 'set-offset', { offset: n })) },
      h('label', { for: 'newOffset' }, 'Set offset to'), offset, h('span', { class: 'muted small' }, 'steps'),
      h('button', { type: 'submit', class: 'btn' }, 'Set')),
    h('form', { class: 'rowwrap', onsubmit: ask(newAddress, (n) => job(`Storing address ${n} in the unit`, 'set-address', { address: n },
        `Store address ${n} in this unit? It answers there from now on, whatever its switches say.`)) },
      h('label', { for: 'newAddress' }, 'Bus address'), newAddress,
      h('button', { type: 'submit', class: 'btn' }, 'Store in the unit'),
      h('button', { type: 'button', class: 'btn', onclick: () => job('Clearing its stored address', 'clear-address', null,
        'Clear the stored address? The unit then takes its address from its switches.') }, 'Use its switches')),
    h('div', { class: 'rowwrap' },
      h('button', { type: 'button', class: 'btn', onclick: () => job('Writing the unit firmware again', 'update-units', { force: 1 },
        'Write the unit firmware to this unit again? It takes about a minute and the row shows nothing meanwhile.') }, 'Reinstall firmware'),
      h('button', { type: 'button', class: 'btn', onclick: () => job('Updating its bootloader', 'boot-update', null,
        'Update this unit’s bootloader? Do not cut its power while it runs.') }, 'Update bootloader'),
      h('button', { type: 'button', class: 'btn danger', onclick: () => job('Resetting its turn counter', 'reset-odometer', null,
        'Set this unit’s lifetime turn counter back to zero? This cannot be undone.') }, 'Reset turn counter'))));

  const actions = h('div', { class: 'rowwrap' },
    h('button', { type: 'button', class: 'btn', onclick: () => job('Blinking its light', 'identify') }, 'Blink its light'),
    h('button', { type: 'button', class: 'btn', onclick: () => job('Finding home', 'home') }, 'Find home'),
    h('button', { type: 'button', class: 'btn', onclick: selfTest }, 'Run self-test'),
    h('button', { type: 'button', class: 'btn', onclick: () => job('Restarting the unit', 'restart-unit') }, 'Restart unit'));
  const picture = h('div', { class: 'wall single' });
  const shows = h('p', { class: 'muted small' });
  const root = h('div', { class: 'view' }, top,
    h('div', { class: 'rowwrap top-align' }, picture,
      h('div', { class: 'section grow' }, actions, shows, status)),
    facts, lineUp, service);

  let lineUpFor = null;
  function refresh() {
    const wall = app.state.wall;
    const title = wall ? boardTitle(wallLayout(wall), id) : id;
    const crumbs = h('div', { class: 'crumbs' }, h('a', { href: '#wall' }, 'Wall'), ' / ',
      h('a', { href: '#board/' + id }, title), ' / Unit ' + address);
    if (!unit) {
      fill(top, crumbs, h('div', { class: 'head' }, h('h1', {}, `${title}, unit ${address}`),
        h('span', { class: 'muted small' }, missing ? 'The master knows no unit at this address.' : 'Reading the unit…')));
      return;
    }
    const verdict = unit.verdict ? unitVerdictText(unit.verdict, ALPHABET) : { cls: 'unknown', title: 'Not judged yet', why: '' };
    const also = ((unit.verdict && unit.verdict.also) || [])
      .map((reason) => unitVerdictText({ level: '', reason, a: 0, b: 0 }, ALPHABET).title.toLowerCase());
    fill(top, crumbs,
      h('div', { class: 'head' }, h('h1', {}, `${title}, unit ${address}`), pill(verdict.cls, verdict.title)),
      h('p', { class: 'muted' }, verdict.why, also.length ? ' Also: ' + also.join(', ') + '.' : ''));
    const drum = unit.drum || {};
    const known = unit.state === 'running' && drum.shows != null;
    fill(picture, h('div', { class: 'flap ' + verdict.cls }, h('span', {}, known ? ALPHABET[drum.shows] : '')));
    shows.textContent = `${showsText(unit, ALPHABET)} Position ${unit.position + 1} from the left, bus address ${address}.`;
    fill(facts, factGroups(unitFacts(unit)));

    // Built once for what the unit was sent to, so a choice made is kept.
    const sent = drum.commanded != null ? drum.commanded : null;
    if (lineUpFor !== sent) {
      lineUpFor = sent;
      const should = flapSelect('Should show', sent == null ? 0 : sent);
      const actual = flapSelect('Actually shows', drum.shows != null ? drum.shows : 0);
      fill(lineUp, h('details', {}, h('summary', {}, 'Line up this flap'), h('div', { class: 'in' },
        h('p', { class: 'muted small' },
          'If the flap you see on the wall is not the one this unit was sent to, say what you see and its offset is corrected.'),
        h('div', { class: 'rowwrap' }, h('span', {}, 'It should show'), should,
          h('span', {}, 'and on the wall it shows'), actual,
          h('button', { type: 'button', class: 'btn primary',
                        onclick: () => correct(Number(should.value), Number(actual.value)) }, 'Correct it')))));
    }
  }

  async function read() {
    try {
      unit = await getJson(`/api/v2/unit/${encodeURIComponent(id)}/${address}`);
      missing = false;
    } catch (error) {
      missing = unit == null;
    }
    refresh();
  }
  read();
  // A unit is read in turn by its board, so its facts move without an event.
  const timer = setInterval(read, 10000);

  return { root, refresh, reread: read, leave: () => clearInterval(timer) };
}
