// The wall's history in words, from GET /api/v2/history. Pure.
import { unitVerdictText, boardVerdictText } from './verdict.js';
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

// One entry as {cls, title, why}. `where` is what the entry's board is
// called (its place on the wall, or its id when it has left).
export function eventText(event, where, alphabet) {
  const unit = event.unit ? `${where}, unit ${event.unit}` : where;
  const verdict = { level: '', reason: event.reason, a: event.a, b: event.b };
  switch (event.kind) {
    case 'unit-reason-on': {
      const text = unitVerdictText(verdict, alphabet);
      return { cls: 'note', title: `${unit}: ${text.title.toLowerCase()}`, why: text.why };
    }
    case 'unit-reason-off':
      return { cls: 'ok', why: '',
               title: `${unit}: no longer “${unitVerdictText(verdict, alphabet).title.toLowerCase()}”` };
    case 'board-reason-on': {
      const text = boardVerdictText(verdict);
      return { cls: 'note', title: `${where}: ${text.title.toLowerCase()}`, why: text.why };
    }
    case 'board-reason-off':
      return { cls: 'ok', why: '',
               title: `${where}: no longer “${boardVerdictText(verdict).title.toLowerCase()}”` };
    case 'unit-restarted':
      return { cls: 'note', title: `${unit}: restarted (${event.cause || 'cause unknown'})`,
               why: `Over its lifetime: ${plural(event.a, 'restart')} from low voltage, ${plural(event.b, 'watchdog reset')}.` };
    case 'master-started':
      return { cls: 'info', title: 'The master started', why: `Firmware ${revText(event.a)}.` };
    case 'row-started':
      return { cls: event.detail === 1 ? 'note' : 'info',
               title: `${where}: connected` + (event.detail === 1 ? ' in rescue mode' : ''),
               why: `Firmware ${revText(event.a)}.` };
    case 'release-found':
      return { cls: 'info', title: 'A newer release was found', why: `Master firmware ${revText(event.a)}.` };
    case 'update-started':
      return { cls: 'info', title: 'An update from a release started', why: `To master firmware ${revText(event.a)}.` };
    case 'job-done':
      return { cls: 'info', title: `${unit}: ${event.job} done`, why: '' };
    case 'job-failed':
      return { cls: 'bad', title: `${unit}: ${event.job} failed`, why: '' };
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
        const lines = ['both lines free', 'SCL held low', 'SCL held low', 'SDA held low', 'SDA held low'][a & 15]
          || 'lines in an unknown state';
        const moved = event.b >= 0xFFFF ? 'It had not moved a flap since its start.'
          : `Its last move started ${dur(event.b)} before.`;
        return { cls: 'bad', title: `${where}: read off its dead unit bus`,
                 why: `${plural((a >> 19) & 31, 'unit')} had answered before; ${lines}; of 16 addresses `
                   + `${(a >> 4) & 31} acknowledged, ${(a >> 9) & 31} did not, ${(a >> 14) & 31} could not be asked. ${moved}` };
      }
      if (event.event === 'bus-lines') {
        const rise = (t) => (t === 0 ? 'not measured' : t === 0xFFFF ? 'no rise' : `${(t / 10).toFixed(1)} µs`);
        const pair = (v) => `SDA ${rise(v & 0xFFFF)}, SCL ${rise(v >>> 16)}`;
        return { cls: 'info', title: `${where}: how its bus lines rise`,
                 why: `Dead: ${pair(event.a)}. ` + (event.b ? `Working: ${pair(event.b)}.` : 'Not measured on the working bus yet.') };
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
  if (event.board === undefined) return null;
  return event.board === '' ? masterId : event.board;
}
