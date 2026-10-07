// The page's pure model, run by `node --test` (under pytest: tests/test_web_page.py).
import test from 'node:test';
import assert from 'node:assert/strict';

import { ALPHABET } from '../gen/constants.js';
import { dur, volt, listOf, flapName, plural } from '../model/format.js';
import { unitVerdictText, boardVerdictText, wallVerdictText, letterClass } from '../model/verdict.js';
import { wallLayout, boardTitle, attentionList, notesList, boardLine, boardId, noFlapFor,
         composeLines, composeText } from '../model/wall.js';

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
