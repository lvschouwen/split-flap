// Calibrate the wall: every unit turns to one letter, the flaps that show
// something else are marked on the picture, and each of those is told what it
// shows.
import { ALPHABET, OFFSET_LIMIT_STEPS } from '../gen/constants.js';
import { h, fill, statusLine } from './dom.js';
import { getJson, action } from './api.js';
import { runJob, targetRow } from './jobs.js';
import { wallPicture } from './view_wall.js';
import { wallLayout, boardTitle, composeLines } from '../model/wall.js';
import { flapName, plural } from '../model/format.js';
import { testText, calibrationPlan } from '../model/calibrate.js';

// How long the test letter stays without being asked for again.
const TEST_LETTER_S = 900;
const STEP_TITLES = ['Show a test letter', 'Mark the wrong ones', 'Say what they show'];

export function calibrateView(app) {
  let step = 1;
  let place = 1;             // the test letter's place on the drum
  let busy = false;
  const marked = new Set();  // "<board id>/<unit>"
  const steps = h('div', { class: 'steps' });
  const body = h('div', { class: 'section' });
  const picture = h('div', {});
  const status = statusLine();
  const root = h('div', { class: 'view' },
    h('div', { class: 'crumbs' }, h('a', { href: '#wall' }, 'Wall'), ' / Calibrate'),
    h('div', { class: 'head' }, h('h1', {}, 'Calibrate the wall')), steps, body, status);

  const letter = () => ALPHABET[place];
  const layout = () => wallLayout(app.state.wall);
  const button = (text, run, cls) =>
    h('button', { type: 'button', class: 'btn' + (cls ? ' ' + cls : ''),
                  onclick: () => { if (!busy) run(); } }, text);
  // What was said belongs to the step that is left.
  const go = (to) => { step = to; status.say(''); draw(); };

  // Also asked for again after a correction: the same text changes nothing
  // on the wall and starts its time anew.
  function showLetter() {
    return action('show', null, { text: testText(composeLines(layout()), letter()), forS: TEST_LETTER_S });
  }

  async function start() {
    if (!app.state.wall) return status.say('The wall has not been read yet.', true);
    busy = true;
    try {
      await showLetter();
      marked.clear();
      go(2);
    } catch (error) {
      status.say(error.message, true);
    }
    busy = false;
  }

  // Ends the test letter before its time: the mode shows its own again.
  async function finish() {
    const mode = (app.state.show || app.state.wall || {}).mode;
    busy = true;
    try {
      if (mode) await action('mode', null, { mode });
      go(1);
      status.say(mode === 'clock' ? 'Done. The wall is back to the clock.' : 'Done. The wall is back to its text.');
    } catch (error) {
      status.say(error.message, true);
    }
    busy = false;
  }

  async function correct(choices) {
    if (choices.some((choice) => choice.select.value === '')) {
      return status.say('Choose for every unit what it shows.', true);
    }
    busy = true;
    const marks = [];
    for (const { id, unit, title, select } of choices) {
      status.say(`Reading ${title}, unit ${unit}…`);
      let offset = null;
      try {
        const drum = (await getJson(`/api/v2/unit/${encodeURIComponent(id)}/${unit}`)).drum || {};
        if (drum.offset != null) offset = drum.offset;
      } catch (error) { /* said by the plan */ }
      marks.push({ id, title, unit, shows: Number(select.value), offset });
    }
    const plan = calibrationPlan(marks, place, ALPHABET.length, OFFSET_LIMIT_STEPS, OFFSET_LIMIT_STEPS);
    const problems = plan.problems.slice();
    const homes = new Map();  // board id -> its title
    let corrected = 0;
    for (const set of plan.sets) {
      const name = `${set.title}, unit ${set.unit}`;
      const ended = await runJob(status, `${name}: setting its offset to ${set.offset}`, 'set-offset',
                                 targetRow(app, set.id, set.unit), { offset: set.offset });
      if (ended && ended.state === 'done') {
        corrected++;
        homes.set(set.id, set.title);
      } else {
        problems.push(`${name}: ${status.textContent}`);
      }
    }
    // Finding home on the whole board shows the board's text again when it
    // is done, so the corrected flaps turn back to the test letter.
    for (const [id, title] of homes) {
      const ended = await runJob(status, `${title}: finding home with the new offsets, about a minute`, 'home-all',
                                 targetRow(app, id));
      if (!ended || ended.state !== 'done') problems.push(`${title}: ${status.textContent}`);
    }
    try { await showLetter(); } catch (error) { problems.push(error.message); }
    busy = false;
    marked.clear();
    go(2);
    status.say((corrected ? `Corrected ${plural(corrected, 'unit')}: look at the wall once more. `
                          : 'Nothing was corrected. ') + problems.join(' '), corrected === 0);
  }

  function pickPicture() {
    return wallPicture(layout(), (board, cell, attrs) => {
      const key = `${board.id}/${cell.unit}`;
      const dead = cell.cls === 'bad';
      return h('button', {
        // No verdict colours here: the only mark on this picture is the reader's.
        ...attrs, type: 'button', disabled: dead,
        class: 'flap plain' + (dead ? ' bad' : '') + (marked.has(key) ? ' sel' : ''),
        'aria-pressed': String(marked.has(key)),
        onclick: () => {
          if (!marked.delete(key)) marked.add(key);
          draw();
        },
      }, h('span', {}, dead ? '' : letter()));
    });
  }

  // The marked units as they stand on the wall now, top to bottom.
  function markedUnits() {
    const wall = layout();
    const units = [];
    for (const line of wall.lines) {
      for (const board of line.boards) {
        for (const cell of board.cells) {
          if (marked.has(`${board.id}/${cell.unit}`)) {
            units.push({ id: board.id, unit: cell.unit, title: boardTitle(wall, board.id) });
          }
        }
      }
    }
    return units;
  }

  function draw() {
    fill(steps, STEP_TITLES.map((title, i) =>
      h(i + 1 === step ? 'b' : 'span', {}, `${i + 1}. ${title}`)));
    const name = flapName(ALPHABET, place);
    if (!app.state.wall) {
      fill(body, h('p', { class: 'muted' }, 'Reading the wall…'));
    } else if (step === 1) {
      const pick = h('select', { 'aria-label': 'Test letter', onchange: () => { place = Number(pick.value); } },
        [...ALPHABET].map((ch, at) => at > 0 && h('option', { value: at, selected: at === place }, ch)));
      fill(body,
        h('p', { class: 'muted' }, `Every unit turns to the same letter for ${TEST_LETTER_S / 60} minutes. ` +
          'Then you compare the real wall with a picture of it here.'),
        h('div', { class: 'rowwrap' }, pick, button('Show it on every unit', start, 'primary')));
    } else if (step === 2) {
      const n = marked.size;
      fill(picture, pickPicture());
      fill(body,
        h('p', { class: 'muted' }, `The drums are turning to ${name}: wait until the real wall stands still. ` +
          'Then mark each flap here that shows something else on the real wall.'),
        picture,
        h('div', { class: 'rowwrap' },
          n ? button(`Continue with ${plural(n, 'unit')}`, () => go(3), 'primary')
            : button(`They all show ${name}: done`, finish, 'primary'),
          button('Back', () => go(1))));
    } else {
      const choices = markedUnits().map((unit) => ({
        ...unit,
        select: h('select', { 'aria-label': `${unit.title}, unit ${unit.unit} actually shows` },
          h('option', { value: '' }, 'It shows…'),
          [...ALPHABET].map((ch, at) => at !== place && h('option', { value: at }, flapName(ALPHABET, at)))),
      }));
      fill(body,
        h('p', { class: 'muted' }, 'For each marked unit, choose what it really shows. Its offset is corrected, ' +
          'its row finds home and turns to the test letter again.'),
        h('div', { class: 'list' }, choices.map((choice) =>
          h('div', { class: 'item' },
            h('span', { class: 'main' },
              h('div', { class: 't' }, `${choice.title}, unit ${choice.unit}`),
              h('div', { class: 'd' }, 'should show ' + name)),
            choice.select))),
        h('div', { class: 'rowwrap' },
          button(`Correct ${plural(choices.length, 'unit')}`, () => correct(choices), 'primary'),
          button('Back', () => go(2))));
    }
  }

  // The stream moves the picture's levels; what was chosen in step 3 stays.
  let drawn = false;
  function refresh() {
    if (!app.state.wall) return draw();
    if (!drawn) {
      drawn = true;
      draw();
    } else if (step === 2 && !busy) {
      fill(picture, pickPicture());
    }
  }

  return { root, refresh };
}
