// The wall's history in words, from GET /api/v2/history. Pure.
import { unitVerdictText, boardVerdictText, UNIT_FAULT_REASONS, BOARD_FAULT_REASONS } from './verdict.js';
import { plural, dur } from './format.js';

const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const two = (n) => String(n).padStart(2, '0');

// When an entry was written, as close as it needs to be said. `timeS` is
// Unix seconds (absent: the master's clock was not set), `now` a Date.
export function whenText(timeS, now) {
  if (!timeS) return 'no clock';
  const at = new Date(timeS * 1000);
  const clock = `${two(at.getHours())}:${two(at.getMinutes())}`;
  const midnight = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
  if (at.getTime() >= midnight) return 'Today ' + clock;
  if (at.getTime() >= midnight - 6 * 86400000) return `${DAYS[at.getDay()]} ${clock}`;
  return `${at.getDate()} ${MONTHS[at.getMonth()]} ${clock}`;
}

// A rev that was kept as a number.
const revText = (n) => (n >>> 0).toString(16).padStart(7, '0');

const SELF_RESTART = { 1: 'its unit bus was dead', 2: 'it ran out of memory' };

// Why the master started, by the chip's reset reason (esp_reset_reason_t):
// [words, whether it is a fault].
const MASTER_START = {
  1: ['Its power came on.'],
  3: ['It was restarted on purpose: an update or a restart that was asked for.'],
  4: ['Its firmware crashed.', true],
  5: ['It hung and its watchdog restarted it.', true],
  6: ['It hung and its watchdog restarted it.', true],
  7: ['It hung and its watchdog restarted it.', true],
  9: ['Its supply voltage dropped too low.', true],
  15: ['Its firmware crashed.', true],
};

// A job as what was done: [done, failed], after "<where>: ". A job on one
// unit or on a row's units reads differently under the same name.
const JOBS = {
  'home': ['found home', 'did not find home'],
  'home-all': ['all its units found home', 'not all its units found home'],
  'identify': ['blinked its light', 'did not blink its light'],
  'jog': ['was nudged', 'could not be nudged'],
  'set-offset': ['got a new offset', 'did not take a new offset'],
  'self-test': ['passed its self-test', 'failed its self-test'],
  'restart-unit': ['was restarted', 'could not be restarted'],
  'reset-odometer': ['its turn counter was reset', 'its turn counter could not be reset'],
  'set-gates': ['its feature switches were set', 'its feature switches could not be set'],
  'boot-info': ['its bootloader was read', 'its bootloader could not be read'],
  'boot-dump': ['its bootloader was copied out', 'its bootloader could not be copied out'],
  'boot-update': ['its bootloader was updated', 'updating its bootloader failed'],
  'set-address': ['its bus address was stored', 'its bus address could not be stored'],
  'clear-address': ['its stored bus address was cleared', 'its stored bus address could not be cleared'],
  'probe': ['looked for its units', 'could not look for its units'],
  'update': ['was offered the stored image again', 'could not be offered the stored image'],
};
const UNIT_OR_ROW_JOBS = {
  'update-units': [['its firmware was written', 'writing its firmware failed'],
                   ['its units were updated', 'updating its units failed']],
};
// Jobs of the wall as a whole, which no board did: whole sentences.
const WALL_JOBS = {
  'pair': ['A row board was added', 'Adding a row board failed'],
  'release': ['A row board was removed', 'Removing a row board failed'],
  'arrange': ['The rows were arranged', 'Arranging the rows failed'],
  'update-from-release': ['The update from a release is done', 'The update from a release failed'],
};

// A fault that ended, as what is so now.
const UNIT_OVER = {
  'no-unit': 'a unit answers here now',
  'not-answering': 'answers again',
  'held-in-bootloader': 'runs its firmware again',
  'in-bootloader': 'runs its firmware again',
  'wrong-protocol': 'speaks the master’s protocol now',
  'bootloader-damaged': 'its bootloader is whole again',
  'home-failed': 'found home again',
  'hall-never': 'its home sensor was seen',
};
const BOARD_OVER = {
  'lost': 'is back',
  'never-seen': 'connected',
  'rescue': 'left rescue mode',
  'bus-dead': 'its unit bus works again',
  'update-blocked': 'takes the stored image again',
  'units-missing': 'all its units answer again',
  'units-fault': 'none of its units has a fault now',
};
const over = (table, reason, title) => table[reason] || `“${title.toLowerCase()}” is over`;

// One entry as {cls, title, why}. `where` is what the entry's board is
// called (its place on the wall, or its id when it has left). What needs
// attention is 'bad'.
export function eventText(event, where, alphabet) {
  const unit = event.unit ? `${where}, unit ${event.unit}` : where;
  const verdict = { level: '', reason: event.reason, a: event.a, b: event.b };
  switch (event.kind) {
    case 'unit-reason-on': {
      const text = unitVerdictText(verdict, alphabet);
      return { cls: UNIT_FAULT_REASONS.includes(event.reason) ? 'bad' : 'note',
               title: `${unit}: ${text.title.toLowerCase()}`, why: text.why };
    }
    case 'unit-reason-off':
      return { cls: 'ok', why: '',
               title: `${unit}: ${over(UNIT_OVER, event.reason, unitVerdictText(verdict, alphabet).title)}` };
    case 'board-reason-on': {
      const text = boardVerdictText(verdict);
      return { cls: BOARD_FAULT_REASONS.includes(event.reason) ? 'bad' : 'note',
               title: `${where}: ${text.title.toLowerCase()}`, why: text.why };
    }
    case 'board-reason-off':
      return { cls: 'ok', why: '',
               title: `${where}: ${over(BOARD_OVER, event.reason, boardVerdictText(verdict).title)}` };
    case 'unit-restarted':
      return { cls: 'note', title: `${unit}: restarted (${event.cause || 'cause unknown'})`,
               why: `Over its lifetime: ${plural(event.a, 'restart')} from low voltage, ${plural(event.b, 'watchdog reset')}.` };
    case 'master-started': {
      const [why, fault] = MASTER_START[event.detail] || [];
      return { cls: fault ? 'bad' : 'info', title: 'The master started',
               why: (why ? why + ' ' : '') + `Firmware ${revText(event.a)}.` };
    }
    case 'row-started':
      // `starts` is there when historyGroups joined the board's own "started".
      return { cls: event.detail === 1 ? 'note' : 'info',
               title: `${where}: ` + (event.starts != null ? 'the board started and connected' : 'connected')
                 + (event.detail === 1 ? ' in rescue mode' : ''),
               why: `Firmware ${revText(event.a)}.`
                 + (event.starts != null ? ` ${plural(event.starts, 'start')} since its power came on.` : '') };
    case 'release-found':
      return { cls: 'info', title: 'A newer release was found', why: `Master firmware ${revText(event.a)}.` };
    case 'update-started':
      return { cls: 'info', title: 'An update from a release started', why: `To master firmware ${revText(event.a)}.` };
    case 'job-done':
    case 'job-failed': {
      const failed = event.kind === 'job-failed';
      const pick = (words) => words[failed ? 1 : 0];
      const split = UNIT_OR_ROW_JOBS[event.job];
      const words = split ? split[event.unit ? 0 : 1] : JOBS[event.job];
      const title = WALL_JOBS[event.job] ? pick(WALL_JOBS[event.job])
        : `${unit}: ` + (words ? pick(words) : `${event.job} ${failed ? 'failed' : 'done'}`);
      return { cls: failed ? 'bad' : 'info', title, why: '' };
    }
    case 'row-event':
      if (event.event === 'started') {
        return { cls: 'info', title: `${where}: the board started`,
                 why: `${plural(event.b, 'start')} since its power came on.` };
      }
      if (event.event === 'self-restart') {
        return { cls: 'note', title: `${where}: the board restarted itself`,
                 why: `Because ${SELF_RESTART[event.a] || 'of a fault'}; ${plural(event.b, 'time')} so far.` };
      }
      if (event.event === 'low-memory') {
        return { cls: 'note', title: `${where}: the board ran low on memory`,
                 why: `Largest free block ${event.a} bytes.` };
      }
      // What a row board read off its unit bus when it went dead; the fields
      // are FollowerBusDeath.h's.
      if (event.event === 'bus-dead') {
        const a = event.a;
        const lines = ['Both bus lines were free', 'The SCL line was held low', 'The SCL line was held low',
          'The SDA line was held low', 'The SDA line was held low'][a & 15] || 'Its bus lines were in an unknown state';
        const still = (a >> 4) & 31;
        const moved = event.b >= 0xFFFF ? 'It had not moved a flap since its start.'
          : `Its last move started ${dur(event.b)} before.`;
        return { cls: 'bad', title: `${where}: its unit bus went dead`,
                 why: `${still || 'None'} of the ${plural((a >> 19) & 31, 'unit')} that had answered still did. ${lines}. ${moved}` };
      }
      if (event.event === 'bus-lines') {
        const rise = (t) => (t === 0 ? 'not measured' : t === 0xFFFF ? 'no rise' : `${(t / 10).toFixed(1)} µs`);
        const pair = (v) => `SDA ${rise(v & 0xFFFF)}, SCL ${rise(v >>> 16)}`;
        return { cls: 'info', title: `${where}: measured its bus lines`,
                 why: `For whoever looks into the dead bus. Dead: ${pair(event.a)}. `
                   + (event.b ? `Working: ${pair(event.b)}.` : 'Not measured on the working bus yet.') };
      }
      return { cls: 'info', title: `${where}: ${event.event}`, why: '' };
    case 'events-dropped':
      return { cls: 'note', title: `${plural(event.a, 'entry was', 'entries were')} lost`,
               why: 'More happened at once than the record takes.' };
    default:
      return { cls: 'info', title: `${unit}: ${event.kind}`, why: '' };
  }
}

// The entry's board as a board id of the wall: the record names the master's
// own row "", and names no board for what the master did itself.
export function eventBoardId(event, masterId) {
  if (event.board === undefined || WALL_JOBS[event.job]) return null;
  return event.board === '' ? masterId : event.board;
}

// How long after a start what is recorded still counts as following from
// it: two rounds of unit reads, a board reading one unit every 3 s of 16.
const AFTER_START_S = 96;

// The record (newest first) as entries {event, after}: a start of the master
// carries everything recorded in the time after it, a start of a row board
// what was recorded on that board, and its own "started" and "connected"
// are one entry. `after` is newest first; an entry without a time belongs
// to no start.
export function historyGroups(events) {
  const top = [];
  let master = null;
  const rows = new Map();  // board id -> its open entry
  const open = (entry, time) => entry && time && time <= entry.until;
  for (const event of [...events].reverse()) {
    const entry = { event, after: [], until: event.time ? event.time + AFTER_START_S : 0 };
    if (event.kind === 'master-started') {
      master = entry;
      rows.clear();
      top.push(entry);
      continue;
    }
    if (event.kind === 'row-started') {
      // Its own "started" is the entry right before it, wherever that stands.
      const list = open(master, event.time) ? master.after : top;
      const before = list[list.length - 1];
      const started = list === top ? before && before.event : before;
      if (started && started.kind === 'row-event' && started.event === 'started' && started.board === event.board
          && event.time && started.time && event.time - started.time <= AFTER_START_S) {
        list.pop();
        entry.event = { ...event, starts: started.b };
      }
    }
    if (open(master, event.time)) {
      master.after.push(entry.event);
    } else if (event.kind !== 'row-started' && open(rows.get(event.board), event.time)) {
      rows.get(event.board).after.push(event);
    } else {
      top.push(entry);
      if (event.kind === 'row-started') rows.set(event.board, entry);
    }
  }
  for (const entry of top) entry.after.reverse();
  return top.reverse().map(({ event, after }) => ({ event, after }));
}

// Only what needs attention, every entry on its own.
export function needingAttention(events, alphabet) {
  return events.filter((event) => eventText(event, '', alphabet).cls === 'bad')
    .map((event) => ({ event, after: [] }));
}
