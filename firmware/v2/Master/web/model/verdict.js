// The master's verdicts in words. The master judges (UnitVerdict.h,
// BoardVerdict.h); this only says what a reason and its two numbers mean.
// Pure. A reason this page does not know is shown by its wire name.
import { dur, volt, plural, flapName } from './format.js';

const LEVEL_CLASS = { working: 'ok', note: 'note', fault: 'bad' };
const LETTER_CLASS = { w: 'ok', n: 'note', f: 'bad' };

export function levelClass(level) {
  return LEVEL_CLASS[level] || 'unknown';
}

// One letter of a board's "unitLevels".
export function letterClass(letter) {
  return LETTER_CLASS[letter] || 'unknown';
}

const hex8 = (n) => (n >>> 0).toString(16).padStart(8, '0');

const UNIT = {
  'working': (a) => ['Working', `Answering and homed. Running for ${dur(a)}.`],
  'no-unit': () => ['No unit here', 'Nothing answers at this place.'],
  // The count of missed reads stops at 255.
  'not-answering': (a, b) => ['Not answering',
    `No reply for ${dur(a)}, ${b >= 255 ? 'at least 255 reads' : plural(b, 'read')} missed.`],
  'held-in-bootloader': (a) => ['Held in its bootloader',
    `Its firmware failed at start ${plural(a, 'time')}, so the bootloader keeps it until it gets a new image.`],
  'in-bootloader': () => ['In its bootloader', 'It runs no firmware and waits for an image.'],
  'wrong-protocol': (a) => ['Speaks another protocol',
    `It reports version ${a} of the unit protocol. Update its firmware.`],
  'bootloader-damaged': (a) => ['Bootloader damaged',
    `Its boot section matches no known image (checksum ${hex8(a)}).`],
  'home-failed': (a) => ['Cannot find home', a
    ? `The last search for its home position failed; ${plural(a, 'failure')} over its lifetime.`
    : 'The last search for its home position failed.'],
  'hall-never': () => ['Never saw its home sensor',
    'The sensor has not triggered once since the unit started. Check the sensor and the magnet.'],
  'being-updated': () => ['Being updated', 'New firmware is being written to it.'],
  'finding-home': (a) => ['Finding home', a ? 'The drum is turning to its home position.'
    : 'It waits for its turn to find home.'],
  'jammed': () => ['Last move stalled', 'The drum did not arrive where it was sent.'],
  'wrong-letter': (a, b, alphabet) => ['Shows the wrong flap',
    `It stands at ${flapName(alphabet, a)}, not at the flap it was sent to.`],
  'low-supply': (a, b) => ['Low supply',
    `Lowest supply since it started: ${volt(a)}. Units warn below ${volt(b)}.`],
  'restarted-by-itself': (a, b) => ['Restarted by itself',
    `Over its lifetime: ${plural(a, 'restart')} from low voltage, ${plural(b, 'watchdog reset')}.`],
  'firmware-outdated': () => ['Firmware out of date', 'It runs an older unit firmware than the master holds.'],
  'bootloader-outdated': () => ['Bootloader out of date', 'It works, but a newer bootloader is available.'],
  'dragging': (a, b) => ['Drum drags',
    `It needed ${a} steps more than expected to reach home; the limit is ${b}.`],
  'hall-anomaly': (a) => ['Home sensor reads oddly',
    `${plural(a, 'edge')} seen in the last turn; one is expected.`],
  'worn': (a) => ['Many turns', `${a} turns of the drum, far more than the others on its row.`],
  'not-read': () => ['Not read yet', 'The master has not read this unit since it started.'],
  'home-failed-before': (a, b) => ['Failed to find home earlier',
    `${plural(a, 'time')} since the master started, ${b} over its lifetime. It is at home now.`],
};

// A third sentence says what to do about it, or that there is nothing to do.
const BOARD = {
  'working': (a, b) => ['Working', `${plural(a, 'unit')} found. Running for ${dur(b)}.`],
  'lost': (a) => ['Lost', `Not heard for ${dur(a)}.`,
    'Check that it has power and is in reach of the WiFi. It connects again by itself.'],
  'never-seen': () => ['Never seen', 'It has not connected since the master started.',
    'Check that it has power and is in reach of the WiFi. It connects by itself.'],
  'rescue': () => ['In rescue mode', 'It runs its rescue image and leaves its units alone until it is updated.',
    'Nothing to do while the master holds an image for row boards: it is offered that by itself. Firmware shows whether one is stored.'],
  'bus-dead': (a) => ['Unit bus dead',
    `None of its units answer. The bus has gone dead ${plural(a, 'time')} since the board started.`,
    'Check the cable from the board to its first unit and the units’ power. The board keeps asking, and the row comes back by itself.'],
  'update-blocked': (a) => ['Update blocked', `${plural(a, 'offer')} of the stored image failed.`,
    'Press “Offer the stored image again” below.'],
  'units-missing': (a, b) => ['Units missing', `${a} of ${plural(b, 'unit')} found.`,
    'Check the cable to the first place that does not answer; the page of that place says what the master sees.'],
  'units-fault': (a, b) => [a === 1 ? '1 unit has a fault' : `${a} units have a fault`,
    `${b - a} of ${plural(b, 'unit')} working.`, 'Open a unit marked red below: its page says what is wrong.'],
  'updating': () => ['Updating', 'It is installing new firmware.', 'Nothing to do: it is back in about a minute.'],
  'updating-units': () => ['Updating its units', 'Unit firmware is being written; the row shows nothing meanwhile.',
    'Nothing to do until it is done.'],
  'away': (a) => ['Away', `Not heard for ${dur(a)}; it may be restarting.`, 'Nothing to do yet.'],
  'units-unknown': () => ['Units not read yet', 'It has not sent its unit facts yet.', 'Nothing to do: they follow.'],
  'clock-not-set': () => ['Clock not set', 'It has no time yet, so its row may flip late.',
    'Nothing to do: it asks the master for the time by itself.'],
  'firmware-differs': () => ['Other firmware than the master holds for it', 'It will be offered the stored image.',
    'Nothing to do: the master offers it by itself.'],
  'units-note': (a, b) => ['Working, with notes', `All ${plural(b, 'unit')} answer, ${a} with a note.`,
    'Nothing to do: a unit marked amber below says what was noted.'],
};

export const BOARD_REASON_NAMES = Object.keys(BOARD);

function say(table, verdict, alphabet) {
  const words = table[verdict.reason];
  const [title, why, todo] = words ? words(verdict.a, verdict.b, alphabet) : [verdict.reason, ''];
  return { cls: levelClass(verdict.level), title, why, todo: todo || '' };
}

// {level, reason, a, b} -> {cls, title, why}
export function unitVerdictText(verdict, alphabet) {
  return say(UNIT, verdict, alphabet);
}

export function boardVerdictText(verdict) {
  return say(BOARD, verdict);
}

// The one line for the whole wall.
export function wallVerdictText(level, attention) {
  if (level === 'fault') {
    return { cls: 'bad', title: plural(attention, 'thing needs', 'things need') + ' attention' };
  }
  return { cls: 'ok', title: 'Everything is working' };
}
