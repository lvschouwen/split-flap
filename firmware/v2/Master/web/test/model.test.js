// The page's pure model, run by `node --test` (under pytest: tests/test_web_page.py).
import test from 'node:test';
import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';

import { ALPHABET } from '../gen/constants.js';
import { dur, volt, listOf, flapName, plural } from '../model/format.js';
import { unitVerdictText, boardVerdictText, wallVerdictText, letterClass,
         BOARD_REASON_NAMES, UNIT_REASON_NAMES } from '../model/verdict.js';
import { boardUnits, tileLines, lowestSupply, unitsBehind, boardReach, startWords, sparkPoints, boardFacts,
         startMarks } from '../model/board.js';
import { showsText, unitNow, unitFacts, unitConcerns, unitCan, correctedOffset, selfTestText } from '../model/unit.js';
import { firmwareRows, firmwareVerdict, firmwareFile, installQuestion, firmwareJobs,
         releaseText, releaseProgress, updateQuestion } from '../model/firmware.js';
import { md5Hex } from '../model/md5.js';
import { whenText, eventText, eventBoardId } from '../model/events.js';
import { wallLayout, boardTitle, attentionList, notesList, boardLine, boardId, noFlapFor,
         composeLines, composeText } from '../model/wall.js';
import { testText, calibrationPlan } from '../model/calibrate.js';
import { rowsDraft, arrangeProblem, arrangeArgs, zoneFor, brokerText, foundLine } from '../model/settings.js';

const FIELDS = ['address', 'level', 'reason', 'a', 'b', 'state', 'rev', 'firmware',
                'bootloader', 'supplyMv', 'supplyMinMv', 'shows', 'turns', 'offset'];
const unitRow = (address, level, reason, a = 0, b = 0, supplyMinMv = 5000) =>
  [address, level, reason, a, b, 'running', 'abc', 'current', 'ok', 5000, supplyMinMv, 0, 10, 60];

const WALL = {
  master: { id: 'wall-master', rev: 'aaa1111' },
  verdict: 'fault',
  mode: 'clock',
  rows: [
    { id: '', own: true, row: 1, col: 0, width: 4, text: 'ab d', showing: 'zzzz',
      verdict: { level: 'fault', reason: 'units-fault', a: 1, b: 4 }, unitLevels: 'wnfn' },
    { id: 'wall-row', own: false, row: 0, col: 1, width: 3, text: '12:', rev: 'aaa1111',
      verdict: { level: 'fault', reason: 'bus-dead', a: 6, b: 0 }, unitLevels: 'f',
      status: { rssi: -57 } },
  ],
};
const BOARDS = {
  'wall-master': { units: { fields: FIELDS, rows: [
    unitRow(1, 'working', 'working', 100),
    unitRow(2, 'note', 'jammed'),
    unitRow(3, 'fault', 'hall-never', 0, 0, 4700),
    unitRow(4, 'note', 'jammed'),
  ] } },
};

test('durations and volts read as on the page', () => {
  assert.equal(dur(59), '59 s');
  assert.equal(dur(125), '2 min');
  assert.equal(dur(3 * 3600 + 120), '3 h 2 min');
  assert.equal(dur(2 * 86400 + 3600), '2 d 1 h');
  assert.equal(volt(4871), '4.87 V');
  assert.equal(listOf([1]), '1');
  assert.equal(listOf([1, 2, 7]), '1, 2 and 7');
  assert.equal(plural(1, 'unit'), '1 unit');
  assert.equal(plural(3, 'unit'), '3 units');
});

test('a flap is named by the character at its place on the drum', () => {
  assert.equal(ALPHABET[0], ' ');
  assert.equal(flapName(ALPHABET, 0), 'blank');
  assert.equal(flapName(ALPHABET, 1), 'A');
  assert.equal(flapName(ALPHABET, 999), '?');
});

test('a unit verdict says what its two numbers mean', () => {
  const low = unitVerdictText({ level: 'fault', reason: 'low-supply', a: 3900, b: 4000 }, ALPHABET);
  assert.equal(low.cls, 'bad');
  assert.equal(low.title, 'Low supply');
  assert.equal(low.why, 'Lowest supply since it started: 3.90 V. Units warn below 4.00 V.');
  const wrong = unitVerdictText({ level: 'note', reason: 'wrong-letter', a: 30, b: 0 }, ALPHABET);
  assert.match(wrong.why, /stands at 0,/);
  assert.equal(wrong.cls, 'note');
});

test('a reason the page does not know is shown by its wire name', () => {
  const text = unitVerdictText({ level: 'note', reason: 'brand-new', a: 1, b: 2 }, ALPHABET);
  assert.deepEqual(text, { cls: 'note', title: 'brand-new', why: '', todo: '' });
  assert.equal(boardVerdictText({ level: 'fault', reason: 'brand-new' }).title, 'brand-new');
  assert.equal(letterClass('x'), 'unknown');
});

test('a board verdict counts its units', () => {
  assert.equal(boardVerdictText({ level: 'fault', reason: 'units-fault', a: 1, b: 16 }).title,
               '1 unit has a fault');
  assert.equal(boardVerdictText({ level: 'fault', reason: 'units-fault', a: 2, b: 16 }).why,
               '14 of 16 units working.');
});

test('the wall verdict counts what needs attention', () => {
  assert.deepEqual(wallVerdictText('fault', 1), { cls: 'bad', title: '1 thing needs attention' });
  assert.equal(wallVerdictText('fault', 3).title, '3 things need attention');
  assert.equal(wallVerdictText('working', 0).title, 'Everything is working');
});

test('the layout puts rows top to bottom and boards at their column', () => {
  const layout = wallLayout(WALL);
  assert.equal(layout.cols, 4);
  assert.deepEqual(layout.lines.map((l) => l.title), ['Row 1', 'Row 2']);
  const [top, bottom] = layout.lines;
  assert.equal(top.boards[0].id, 'wall-row');
  assert.equal(top.boards[0].col, 1);
  assert.deepEqual(top.boards[0].cells.map((c) => c.ch + c.cls), ['1bad', '2unknown', ':unknown']);
  assert.equal(bottom.boards[0].id, 'wall-master');
  assert.deepEqual(bottom.boards[0].cells.map((c) => c.ch), ['A', 'B', ' ', 'D']);
  assert.deepEqual(bottom.boards[0].cells.map((c) => c.unit), [1, 2, 3, 4]);
});

test('boards that share a grid row are named by their place in it', () => {
  const wall = { master: { id: 'm' }, rows: [
    { id: '', own: true, row: 0, col: 0, width: 2, text: '' },
    { id: 'r', own: false, row: 0, col: 2, width: 2, text: '' },
  ] };
  const layout = wallLayout(wall);
  assert.equal(layout.lines.length, 1);
  assert.equal(layout.cols, 4);
  assert.equal(boardTitle(layout, 'm'), 'Row 1, board 1');
  assert.equal(boardTitle(layout, 'r'), 'Row 1, board 2');
  assert.equal(boardId(wall, wall.rows[0]), 'm');
});

test('needs attention lists a board in trouble and every unit with a fault', () => {
  const layout = wallLayout(WALL);
  const items = attentionList(WALL, layout, BOARDS, ALPHABET);
  assert.deepEqual(items.map((i) => [i.title, i.href]), [
    ['Row 2, unit 3: never saw its home sensor', '#unit/wall-master/3'],
    ['Row 1: unit bus dead', '#board/wall-row'],
  ]);
});

test('a board in trouble is listed once, not once more for each of its units', () => {
  const layout = wallLayout(WALL);
  const boards = { ...BOARDS, 'wall-row': { units: { fields: FIELDS, rows: [
    unitRow(1, 'fault', 'not-answering', 5400, 255)] } } };
  const titles = attentionList(WALL, layout, boards, ALPHABET).map((i) => i.title);
  assert.deepEqual(titles.filter((t) => t.startsWith('Row 1')), ['Row 1: unit bus dead']);
});

test('the missed-reads count is not quoted past where it stops', () => {
  const text = unitVerdictText({ level: 'fault', reason: 'not-answering', a: 5400, b: 255 }, ALPHABET);
  assert.equal(text.why, 'No reply for 1 h 30 min, at least 255 reads missed.');
});

test('worth knowing groups the units that share a note, then the lowest supply', () => {
  const layout = wallLayout(WALL);
  const items = notesList(WALL, layout, BOARDS, ALPHABET);
  assert.deepEqual(items.map((i) => [i.cls, i.title, i.why, i.href]), [
    ['note', 'Row 2: 2 units, last move stalled', 'Units 2 and 4.', '#board/wall-master'],
    ['info', 'Lowest supply seen: 4.70 V', 'Row 2, unit 3.', '#unit/wall-master/3'],
  ]);
});

test('a board line carries its verdict and what it runs', () => {
  const layout = wallLayout(WALL);
  const line = boardLine(WALL, layout, WALL.rows[1]);
  assert.equal(line.title, 'Row 1');
  assert.equal(line.kind, 'row board');
  assert.equal(line.verdict, 'Unit bus dead');
  assert.match(line.why, /Firmware aaa1111 WiFi -57 dBm$/);
  assert.equal(boardLine(WALL, layout, WALL.rows[0]).kind, 'master');
});

test('the characters the drum has no flap for are named once each', () => {
  assert.deepEqual(noFlapFor('Hello, wörld, ok', ALPHABET), [',', 'Ö']);
  assert.deepEqual(noFlapFor('dinner 18:30!', ALPHABET), []);
});

test('the compose form has a line a grid row, as wide as the wall is there', () => {
  assert.deepEqual(composeLines(wallLayout(WALL)),
                   [{ title: 'Row 1', width: 4 }, { title: 'Row 2', width: 4 }]);
});

test('the text sent is a line a row, without empty lines at the end', () => {
  assert.equal(composeText(['12:00', 'DINNER']), '12:00\nDINNER');
  assert.equal(composeText(['', 'DINNER']), '\nDINNER');
  assert.equal(composeText(['HELLO', ' ']), 'HELLO');
  assert.equal(composeText(['', '']), '');
});

test('a tile says what the unit shows, or why it cannot', () => {
  const [working, , faulty, silent] = boardUnits({ units: { fields: FIELDS, rows: [
    unitRow(1, 'working', 'working'), unitRow(2, 'note', 'jammed'), unitRow(3, 'fault', 'hall-never'),
    [4, 'fault', 'not-answering', 9, 9, 'running', null, null, null, null, null, null, 7, null]] } });
  assert.deepEqual(tileLines(working, ALPHABET), ['shows blank', '5.00 V']);
  assert.deepEqual(tileLines({ ...working, shows: 1 }, ALPHABET), ['shows A', '5.00 V']);
  assert.deepEqual(tileLines({ ...working, shows: null }, ALPHABET), ['flap unknown', '5.00 V']);
  assert.deepEqual(tileLines({ ...working, state: 'bootloader' }, ALPHABET), ['in bootloader', '5.00 V']);
  // A unit with a fault says the fault, not the flap it stands at.
  assert.equal(tileLines(faulty, ALPHABET)[0], 'Never saw its home sensor');
  assert.deepEqual(tileLines(silent, ALPHABET), ['Not answering']);
});

test('the weakest supply is named with its unit', () => {
  const units = [{ address: 1, supplyMinMv: 5300 }, { address: 2, supplyMinMv: 4650 },
                 { address: 3, supplyMinMv: 3900 }, { address: 4, supplyMinMv: null }];
  assert.deepEqual(lowestSupply(units), { mv: 3900, address: 3 });
  assert.equal(lowestSupply([{ address: 4, supplyMinMv: null }]), null);
});

test('a button is offered when it has something to do', () => {
  assert.equal(unitsBehind([{ firmware: 'current' }, { firmware: 'outdated' }, { firmware: null }]), 1);
  assert.deepEqual(boardReach({ verdict: { reason: 'working' } }), { board: true, units: true, why: '' });
  for (const reason of ['lost', 'never-seen', 'away']) {
    const reach = boardReach({ verdict: { reason } });
    assert.deepEqual([reach.board, reach.units], [false, false], reason);
    assert.ok(reach.why);
  }
  const rescue = boardReach({ verdict: { reason: 'rescue' } });
  assert.deepEqual([rescue.board, rescue.units], [true, false]);
  assert.ok(rescue.why);
});

test('why a board started is said in words, with what the board said under it', () => {
  assert.deepEqual(startWords('Software reset', 'reboot requested (unattributed)'),
    ['It was restarted on purpose', 'As the board says it: Software reset.']);
  assert.deepEqual(startWords('Panic', 'LoadProhibited in displayTask'),
    ['Its firmware crashed', 'As the board says it: Panic: LoadProhibited in displayTask.']);
  assert.equal(startWords('Power on')[0], 'The power was switched on');
  assert.equal(startWords('Brownout')[0], 'Its supply voltage dropped too low');
  assert.equal(startWords('Task watchdog')[0], 'It hung and its watchdog restarted it');
  assert.deepEqual(startWords('Something new', 'x'), ['Something new: x', null]);
  assert.equal(startWords(undefined), null);
});

test('a history ring becomes a line from left to right', () => {
  assert.equal(sparkPoints([0, 10], 300, 40), '0.0,36.0 300.0,4.0');
  assert.equal(sparkPoints([5, 5, 5], 300, 40), '0.0,36.0 150.0,36.0 300.0,36.0');
  assert.equal(sparkPoints([1], 300, 40), '');
});

test('the facts of the master and of a row board answer the same questions, counters last and folded', () => {
  const master = boardFacts({ kind: 'master', address: '10.0.0.2', rev: 'aaa1111', mqttConnected: true,
    stats: { now: { rssi: -52, txPower: 20, heap: 61440, minHeap: 36864, temp: 316, uptime: 5620,
                    i2cTx: 13780, i2cErr: 0, ntpAge: 5611 } },
    network: { gw: 'fail', self: 'ok' }, lastStart: { reset: 'Software reset', cause: 'asked for' },
    rescue: { rev: 'bbb2222', state: 'stale', warn: false } }, [{ address: 1, supplyMinMv: 4850 }]);
  assert.deepEqual(master.map((g) => [g.title, g.folded]),
    [['Connection', false], ['Unit bus', false], ['Running', false], ['Firmware', false], ['Counters', true]]);
  const find = (groups, label) => groups.flatMap((g) => g.rows).find((r) => r[0] === label);
  assert.deepEqual(find(master, 'WiFi signal'), ['WiFi signal', '-52 dBm, good']);
  // What the page itself proves is not listed; what is plainly wrong is marked.
  assert.equal(find(master, 'Own web server answers'), undefined);
  assert.deepEqual(find(master, 'Router reachable'), ['Router reachable', 'no', null, 'bad']);
  assert.deepEqual(find(master, 'Last start'),
    ['Last start', 'It was restarted on purpose', 'As the board says it: Software reset: asked for.', undefined]);
  assert.deepEqual(find(master, 'Lowest unit supply').slice(0, 2), ['Lowest unit supply', '4.85 V (unit 1)']);
  assert.deepEqual(master[4].rows.map((r) => r[0]), ['Unit bus exchanges since start', 'of which failed',
    'Free memory', 'Lowest it has been', 'Chip temperature', 'Transmit power']);
  assert.deepEqual(find(master, 'Transmit power'), ['Transmit power', '5 dBm']);

  const row = boardFacts({ kind: 'row', pairedAt: '10.0.0.3', reach: 'down', heardMsAgo: 1400, rev: 'aaa1111',
    rescue: false, connects: 2, restarts: 1, lastLateMs: 0, worstLateMs: 12, updateAttempts: 0,
    status: { uptimeS: 5223, heap: 35000, heapMin: 32120, rssi: -77, txPowerDbm: 2, busTx: 12326,
              busErrors: 11514, busDead: true, busEpisodes: 6, escalations: 1, imageSize: 460752,
              timeSynced: true } }, []);
  assert.deepEqual(row.map((g) => g.title), ['Connection', 'Unit bus', 'Running', 'Firmware', 'Counters']);
  assert.deepEqual(find(row, 'Address'), ['Address', '10.0.0.3']);
  assert.deepEqual(find(row, 'Link to the master'), ['Link to the master', 'down', null, 'bad']);
  assert.deepEqual(find(row, 'WiFi signal'), ['WiFi signal', '-77 dBm, weak']);
  assert.deepEqual(find(row, 'Answers now'), ['Answers now', 'no', null, 'bad']);
  assert.deepEqual(find(row, 'Row flips late by'), ['Row flips late by', '0 ms', 'Worst since it connected: 12 ms.']);
  assert.equal(find(row, 'Lowest unit supply'), undefined);
  assert.equal(row[4].rows.length, 11);
});

test('a fact the board did not give is left out, not shown as zero', () => {
  const groups = boardFacts({ kind: 'row' }, []);
  assert.deepEqual(groups.flatMap((g) => g.rows), []);
  assert.equal(groups.some((g) => g.folded), false);
});

test('a board verdict that is not plain working says what to do, or that there is nothing to do', () => {
  assert.ok(BOARD_REASON_NAMES.length >= 15);
  for (const reason of BOARD_REASON_NAMES) {
    const text = boardVerdictText({ level: 'fault', reason, a: 2, b: 16 });
    if (reason !== 'working') assert.ok(text.todo, reason + ' says nothing to do about it');
  }
  assert.equal(boardVerdictText({ level: 'note', reason: 'units-note', a: 1, b: 16 }).title, 'Working, with notes');
  assert.equal(boardVerdictText({ level: 'working', reason: 'working', a: 16, b: 60 }).todo, '');
});

test('starts are marked by whether the power was cut', () => {
  assert.deepEqual(startMarks({ starts: [{ reset: 'Software reset', stage: 'online' },
                                         { reset: 'Power on', stage: 'online' },
                                         { reset: 'Panic', stage: 'wifi' }] }),
    [{ cls: '', title: 'Software reset' }, { cls: 'pwr', title: 'Power on' },
     { cls: 'odd', title: 'Panic, reached wifi' }]);
});

test('what a unit shows is said against what it was sent to', () => {
  const unit = { state: 'running', drum: { shows: 30, commanded: 30, wrongLetter: false } };
  assert.equal(showsText(unit, ALPHABET), 'Shows 0, as it should.');
  assert.equal(showsText({ state: 'running', drum: { shows: 0, commanded: 30, wrongLetter: true } }, ALPHABET),
               'Shows blank; it was sent to 0.');
  assert.equal(showsText({ state: 'running', drum: {} }, ALPHABET), 'Where its drum stands is not known.');
  assert.match(showsText({ state: 'silent', drum: { shows: 3 } }, ALPHABET), /not running/);
});

test('a unit that was read for nothing has no facts', () => {
  const silent = { state: 'silent', firmware: {}, power: {}, link: {}, drum: {}, bootloader: {} };
  assert.deepEqual(unitNow(silent), []);
  assert.deepEqual(unitFacts(silent), []);
});

const READ_IN_FULL = {
  state: 'running', addressStored: false,
  firmware: { rev: 'd360e2b', status: 'current', uptimeS: 182333, protocol: 1, protocolSupported: true },
  power: { brownouts: 2, watchdogResets: 1, lastStart: 'requested', restartedWhileWatched: false,
           supplyMv: 5091, supplyMinMv: 4979, freeRamMin: 1490, supplyDuringLastMoveMv: 5001 },
  link: { badCommands: 87, received: 29696, answered: 41254, heardMsAgo: 38222, missed: 0, failed: 0 },
  drum: { home: 'not-homed', homeSteps: 256, homeFailures: 0, slips: 0, jammed: true, turns: 1092, offset: 70,
          homeExcessSteps: 0, homeExcessStepsMax: 61, homeExcessStepsEver: 80,
          selfTest: { firstHallWindow: 38, lastHallWindow: 35, firstStepsPerTurn: 2057, lastStepsPerTurn: 2050 } },
  bootloader: { verdict: 'ok' } };

test('the few facts of now stand on the page, the rest is under all facts', () => {
  assert.deepEqual(unitNow(READ_IN_FULL), [
    ['Supply', '5.09 V', 'Lowest since it started: 4.98 V.'],
    ['Last heard', '38 s ago'],
    ['Home', 'not homed'],
    ['Firmware', 'd360e2b (current)'],
    ['Offset', '70 steps'],
  ]);
  const groups = unitFacts(READ_IN_FULL);
  assert.deepEqual(groups.map((g) => g.title),
                   ['Power', 'Link to its board', 'Drum', 'Firmware', 'Over its lifetime']);
  const find = (label) => groups.flatMap((g) => g.rows).find((r) => r[0] === label);
  assert.deepEqual(find('Last search for home'), ['Last search for home', '256 steps']);
  assert.deepEqual(find('Overshot home'), ['Overshot home', '0 steps', 'At worst since it started: 61 steps.']);
  assert.deepEqual(find('Last move stalled'), ['Last move stalled', 'yes']);
  assert.deepEqual(find('Slipped and corrected itself'), ['Slipped and corrected itself', 0, null]);
  assert.deepEqual(find('Address kept in its memory'),
                   ['Address kept in its memory', 'no', 'It takes its address from its switches.']);
  // What a self-test measured is said against the unit's first one.
  assert.deepEqual(find('Self-test, steps a turn'), ['Self-test, steps a turn', 2050, 'At its first test: 2057.']);
  assert.deepEqual(unitFacts({ ...READ_IN_FULL, drum: { selfTest: { firstHallWindow: 38, lastHallWindow: 38,
    firstStepsPerTurn: 2050, lastStepsPerTurn: 2050 } } }).flatMap((g) => g.rows)
    .filter((r) => r[0].startsWith('Self-test')).map((r) => r[2]), ['As at its first test.', 'As at its first test.']);
});

test('what a unit counted over its whole life is a group of its own', () => {
  const life = unitFacts(READ_IN_FULL).find((g) => g.title === 'Over its lifetime');
  assert.deepEqual(life.rows, [
    ['Restarts from low voltage', 2],
    ['Watchdog resets', 1],
    ['Failed to find home', 0],
    ['Worst overshoot of home', '80 steps'],
    ['Turns of the drum', '1,092'],
  ]);
  const now = unitFacts(READ_IN_FULL).filter((g) => g.title !== 'Over its lifetime').flatMap((g) => g.rows);
  assert.equal(now.some((r) => /lifetime/i.test(r[0])), false);
});

test('every reason a unit can have says what to do about it, or that there is nothing to do', () => {
  assert.ok(UNIT_REASON_NAMES.length >= 22);
  for (const reason of UNIT_REASON_NAMES) {
    const text = unitVerdictText({ level: 'note', reason, a: 2, b: 16 }, ALPHABET);
    if (reason !== 'working') assert.ok(text.todo, reason + ' says nothing to do about it');
  }
  const cure = (reason) => unitVerdictText({ level: 'fault', reason, a: 1, b: 0 }, ALPHABET).cure;
  assert.equal(cure('wrong-protocol'), 'firmware');
  assert.equal(cure('held-in-bootloader'), 'firmware');
  assert.equal(cure('in-bootloader'), 'firmware');
  assert.equal(cure('firmware-outdated'), 'firmware');
  assert.equal(cure('bootloader-outdated'), 'boot-update');
  assert.equal(cure('home-failed'), 'home');
  assert.equal(cure('jammed'), 'home');
  assert.equal(cure('wrong-letter'), 'home');
  // An update refuses a boot section it does not know: nothing to press.
  assert.equal(cure('bootloader-damaged'), undefined);
  assert.equal(cure('low-supply'), undefined);
});

test('what is out of the ordinary is the reason that leads and every other that applies', () => {
  assert.deepEqual(unitConcerns({ verdict: { level: 'working', reason: 'working', a: 60, b: 0, also: [] } }, ALPHABET), []);
  assert.deepEqual(unitConcerns({}, ALPHABET), []);
  const unit = { state: 'running',
    verdict: { level: 'fault', reason: 'home-failed', a: 4, b: 0, also: ['hall-never', 'low-supply', 'dragging', 'worn'] },
    power: { supplyMinMv: 4210 }, drum: { homeExcessStepsMax: 61, turns: 48211 } };
  const concerns = unitConcerns(unit, ALPHABET);
  assert.deepEqual(concerns.map((c) => [c.cls, c.title]), [
    ['bad', 'Cannot find home'], ['bad', 'Never saw its home sensor'], ['note', 'Low supply'],
    ['note', 'Drum drags'], ['note', 'Many turns']]);
  // The numbers of a reason that does not lead are the unit's own facts; a
  // limit only the master knows is left out, not made up.
  assert.equal(concerns[2].why, 'Lowest supply since it started: 4.21 V.');
  assert.equal(concerns[3].why, 'It needed 61 steps more than expected to reach home.');
  assert.match(concerns[4].why, /^48211 turns/);
  for (const c of concerns) assert.ok(c.todo);
  const before = unitConcerns({ verdict: { level: 'note', reason: 'worn', a: 9, b: 0, also: ['home-failed-before'] },
                                drum: { homeFailures: 9 } }, ALPHABET)[1];
  assert.equal(before.why, '9 times over its lifetime. It is at home now.');
});

test('a button is on a unit page only when the unit can do it', () => {
  const can = (state, reason) => unitCan({ state, verdict: { reason } });
  assert.deepEqual(can('running', 'working'), { act: true, firmware: true });
  assert.deepEqual(can('running', 'jammed'), { act: true, firmware: true });
  assert.deepEqual(can('running', 'not-answering'), { act: false, firmware: false });
  assert.deepEqual(can('silent', 'no-unit'), { act: false, firmware: false });
  assert.deepEqual(can('running', 'wrong-protocol'), { act: false, firmware: true });
  assert.deepEqual(can('bootloader', 'in-bootloader'), { act: false, firmware: true });
  assert.deepEqual(can('bootloader', 'held-in-bootloader'), { act: false, firmware: true });
  assert.deepEqual(can('bootloader', 'being-updated'), { act: false, firmware: false });
  assert.deepEqual(unitCan({ state: 'running' }), { act: true, firmware: true });
});

test('a flap too far is corrected by stopping a flap of steps earlier', () => {
  // 2038 steps a turn, 45 flaps: 45.3 steps a flap.
  assert.deepEqual(correctedOffset(70, 1, 2, 45, 2038, 2038), { offset: 25 });
  assert.deepEqual(correctedOffset(70, 2, 1, 45, 2038, 2038), { offset: 115 });
  assert.deepEqual(correctedOffset(70, 5, 5, 45, 2038, 2038), { offset: 70 });
});

test('the correction goes the shorter way round the drum', () => {
  // Should show the last flap, shows the blank one: one flap too far, not 44 short.
  assert.deepEqual(correctedOffset(0, 44, 0, 45, 2038, 2038), { offset: -45 });
  assert.deepEqual(correctedOffset(0, 0, 44, 45, 2038, 2038), { offset: 45 });
});

test('a correction past what a unit accepts is refused in words', () => {
  // 22 flaps too far is 996 steps earlier.
  assert.deepEqual(correctedOffset(1900, 0, 22, 45, 2038, 2038), { offset: 904 });
  assert.deepEqual(correctedOffset(-1900, 0, 22, 45, 2038, 2038),
    { error: 'That needs an offset of -2896 steps; a unit takes -2038 to 2038.' });
});

test('a self-test result reads as a sentence', () => {
  assert.equal(selfTestText({ state: 'ok', steps_per_rev: 2050, hall_window: 49, rev_time_ms: 6039 }),
    'Self-test passed: 2050 steps a turn, home sensor 49 steps wide, 6.0 s a turn.');
  assert.equal(selfTestText({ state: 'failed', reason: 'timeout', unit_reason: 'no-hall' }),
    'The self-test failed: no-hall.');
  assert.equal(selfTestText(undefined), 'The self-test failed.');
});

const FIRMWARE = {
  master: { id: 'wall-master', rev: 'aaa1111' },
  rescue: { rev: 'f8da0fa', state: 'stale', warn: false },
  rowImage: { rev: 'aaa1111', size: 325351, packed: true },
  boards: [
    { id: 'wall-master', kind: 'master', rev: 'aaa1111', current: true,
      units: { total: 16, current: 16, outdated: 0, unknown: 0 } },
    { id: 'wall-row', kind: 'row', rev: 'aaa1111', current: true, updateBlocked: false,
      units: { total: 5, current: 5, outdated: 0, unknown: 0 } },
  ],
  units: { shouldBe: '68b94c5', total: 21, current: 21, outdated: 0, unknown: 0 },
  bootloaders: { shouldBe: '506b3970', total: 21, ok: 21, outdated: 0, damaged: 0, unread: 0 },
};

test('md5 matches the reference for every length around a block edge', () => {
  for (const length of [0, 1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 1000, 70000]) {
    const bytes = Uint8Array.from({ length }, (_, i) => (i * 31 + length) & 255);
    assert.equal(md5Hex(bytes), createHash('md5').update(bytes).digest('hex'), `length ${length}`);
  }
});

test('a wall that is up to date says so line by line', () => {
  const rows = firmwareRows(FIRMWARE);
  assert.deepEqual(rows.map((r) => [r.what, r.shouldBe, r.is, r.state]), [
    ['Master firmware', 'no release known', 'aaa1111', 'running'],
    ['Row board firmware', 'aaa1111', '1 of 1 row board', 'current'],
    ['Stored image for row boards', 'no release known', 'aaa1111', 'the master’s build'],
    ['Unit firmware', '68b94c5', '21 of 21 units', 'current'],
    ['Unit bootloader', '506b3970', '21 of 21 units intact', 'current'],
    ['Rescue image (master)', 'no release known', 'f8da0fa', 'older, fine'],
  ]);
  assert.deepEqual(rows.flatMap((r) => r.actions), []);
  assert.deepEqual(firmwareVerdict(rows), { cls: 'ok', title: 'Everything is up to date', why: '' });
  assert.deepEqual(firmwareJobs(FIRMWARE), []);
});

test('a master ahead of the release, with everything else current, is up to date', () => {
  // The wall after a master-only fix on top of a release (#587).
  const fw = structuredClone(FIRMWARE);
  fw.master.rev = fw.boards[0].rev = 'ccc3333';
  fw.release = { state: 'up-to-date', tag: 'v2026.10.10', master: 'aaa1111', rowImage: 'aaa1111', rescue: 'aaa1111' };
  const rows = firmwareRows(fw);
  assert.deepEqual(rows.find((r) => r.what === 'Stored image for row boards'),
    { what: 'Stored image for row boards', shouldBe: 'aaa1111', is: 'aaa1111', cls: 'ok', state: 'release v2026.10.10',
      note: 'Kept on the master, 318 KB packed. The row boards install this one.', actions: [] });
  assert.deepEqual([rows[0].shouldBe, rows[0].is, rows[0].cls, rows[0].state, rows[0].note],
    ['aaa1111', 'ccc3333', 'ok', 'newer than the release',
     'Runs a build newer than the newest release, v2026.10.10. Nothing to do.']);
  assert.equal(firmwareVerdict(rows).title, 'Everything is up to date');
  // On the release itself the row reads current.
  fw.release.master = 'ccc3333';
  assert.equal(firmwareRows(fw)[0].state, 'current');
  // A stored image that is nobody's release is as good while the row boards run it.
  fw.release = undefined;
  assert.equal(firmwareRows(fw)[2].state, 'another build, fine');
  assert.equal(firmwareVerdict(firmwareRows(fw)).cls, 'ok');
});

test('what is behind is counted, and what can be started about it is offered', () => {
  const fw = structuredClone(FIRMWARE);
  fw.boards[1].current = false;
  fw.boards[1].updateBlocked = true;
  fw.boards[0].units.outdated = 3;
  fw.units = { shouldBe: '68b94c5', total: 17, current: 13, outdated: 3, unknown: 1 };
  fw.bootloaders.damaged = 1;
  fw.rowImage.rev = 'bbb2222';
  const rows = firmwareRows(fw);
  assert.deepEqual(rows.map((r) => [r.cls, r.state]), [
    ['ok', 'running'], ['note', '1 board behind'], ['ok', 'another build, fine'], ['note', '3 units behind'],
    ['bad', '1 unit damaged'], ['ok', 'older, fine']]);
  assert.equal(rows[1].shouldBe, 'bbb2222');
  assert.match(rows[1].todo, /failed to install three times/);
  assert.deepEqual(rows.map((r) => r.actions.map((a) => [a.type, a.id, a.label, !!a.primary])), [
    [], [['job', 'wall-row', 'Offer the stored image again', false]], [],
    [['job', 'wall-master', 'Update 3 units', true]], [], []]);
  assert.deepEqual(firmwareVerdict(rows), { cls: 'bad', title: 'Something needs installing',
    why: 'Row board firmware: 1 board behind; Unit firmware: 3 units behind; Unit bootloader: 1 unit damaged.' });
  assert.deepEqual(firmwareJobs(fw).map((j) => [j.id, j.name, j.target, j.label]), [
    ['wall-master', 'update-units', { row: '' }, 'Update 3 units'],
    ['wall-row', 'update', { row: 'wall-row' }, 'Offer the stored image again']]);
  fw.boards[1].updateBlocked = false;
  assert.match(firmwareRows(fw)[1].todo, /^Nothing to do/);
});

test('without a stored image the row boards are not called behind', () => {
  const fw = structuredClone(FIRMWARE);
  fw.rowImage = undefined;
  delete fw.boards[1].current;
  assert.deepEqual(firmwareRows(fw).slice(1, 3).map((r) => [r.what, r.is, r.cls, r.state]), [
    ['Row board firmware', 'aaa1111', 'ok', 'running'],
    ['Stored image for row boards', 'none', 'note', 'missing']]);
  // One a release stored waits for the master to run that release.
  fw.rowImageHeld = { rev: 'bbb2222', until: 'bbb2222' };
  const held = firmwareRows(fw)[2];
  assert.deepEqual([held.is, held.cls, held.state], ['bbb2222', 'note', 'waiting']);
  assert.match(held.note, /until the master runs bbb2222/);
});

test('a newer release fills what should run, and its update is the one thing that stands out', () => {
  const fw = structuredClone(FIRMWARE);
  fw.release = { state: 'newer', tag: 'v2026.10.12', master: 'bbb2222', rowImage: 'bbb2222', rescue: 'f8da0fa' };
  fw.boards[0].units.outdated = 3;
  fw.boards[0].bootloaders = { damaged: 1, outdated: 2 };
  fw.bootloaders.damaged = 1;
  const rows = firmwareRows(fw);
  assert.deepEqual([rows[0].shouldBe, rows[0].cls, rows[0].state], ['bbb2222', 'note', 'update available']);
  assert.equal(rows[0].todo, 'Release v2026.10.12 is newer. Image for row boards: aaa1111 becomes bbb2222; '
    + 'the row boards install it once the master runs bbb2222.');
  assert.deepEqual(rows[0].actions, [{ type: 'release', tag: 'v2026.10.12', primary: true }]);
  assert.deepEqual([rows[2].shouldBe, rows[5].shouldBe], ['bbb2222', 'f8da0fa']);
  // The units' update is offered, and does not stand out next to the release.
  assert.deepEqual(rows[3].actions.map((a) => [a.name, !!a.primary]), [['update-units', false]]);
  // A bootloader that is not intact leads to the page of its board.
  assert.deepEqual(rows[4].actions, [{ type: 'link', id: 'wall-master', label: '1 unit damaged, 2 units behind' }]);
  // What mends a red row comes before the release.
  fw.rescue = { rev: '', state: 'missing', warn: true };
  const red = firmwareRows(fw);
  assert.deepEqual(red[5].actions, [{ type: 'file', label: 'Choose rescue-….bin…', primary: true }]);
  assert.equal(red[0].actions[0].primary, undefined);
  assert.equal(red.flatMap((r) => r.actions).filter((a) => a.primary).length, 1);
});

test('every line that is not green says what to do, and the headline names it', () => {
  const shapes = [
    { rescue: { rev: '', state: 'missing', warn: true } },
    { rowImage: undefined },
    { rowImage: undefined, rowImageHeld: { rev: 'bbb2222', until: 'bbb2222' } },
    { units: { shouldBe: '68b94c5', total: 21, current: 18, outdated: 3, unknown: 0 } },
    { units: { shouldBe: '68b94c5', total: 21, current: 19, outdated: 0, unknown: 2 } },
    { bootloaders: { shouldBe: '506b3970', total: 21, ok: 20, outdated: 0, damaged: 1, unread: 0 } },
    { bootloaders: { shouldBe: '506b3970', total: 21, ok: 20, outdated: 1, damaged: 0, unread: 0 } },
    { bootloaders: { shouldBe: '506b3970', total: 21, ok: 20, outdated: 0, damaged: 0, unread: 1 } },
    { boards: [FIRMWARE.boards[0], { ...FIRMWARE.boards[1], current: false }] },
    { boards: [FIRMWARE.boards[0], { ...FIRMWARE.boards[1], current: false, updateBlocked: true }] },
  ];
  for (const shape of shapes) {
    const rows = firmwareRows({ ...FIRMWARE, ...shape });
    const cause = rows.filter((r) => r.cls !== 'ok');
    assert.equal(cause.length, 1, JSON.stringify(shape));
    assert.ok(cause[0].todo, `${cause[0].what} is ${cause[0].state} and says nothing to do about it`);
    const verdict = firmwareVerdict(rows);
    assert.equal(verdict.cls, cause[0].cls);
    assert.equal(verdict.why, `${cause[0].what}: ${cause[0].state}.`);
  }
});
test('a firmware file is known by the name the build gives it', () => {
  assert.deepEqual(firmwareFile('firmware-9618d55-master.bin'),
    { kind: 'master', rev: '9618d55', route: '/firmware/master', label: 'the master’s firmware' });
  assert.equal(firmwareFile('follower-de38289-gz.bin').route, '/firmware/row');
  assert.equal(firmwareFile('follower-de38289.bin').kind, 'row');
  assert.equal(firmwareFile('rescue-f8da0fa.bin').route, '/firmware/rescue');
  assert.equal(firmwareFile('firmware-9618d55-dirty-master.bin').rev, '9618d55-dirty');
  assert.equal(firmwareFile('firmware.bin'), null);
  assert.equal(firmwareFile('firmware-9618d55-master.bin.txt'), null);
  assert.equal(firmwareFile('holiday.jpg'), null);
});

test('the question before installing says what will happen', () => {
  assert.match(installQuestion(firmwareFile('firmware-9618d55-master.bin')), /^Install 9618d55 on the master\? It restarts/);
  assert.match(installQuestion(firmwareFile('follower-de38289-gz.bin')), /row boards/);
  assert.match(installQuestion(firmwareFile('rescue-f8da0fa.bin')), /not touched/);
});

test('when an entry was written is said as near as it needs to be', () => {
  const now = new Date(2026, 9, 7, 21, 0);  // Wednesday 7 October 2026
  const at = (...parts) => Math.floor(new Date(...parts).getTime() / 1000);
  assert.equal(whenText(at(2026, 9, 7, 18, 5), now), 'Today 18:05');
  assert.equal(whenText(at(2026, 9, 7, 0, 0), now), 'Today 00:00');
  assert.equal(whenText(at(2026, 9, 6, 23, 59), now), 'Tue 23:59');
  assert.equal(whenText(at(2026, 9, 1, 9, 30), now), 'Thu 09:30');
  assert.equal(whenText(at(2026, 8, 30, 9, 30), now), '30 Sep 09:30');
  assert.equal(whenText(undefined, now), 'no clock');
});

test('an entry of the record reads as what happened', () => {
  const say = (event) => eventText(event, 'Row 1', ALPHABET);
  assert.deepEqual(say({ kind: 'unit-reason-on', unit: 3, reason: 'not-answering', a: 21, b: 6 }),
    { cls: 'note', title: 'Row 1, unit 3: not answering', why: 'No reply for 21 s, 6 reads missed.' });
  assert.deepEqual(say({ kind: 'unit-reason-off', unit: 3, reason: 'not-answering', a: 0, b: 0 }),
    { cls: 'ok', title: 'Row 1, unit 3: no longer “not answering”', why: '' });
  assert.equal(say({ kind: 'board-reason-on', unit: 0, reason: 'bus-dead', a: 6, b: 0 }).title, 'Row 1: unit bus dead');
  assert.equal(say({ kind: 'board-reason-off', unit: 0, reason: 'bus-dead' }).title, 'Row 1: no longer “unit bus dead”');
  assert.deepEqual(say({ kind: 'job-failed', unit: 3, job: 'home' }), { cls: 'bad', title: 'Row 1, unit 3: home failed', why: '' });
  assert.equal(say({ kind: 'job-done', unit: 0, job: 'home-all' }).title, 'Row 1: home-all done');
  assert.deepEqual(say({ kind: 'unit-restarted', unit: 2, cause: 'brownout', a: 3, b: 1 }),
    { cls: 'note', title: 'Row 1, unit 2: restarted (brownout)',
      why: 'Over its lifetime: 3 restarts from low voltage, 1 watchdog reset.' });
});

test('a start names the firmware that started', () => {
  assert.deepEqual(eventText({ kind: 'master-started', a: 0xebb6f64, detail: 3 }, 'The master', ALPHABET),
    { cls: 'info', title: 'The master started', why: 'Firmware ebb6f64.' });
  assert.equal(eventText({ kind: 'row-started', a: 0x0e38289, detail: 0 }, 'Row 1', ALPHABET).why, 'Firmware 0e38289.');
  assert.equal(eventText({ kind: 'row-started', a: 1, detail: 1 }, 'Row 1', ALPHABET).title, 'Row 1: connected in rescue mode');
});

test('what a row board reports about itself is put in words, and a new code by its name', () => {
  const say = (event) => eventText({ kind: 'row-event', unit: 0, ...event }, 'Row 1', ALPHABET);
  assert.equal(say({ event: 'self-restart', a: 1, b: 2 }).why, 'Because its unit bus was dead; 2 times so far.');
  assert.equal(say({ event: 'low-memory', a: 5120 }).why, 'Largest free block 5120 bytes.');
  assert.equal(say({ event: 'started', b: 1 }).why, '1 start since its power came on.');
  assert.equal(say({ event: '?' }).title, 'Row 1: ?');
  assert.equal(eventText({ kind: 'brand-new', unit: 0 }, 'Row 1', ALPHABET).title, 'Row 1: brand-new');
  assert.equal(eventText({ kind: 'events-dropped', a: 1 }, '', ALPHABET).title, '1 entry was lost');
  assert.equal(eventText({ kind: 'events-dropped', a: 4 }, '', ALPHABET).title, '4 entries were lost');
});

test('the record names the master\u2019s own row "" and no board for the master itself', () => {
  assert.equal(eventBoardId({ board: '' }, 'wall-master'), 'wall-master');
  assert.equal(eventBoardId({ board: 'wall-row' }, 'wall-master'), 'wall-row');
  assert.equal(eventBoardId({}, 'wall-master'), null);
});

const TWO_ROWS = { master: { id: 'wall-master' }, rows: [
  { id: '', own: true, row: 1, col: 0, width: 16 },
  { id: 'wall-row', own: false, row: 0, col: 0, width: 5 }] };

test('the rows are listed top to bottom, counted from 1, and saved counted from 0', () => {
  const draft = rowsDraft(TWO_ROWS);
  assert.deepEqual(draft.map((line) => [line.name, line.id, line.row, line.col, line.width]),
    [['wall-row', 'wall-row', 1, 1, 5], ['wall-master', '', 2, 1, 16]]);
  assert.deepEqual(arrangeArgs(draft), { rows: [
    { id: 'wall-row', row: 0, col: 0, width: 5 }, { id: '', row: 1, col: 0, width: 16 }] });
});

test('row numbers with a gap or in another order become rows without a gap', () => {
  const draft = rowsDraft(TWO_ROWS);
  draft[0].row = 7;
  draft[1].row = 3;
  draft[0].col = 6;
  assert.deepEqual(arrangeArgs(draft).rows.map((r) => [r.id, r.row, r.col]), [['wall-row', 1, 5], ['', 0, 0]]);
});

test('rows that cannot be saved say why', () => {
  const draft = rowsDraft(TWO_ROWS);
  assert.equal(arrangeProblem(draft), '');
  draft[0].row = 2;  // beside the master, over its first units
  assert.equal(arrangeProblem(draft), 'wall-row and wall-master would overlap on row 2.');
  draft[0].col = 17;  // to the right of it
  assert.equal(arrangeProblem(draft), '');
  draft[0].col = 0;
  assert.equal(arrangeProblem(draft), 'wall-row: the column is a whole number from 1.');
  draft[0].col = 1;
  draft[0].row = NaN;
  assert.equal(arrangeProblem(draft), 'wall-row: the row is a whole number from 1.');
  draft[0].row = 2;
  draft[0].width = 16;  // the same place as the master: shows the same text
  assert.equal(arrangeProblem(draft), '');
  draft[0].width = 0;
  assert.equal(arrangeProblem(draft), 'wall-row: the number of units is a whole number from 1.');
});

test('the time zone is named by the browser\u2019s zone when that has the wall\u2019s rule', () => {
  const zones = { 'Europe/Amsterdam': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Berlin': 'CET-1CEST,M3.5.0,M10.5.0/3',
                  'Etc/UTC': 'UTC0' };
  assert.equal(zoneFor(zones, 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Berlin'), 'Europe/Berlin');
  assert.equal(zoneFor(zones, 'CET-1CEST,M3.5.0,M10.5.0/3', 'America/New_York'), 'Europe/Amsterdam');
  assert.equal(zoneFor(zones, 'UTC0', 'Europe/Berlin'), 'Etc/UTC');
  assert.equal(zoneFor(zones, 'XYZ5', 'Europe/Berlin'), '');
});

test('the broker line says whether Home Assistant is reached, and where', () => {
  const mqtt = { host: '192.168.1.4', port: 1883, user: 'splitflap' };
  assert.deepEqual(brokerText(mqtt, true), { cls: 'ok', title: 'Connected', why: '192.168.1.4, port 1883, user splitflap' });
  assert.equal(brokerText(mqtt, false).title, 'Not connected');
  assert.equal(brokerText(mqtt, undefined).title, 'Checking');
  assert.equal(brokerText({ host: '10.0.0.2', port: 1884, user: '' }, true).why, '10.0.0.2, port 1884');
  assert.equal(brokerText({ host: '', port: 1883, user: '' }, false).title, 'Not set up');
});

test('a row board that was found is a line with what it said about itself', () => {
  assert.deepEqual(foundLine({ id: 'split-flap-aaaaaa', address: '192.168.1.51', rev: 'de38289', units: 5 }),
    { title: 'split-flap-aaaaaa', why: 'firmware de38289, at 192.168.1.51' });
  assert.equal(foundLine({ id: 'x', address: '192.168.1.51', rev: '', units: 0 }).why, 'at 192.168.1.51');
});

test('the test letter fills every row of the wall, as wide as the wall is there', () => {
  assert.equal(testText(composeLines(wallLayout(WALL)), 'A'), 'AAAA\nAAAA');
  assert.equal(testText([{ title: 'Row 1', width: 2 }], '8'), '88');
});

test('every marked unit gets the offset that turns what it shows into the test letter', () => {
  const marks = [
    { id: 'wall-row', title: 'Row 1', unit: 2, shows: 2, offset: 70 },
    { id: 'wall-master', title: 'Row 2', unit: 5, shows: 44, offset: 0 },
    { id: 'wall-row', title: 'Row 1', unit: 3, shows: 3, offset: 10 },
  ];
  // Should show A (place 1). 2038 steps a turn, 45 flaps.
  assert.deepEqual(calibrationPlan(marks, 1, 45, 2038, 2038), {
    sets: [{ id: 'wall-row', title: 'Row 1', unit: 2, offset: 25 },
           { id: 'wall-master', title: 'Row 2', unit: 5, offset: 91 },
           { id: 'wall-row', title: 'Row 1', unit: 3, offset: -81 }],
    problems: [],
  });
});

test('a marked unit that cannot be corrected is named with the reason, the others go on', () => {
  const marks = [
    { id: 'a', title: 'Row 1', unit: 1, shows: 1, offset: 5 },
    { id: 'a', title: 'Row 1', unit: 2, shows: 2, offset: null },
    { id: 'a', title: 'Row 1', unit: 3, shows: 23, offset: -1900 },
    { id: 'a', title: 'Row 1', unit: 4, shows: 0, offset: 0 },
  ];
  assert.deepEqual(calibrationPlan(marks, 1, 45, 2038, 2038), {
    sets: [{ id: 'a', title: 'Row 1', unit: 4, offset: 45 }],
    problems: ['Row 1, unit 1 shows the test letter already.',
               'Row 1, unit 2: its offset could not be read.',
               'Row 1, unit 3: That needs an offset of -2896 steps; a unit takes -2038 to 2038.'],
  });
});

test('a unit says what it wrote down about the silences of its board', () => {
  const lines = (silences) => {
    const group = unitFacts({ state: 'running', link: { silences } }).find((g) => g.title === 'Over its lifetime');
    return group ? Object.fromEntries(group.rows.map((r) => [r[0], r.slice(1)])) : {};
  };
  // A record without a silence says so and nothing more.
  assert.deepEqual(lines({ count: 0, lastMinutes: 0, longestMinutes: 0, busRestarts: 0, unitRestarts: 0, nowMinutes: 0 }),
    { 'Times its board went silent': [0, null] });
  const dead = lines({ count: 2, lastMinutes: 120, longestMinutes: 200, busRestarts: 119, heardAfterBusRestart: 0,
                       unitRestarts: 1, lastSawTraffic: false, lastLineHeldLow: false, lastEnded: false,
                       lastRestartedUnit: true, nowMinutes: 0 });
  assert.deepEqual(dead['Times its board went silent'],
    [2, 'The last for 120 minutes, the longest for 200 minutes.']);
  assert.equal(dead['During the last silence'][0], 'no traffic');
  assert.match(dead['During the last silence'][1], /nothing on the lines at all.*still going on.*restarted itself/);
  assert.deepEqual(dead['Restarted its bus on silence'], [119, null]);
  assert.deepEqual(dead['Restarted itself on silence'], [1]);
  const deaf = lines({ count: 1, lastMinutes: 3, longestMinutes: 3, busRestarts: 3, heardAfterBusRestart: 1,
                       unitRestarts: 0, lastSawTraffic: true, lastEnded: true, nowMinutes: 4 });
  assert.equal(deaf['During the last silence'][0], 'traffic for others');
  assert.equal(deaf['Restarted its bus on silence'][1], '1 time its board was heard again right after.');
  assert.deepEqual(unitFacts({ state: 'running', link: { silences: { count: 1, nowMinutes: 4 } } })
    .find((g) => g.title === 'Link to its board').rows, [['Silent now', '4 minutes']]);
  // A unit on older firmware has no record: no lines.
  assert.deepEqual(lines(undefined), {});
});

test('what a row board read off its dead unit bus reads as a sentence', () => {
  // 5 units had answered, lines free, 1 address acknowledged, 15 did not, last move 24 s before.
  const a = 0 | (1 << 4) | (15 << 9) | (0 << 14) | (5 << 19);
  const dead = eventText({ kind: 'row-event', event: 'bus-dead', a, b: 24 }, 'Row 1', ALPHABET);
  assert.equal(dead.cls, 'bad');
  assert.equal(dead.title, 'Row 1: read off its dead unit bus');
  assert.match(dead.why, /^5 units had answered before; both lines free; of 16 addresses 1 acknowledged, 15 did not, 0 could not be asked\. Its last move started .* before\.$/);
  const held = eventText({ kind: 'row-event', event: 'bus-dead', a: 3, b: 0xFFFF }, 'Row 1', ALPHABET);
  assert.match(held.why, /SDA held low/);
  assert.match(held.why, /It had not moved a flap since its start\.$/);
  const lines = eventText({ kind: 'row-event', event: 'bus-lines', a: 0xFFFF | (12 << 16), b: 10 | (11 << 16) }, 'Row 1', ALPHABET);
  assert.equal(lines.why, 'Dead: SDA no rise, SCL 1.2 µs. Working: SDA 1.0 µs, SCL 1.1 µs.');
  assert.equal(eventText({ kind: 'row-event', event: 'bus-lines', a: 0, b: 0 }, 'Row 1', ALPHABET).why,
    'Dead: SDA not measured, SCL not measured. Not measured on the working bus yet.');
});

const NOW = 1791630000000;
const RELEASE = { state: 'newer', check: true, lookedAt: 1791629400, channel: 'stable', tag: 'v2026.10.10',
                  notes: 'https://example.org/notes', commitTime: 1791619200,
                  master: 'bbb2222', rowImage: 'bbb2222', rescue: 'f8da0fa', unitRevs: '68b94c5,0d90815' };

test('a newer release says what an update would change', () => {
  const text = releaseText({ ...FIRMWARE, release: RELEASE }, NOW);
  assert.equal(text.title, 'New release v2026.10.10');
  assert.equal(text.canUpdate, true);
  assert.equal(text.why, 'Looked 10 minutes ago.');
  assert.deepEqual(text.changes, [
    'Master firmware: aaa1111 becomes bbb2222.',
    'Image for row boards: aaa1111 becomes bbb2222; the row boards install it once the master runs bbb2222.',
  ]);
  assert.match(updateQuestion(text), /^Update to v2026\.10\.10\? Master firmware: aaa1111 becomes bbb2222\./);
});

test('a release names the rescue image and the units only when they would change', () => {
  const fw = { ...FIRMWARE, rowImageHeld: { rev: 'bbb2222', until: 'bbb2222' }, rowImage: undefined,
               release: { ...RELEASE, rescue: 'ccc3333', unitRevs: 'ddd4444' } };
  assert.deepEqual(releaseText(fw, NOW).changes, [
    'Master firmware: aaa1111 becomes bbb2222.',
    'Rescue image: f8da0fa becomes ccc3333.',
    'Unit firmware: the units will read behind afterwards. Updating them stays a press of its own.',
  ]);
  // A release that does not say which unit firmware it counts as current says nothing about the units.
  assert.equal(releaseText({ ...FIRMWARE, release: { ...RELEASE, unitRevs: undefined } }, NOW).changes.length, 2);
});

test('every other answer of a look offers no update', () => {
  const say = (release) => releaseText({ ...FIRMWARE, release }, NOW);
  assert.equal(say(undefined).title, 'Not looked for a release yet');
  assert.equal(say({ state: 'not-looked', check: false }).why, 'The daily look is off.');
  const same = say({ ...RELEASE, state: 'up-to-date' });
  assert.deepEqual([same.cls, same.why, same.canUpdate], ['ok', 'The master runs a newer build. Looked 10 minutes ago.', undefined]);
  const failed = say({ state: 'failed', check: true, why: 'the signature does not check out', lookedAt: 1791629990 });
  assert.deepEqual([failed.cls, failed.why, failed.canUpdate],
    ['note', 'the signature does not check out. Looked just now.', undefined]);
});

test('an update says which image it is downloading and how far it is', () => {
  const at = (update) => releaseProgress({ release: { ...RELEASE, update } });
  assert.equal(releaseProgress({ release: RELEASE }), '');
  assert.equal(releaseProgress({}), '');
  assert.equal(at({ op: 3, step: 'looking', done: 0, size: 0 }), 'Looking at the release\u2026');
  assert.equal(at({ op: 3, step: 'master', done: 806192, size: 1612384 }), 'Downloading the master\u2019s firmware: 50 %');
  assert.equal(at({ op: 3, step: 'restarting', done: 0, size: 0 }), 'Restarting into the release\u2026');
});

test('the history says a release was found and an update started', () => {
  assert.deepEqual(eventText({ kind: 'release-found', a: 0x0bbb222 }, 'The master', ALPHABET),
    { cls: 'info', title: 'A newer release was found', why: 'Master firmware 0bbb222.' });
  assert.equal(eventText({ kind: 'update-started', a: 0x0bbb222 }, 'The master', ALPHABET).why,
    'To master firmware 0bbb222.');
});
