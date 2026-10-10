// A unit: what it shows, what it says about itself, and the jobs on it.
import { ALPHABET, OFFSET_LIMIT_STEPS } from '../gen/constants.js';
import { h, fill, pill, statusLine } from './dom.js';
import { getJson } from './api.js';
import { runJob, targetRow } from './jobs.js';
import { factGroups } from './view_board.js';
import { wallLayout, boardTitle } from '../model/wall.js';
import { unitVerdictText } from '../model/verdict.js';
import { flapName } from '../model/format.js';
import { showsText, unitNow, unitFacts, unitConcerns, unitCan, correctedOffset, selfTestText } from '../model/unit.js';

// One turn of the drum, in steps: also the furthest an offset can go.
const STEPS_PER_TURN = OFFSET_LIMIT_STEPS;

// `selected` null leaves the choice open: nothing is chosen for the reader.
function flapSelect(label, selected) {
  return h('select', { 'aria-label': label },
    selected == null && h('option', { value: '', selected: true }, 'choose…'),
    [...ALPHABET].map((ch, place) =>
      h('option', { value: place, selected: place === selected }, flapName(ALPHABET, place))));
}

export function unitView(app, id, addressText) {
  const address = Number(addressText);
  const top = h('div', {});
  const concerns = h('div', { class: 'section' });
  const now = h('div', {});
  const allFacts = h('div', { class: 'in' });
  const all = h('details', {}, h('summary', {}, 'All facts'), allFacts);
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

  const findHome = () => job('Finding home', 'home');
  const writeFirmware = () => job('Writing the unit firmware', 'update-units', { force: 1 },
    'Write the unit firmware to this unit? It takes about a minute and the row shows nothing meanwhile.');
  const updateBootloader = () => job('Updating its bootloader', 'boot-update', null,
    'Update this unit’s bootloader? Do not cut its power while it runs.');
  const button = (text, onclick, cls) =>
    h('button', { type: 'button', class: 'btn' + (cls ? ' ' + cls : ''), onclick }, text);
  // The button that cures a reason, next to the reason.
  const CURES = {
    'firmware': (can) => can.firmware && button('Update its firmware', writeFirmware, 'primary'),
    'boot-update': (can) => can.act && button('Update bootloader', updateBootloader),
    'home': (can) => can.act && button('Find home', findHome, 'primary'),
  };

  const number = (id_, min, max, value) =>
    h('input', { type: 'number', id: id_, min, max, value, required: true });
  const steps = number('jogSteps', -127, 127, 10);
  // Filled with the unit's own offset once that is read: a Set must not
  // throw its calibration away.
  const offset = number('newOffset', -OFFSET_LIMIT_STEPS, OFFSET_LIMIT_STEPS, '');
  const setOffset = h('button', { type: 'submit', class: 'btn', disabled: true }, 'Set');
  offset.disabled = true;
  const newAddress = number('newAddress', 1, 126, address);
  const ask = (input, run) => (event) => {
    event.preventDefault();
    run(Number(input.value));
  };
  const ownFirmware = h('div', { class: 'section' },
    h('form', { class: 'rowwrap', onsubmit: ask(steps, (n) => job(`Nudging it by ${n} steps`, 'jog', { steps: n })) },
      h('label', { for: 'jogSteps' }, 'Nudge by'), steps, h('span', { class: 'muted small' }, 'steps'),
      h('button', { type: 'submit', class: 'btn' }, 'Nudge')),
    h('form', { class: 'rowwrap', onsubmit: ask(offset, (n) => job(`Setting its offset to ${n}`, 'set-offset', { offset: n })) },
      h('label', { for: 'newOffset' }, 'Set offset to'), offset, h('span', { class: 'muted small' }, 'steps'),
      setOffset),
    h('form', { class: 'rowwrap', onsubmit: ask(newAddress, (n) => job(`Storing address ${n} in the unit`, 'set-address', { address: n },
        `Store address ${n} in this unit? It answers there from now on, whatever its switches say.`)) },
      h('label', { for: 'newAddress' }, 'Bus address'), newAddress,
      h('button', { type: 'submit', class: 'btn' }, 'Store in the unit'),
      button('Use its switches', () => job('Clearing its stored address', 'clear-address', null,
        'Clear the stored address? The unit then takes its address from its switches.'))));
  const reinstall = button('Reinstall firmware', writeFirmware);
  const ownButtons = [
    button('Update bootloader', updateBootloader),
    button('Reset turn counter', () => job('Resetting its turn counter', 'reset-odometer', null,
      'Set this unit’s lifetime turn counter back to zero? This cannot be undone.'), 'danger')];
  const service = h('details', {}, h('summary', {}, 'Service'), h('div', { class: 'in' },
    ownFirmware, h('div', { class: 'rowwrap' }, reinstall, ownButtons)));

  const actions = h('div', { class: 'rowwrap' },
    button('Blink its light', () => job('Blinking its light', 'identify')),
    button('Find home', findHome),
    button('Run self-test', selfTest),
    button('Restart unit', () => job('Restarting the unit', 'restart-unit')));
  const picture = h('div', { class: 'wall single' });
  const shows = h('p', { class: 'muted small' });
  const body = h('div', { class: 'view-part', hidden: true },
    h('div', { class: 'rowwrap top-align' }, picture,
      h('div', { class: 'section grow' }, concerns, shows, actions, status)),
    now, lineUp, all, service);
  const root = h('div', { class: 'view' }, top, body);

  let lineUpFor;
  function refresh() {
    const wall = app.state.wall;
    const title = wall ? boardTitle(wallLayout(wall), id) : id;
    const crumbs = h('div', { class: 'crumbs' }, h('a', { href: '#wall' }, 'Wall'), ' / ',
      h('a', { href: '#board/' + id }, title), ' / Unit ' + address);
    body.hidden = !unit;
    if (!unit) {
      fill(top, crumbs, h('div', { class: 'head' }, h('h1', {}, `${title}, unit ${address}`),
        h('span', { class: 'muted small' }, missing ? 'The master knows no unit at this address.' : 'Reading the unit…')),
        missing && h('p', {}, h('a', { href: '#board/' + id }, `Back to ${title}`), ', which lists its units.'));
      return;
    }
    const verdict = unit.verdict ? unitVerdictText(unit.verdict, ALPHABET) : { cls: 'unknown', title: 'Not judged yet', why: '' };
    fill(top, crumbs,
      h('div', { class: 'head' }, h('h1', {}, `${title}, unit ${address}`), pill(verdict.cls, verdict.title)));
    const can = unitCan(unit);
    const odd = unitConcerns(unit, ALPHABET);
    fill(concerns, odd.length
      ? [h('h2', {}, 'Out of the ordinary'), h('div', { class: 'list' }, odd.map((c) =>
          h('div', { class: 'item' }, h('span', { class: 'dot ' + c.cls }),
            h('span', { class: 'main' }, h('div', { class: 't' }, c.title),
              h('div', { class: 'd' }, c.why, ' ', c.todo)),
            c.cure && CURES[c.cure](can))))]
      : h('p', { class: 'muted' }, unit.verdict ? 'Nothing is out of the ordinary. ' + verdict.why
                                                 : 'The master has not judged this unit yet.'));
    const drum = unit.drum || {};
    const known = unit.state === 'running' && drum.shows != null;
    fill(picture, h('div', { class: 'flap ' + verdict.cls }, h('span', {}, known ? ALPHABET[drum.shows] : '')));
    shows.textContent = `${showsText(unit, ALPHABET)} Position ${unit.position + 1} from the left, bus address ${address}.`;
    actions.hidden = !can.act;
    ownFirmware.hidden = !can.act;
    ownButtons.forEach((b) => { b.hidden = !can.act; });
    reinstall.hidden = !can.firmware;
    service.hidden = !can.act && !can.firmware;
    if (offset.disabled && drum.offset != null) {
      offset.value = drum.offset;
      offset.disabled = false;
      setOffset.disabled = false;
    }

    const nowRows = unitNow(unit);
    fill(now, nowRows.length > 0 && factGroups([{ title: 'Now', rows: nowRows }]));
    const groups = unitFacts(unit);
    all.hidden = !groups.length;
    fill(allFacts,
      h('p', { class: 'muted small' },
        'Everything the unit and its board report. What matters of it, the master says above.'),
      factGroups(groups, { 'Over its lifetime': h('div', { class: 'why' },
        'Counted since the unit was built and kept in its memory: history, not faults of now.') }));

    // Built once for what the unit was sent to, so a choice made is kept.
    const sent = drum.commanded != null ? drum.commanded : null;
    const key = can.act ? 'to ' + sent : 'cannot';
    if (lineUpFor !== key) {
      lineUpFor = key;
      if (!can.act) return void fill(lineUp);
      const should = flapSelect('Should show', sent);
      const actual = flapSelect('Actually shows', null);
      const go = button('Correct it', () => correct(Number(should.value), Number(actual.value)), 'primary');
      const ready = () => { go.disabled = should.value === '' || actual.value === ''; };
      should.addEventListener('change', ready);
      actual.addEventListener('change', ready);
      ready();
      fill(lineUp, h('details', {}, h('summary', {}, 'Line up this flap'), h('div', { class: 'in' },
        h('p', { class: 'muted small' },
          'For a flap that stands a flap or more beside the one this page says it shows. '
          + 'Say what you see on the wall and its offset is corrected.'),
        h('div', { class: 'rowwrap' }, h('span', {}, 'It should show'), should,
          h('span', {}, 'and on the wall it shows'), actual, go))));
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
