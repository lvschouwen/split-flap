// The page's pure model, run by `node --test` (under pytest: tests/test_web_page.py).
import test from 'node:test';
import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';

import { ALPHABET } from '../gen/constants.js';
import { dur, volt, listOf, flapName, plural } from '../model/format.js';
import { unitVerdictText, boardVerdictText, wallVerdictText, letterClass } from '../model/verdict.js';
import { boardUnits, tileLines, supplyBars, sparkPoints, boardFacts, startMarks } from '../model/board.js';
import { showsText, unitFacts, correctedOffset, selfTestText } from '../model/unit.js';
import { firmwareRows, firmwareVerdict, firmwareFile, installQuestion, firmwareJobs } from '../model/firmware.js';
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
  assert.deepEqual(low, { cls: 'bad', title: 'Low supply',
    why: 'Lowest supply since it started: 3.90 V. Units warn below 4.00 V.' });
  const wrong = unitVerdictText({ level: 'note', reason: 'wrong-letter', a: 30, b: 0 }, ALPHABET);
  assert.match(wrong.why, /stands at 0,/);
  assert.equal(wrong.cls, 'note');
});

test('a reason the page does not know is shown by its wire name', () => {
  const text = unitVerdictText({ level: 'note', reason: 'brand-new', a: 1, b: 2 }, ALPHABET);
  assert.deepEqual(text, { cls: 'note', title: 'brand-new', why: '' });
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
  const [working, , , silent] = boardUnits({ units: { fields: FIELDS, rows: [
    unitRow(1, 'working', 'working'), unitRow(2, 'note', 'jammed'), unitRow(3, 'fault', 'hall-never'),
    [4, 'fault', 'not-answering', 9, 9, 'running', null, null, null, null, null, null, 7, null]] } });
  assert.deepEqual(tileLines(working, ALPHABET), ['shows blank', '5.00 V']);
  assert.deepEqual(tileLines({ ...working, shows: 1 }, ALPHABET), ['shows A', '5.00 V']);
  assert.deepEqual(tileLines({ ...working, shows: null }, ALPHABET), ['flap unknown', '5.00 V']);
  assert.deepEqual(tileLines(silent, ALPHABET), ['no reply']);
  assert.deepEqual(tileLines({ ...working, state: 'bootloader' }, ALPHABET), ['in bootloader', '5.00 V']);
});

test('the supply chart marks the weakest unit and never draws an empty bar', () => {
  const units = [{ address: 1, supplyMinMv: 5300 }, { address: 2, supplyMinMv: 4650 },
                 { address: 3, supplyMinMv: 3900 }, { address: 4, supplyMinMv: null }];
  const chart = supplyBars(units);
  assert.equal(chart.lowestMv, 3900);
  assert.deepEqual(chart.bars.map((b) => [b.address, b.percent, b.lowest]),
                   [[1, 100, false], [2, 50, false], [3, 6, true]]);
  assert.deepEqual(supplyBars([]), { lowestMv: null, bars: [] });
});

test('a history ring becomes a line from left to right', () => {
  assert.equal(sparkPoints([0, 10], 300, 40), '0.0,36.0 300.0,4.0');
  assert.equal(sparkPoints([5, 5, 5], 300, 40), '0.0,36.0 150.0,36.0 300.0,36.0');
  assert.equal(sparkPoints([1], 300, 40), '');
});

test('the facts of the master and of a row board answer the same four questions', () => {
  const master = boardFacts({ kind: 'master', address: '10.0.0.2', rev: 'aaa1111', mqttConnected: true,
    stats: { now: { rssi: -52, txPower: 20, heap: 61440, minHeap: 36864, temp: 316, uptime: 5620,
                    i2cTx: 13780, i2cErr: 0, ntpAge: 5611 } },
    network: { gw: 'ok', self: 'fail' }, lastStart: { reset: 'Software reset', cause: 'asked for' },
    rescue: { rev: 'bbb2222', state: 'stale', warn: false } }, [{ address: 1, supplyMinMv: 4850 }]);
  assert.deepEqual(master.map((g) => g.title), ['Connection', 'Unit bus', 'Running', 'Firmware']);
  const find = (groups, label) => groups.flatMap((g) => g.rows).find((r) => r[0] === label);
  assert.deepEqual(find(master, 'Transmit power'), ['Transmit power', '5 dBm']);
  assert.deepEqual(find(master, 'Own web server answers'), ['Own web server answers', 'no']);
  assert.deepEqual(find(master, 'Exchanges since start'), ['Exchanges since start', '13,780']);
  assert.deepEqual(find(master, 'Chip temperature'), ['Chip temperature', '31.6 °C']);
  assert.deepEqual(find(master, 'Last start'), ['Last start', 'Software reset', 'asked for']);
  assert.deepEqual(find(master, 'Lowest unit supply'), ['Lowest unit supply', '4.85 V']);

  const row = boardFacts({ kind: 'row', pairedAt: '10.0.0.3', reach: 'up', heardMsAgo: 1400, rev: 'aaa1111',
    rescue: false, connects: 2, restarts: 1, lastLateMs: 0, worstLateMs: 12, updateAttempts: 0,
    status: { uptimeS: 5223, heap: 35000, heapMin: 32120, rssi: -57, txPowerDbm: 2, busTx: 12326,
              busErrors: 11514, busDead: true, busEpisodes: 6, escalations: 1, imageSize: 460752,
              timeSynced: true } }, []);
  assert.deepEqual(row.map((g) => g.title), ['Connection', 'Unit bus', 'Running', 'Firmware']);
  assert.deepEqual(find(row, 'Address'), ['Address', '10.0.0.3']);
  assert.deepEqual(find(row, 'Dead now'), ['Dead now', 'yes']);
  assert.deepEqual(find(row, 'Row flips late by'), ['Row flips late by', '0 ms', 'Worst since it connected: 12 ms.']);
  assert.equal(find(row, 'Lowest unit supply'), undefined);
});

test('a fact the board did not give is left out, not shown as zero', () => {
  const groups = boardFacts({ kind: 'row' }, []);
  assert.deepEqual(groups.flatMap((g) => g.rows), []);
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

test('a unit that was read for nothing has no groups, one read in full has four', () => {
  assert.deepEqual(unitFacts({ state: 'silent', firmware: {}, power: {}, link: {}, drum: {}, bootloader: {} }), []);
  const groups = unitFacts({
    addressStored: false,
    firmware: { rev: 'd360e2b', status: 'current', uptimeS: 182333, protocol: 1, protocolSupported: true },
    power: { brownouts: 2, watchdogResets: 1, lastStart: 'requested', restartedWhileWatched: false,
             supplyMv: 5091, supplyMinMv: 5091, freeRamMin: 1490 },
    link: { badCommands: 87, received: 29696, answered: 41254, heardMsAgo: 38222, missed: 0, failed: 0 },
    drum: { home: 'not-homed', homeSteps: 256, homeFailures: 0, slips: 0, jammed: true, turns: 1092, offset: 70,
            selfTest: { firstHallWindow: 38, lastHallWindow: 35, firstStepsPerTurn: 2057, lastStepsPerTurn: 2050 } },
    bootloader: { verdict: 'ok' } });
  assert.deepEqual(groups.map((g) => g.title), ['Power', 'Link to its board', 'Drum', 'Firmware']);
  const find = (label) => groups.flatMap((g) => g.rows).find((r) => r[0] === label);
  assert.deepEqual(find('Unit firmware'), ['Unit firmware', 'd360e2b (current)']);
  assert.deepEqual(find('Home'), ['Home', 'not homed', 'The last search took 256 steps.']);
  assert.deepEqual(find('Turns of the drum, lifetime'), ['Turns of the drum, lifetime', '1,092']);
  assert.deepEqual(find('Last move stalled'), ['Last move stalled', 'yes']);
  assert.deepEqual(find('Slipped and corrected itself'), ['Slipped and corrected itself', 0, null]);
  assert.deepEqual(find('Address kept in its memory'),
                   ['Address kept in its memory', 'no', 'It takes its address from its switches.']);
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
    ['Master firmware', '', 'aaa1111', 'running'],
    ['Row board firmware', 'aaa1111', '1 of 1 row board', 'current'],
    ['Stored image for row boards', 'aaa1111', 'aaa1111', 'matches'],
    ['Unit firmware', '68b94c5', '21 of 21 units', 'current'],
    ['Unit bootloader', '506b3970', '21 of 21 units intact', 'current'],
    ['Rescue image (master)', '', 'f8da0fa', 'older, fine'],
  ]);
  assert.deepEqual(firmwareVerdict(rows), { cls: 'ok', title: 'Everything is up to date' });
  assert.deepEqual(firmwareJobs(FIRMWARE), []);
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
    ['ok', 'running'], ['note', '1 board behind'], ['note', 'another build'], ['note', '3 units behind'],
    ['bad', '1 unit damaged'], ['ok', 'older, fine']]);
  assert.equal(rows[1].shouldBe, 'bbb2222');
  fw.rowImage = null;
  assert.equal(firmwareRows(fw)[2].state, 'missing');
  assert.equal(firmwareVerdict(rows).cls, 'bad');
  assert.deepEqual(firmwareJobs(fw).map((j) => [j.id, j.name, j.target, j.label]), [
    ['wall-master', 'update-units', { row: '' }, 'Update 3 units'],
    ['wall-row', 'update', { row: 'wall-row' }, 'Offer the stored image again']]);
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
    const group = unitFacts({ state: 'running', link: { silences } }).find((g) => g.title === 'Link to its board');
    return group ? Object.fromEntries(group.rows.map((r) => [r[0], r.slice(1)])) : {};
  };
  // A record without a silence says so and nothing more.
  assert.deepEqual(lines({ count: 0, lastMinutes: 0, longestMinutes: 0, busRestarts: 0, unitRestarts: 0, nowMinutes: 0 }),
    { 'Times its board went silent, lifetime': [0, null] });
  const dead = lines({ count: 2, lastMinutes: 120, longestMinutes: 200, busRestarts: 119, heardAfterBusRestart: 0,
                       unitRestarts: 1, lastSawTraffic: false, lastLineHeldLow: false, lastEnded: false,
                       lastRestartedUnit: true, nowMinutes: 0 });
  assert.deepEqual(dead['Times its board went silent, lifetime'],
    [2, 'The last for 120 minutes, the longest for 200 minutes.']);
  assert.equal(dead['During the last silence'][0], 'no traffic');
  assert.match(dead['During the last silence'][1], /nothing on the lines at all.*still going on.*restarted itself/);
  assert.deepEqual(dead['Restarted its bus on silence'], [119, null]);
  assert.deepEqual(dead['Restarted itself on silence'], [1]);
  const deaf = lines({ count: 1, lastMinutes: 3, longestMinutes: 3, busRestarts: 3, heardAfterBusRestart: 1,
                       unitRestarts: 0, lastSawTraffic: true, lastEnded: true, nowMinutes: 4 });
  assert.equal(deaf['During the last silence'][0], 'traffic for others');
  assert.equal(deaf['Restarted its bus on silence'][1], '1 time its board was heard again right after.');
  assert.deepEqual(deaf['Silent now'], ['4 minutes']);
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
