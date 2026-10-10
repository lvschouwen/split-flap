// A unit's page, from GET /api/v2/unit/<board>/<address>. Pure.
import { dur, volt, flapName, plural, count, yesNo } from './format.js';
import { unitVerdictText, UNIT_FAULT_REASONS } from './verdict.js';


// The flap the unit stands at, in a sentence.
export function showsText(unit, alphabet) {
  const drum = unit.drum || {};
  if (unit.state !== 'running') return 'It is not running, so what it shows is not known.';
  if (drum.shows == null) return 'Where its drum stands is not known.';
  const shows = flapName(alphabet, drum.shows);
  if (drum.wrongLetter && drum.commanded != null) {
    return `Shows ${shows}; it was sent to ${flapName(alphabet, drum.commanded)}.`;
  }
  return `Shows ${shows}, as it should.`;
}

// What is out of the ordinary about a unit: the reason its verdict leads
// with and every other that applies, each as {cls, title, why, todo, cure}.
// Nothing for a unit that works. The master gives its two numbers for the
// leading reason only; the others are said from the unit's own facts.
export function unitConcerns(unit, alphabet) {
  const verdict = unit.verdict;
  if (!verdict || verdict.reason === 'working') return [];
  const power = unit.power || {};
  const drum = unit.drum || {};
  const numbers = {
    'home-failed': [drum.homeFailures],
    'finding-home': [drum.home === 'homing' ? 1 : 0],
    'wrong-letter': [drum.shows],
    'low-supply': [power.supplyMinMv],
    'restarted-by-itself': [power.brownouts, power.watchdogResets],
    'dragging': [drum.homeExcessStepsMax],
    'hall-anomaly': [drum.hallEdgesLastTurn],
    'worn': [drum.turns],
    'home-failed-before': [0, drum.homeFailures],
    'bootloader-damaged': [parseInt((unit.bootloader || {}).crc32, 16)],
  };
  return [unitVerdictText(verdict, alphabet), ...(verdict.also || []).map((reason) => {
    const [a, b] = numbers[reason] || [];
    return unitVerdictText({ level: UNIT_FAULT_REASONS.includes(reason) ? 'fault' : 'note', reason,
                             a: a || 0, b: b || 0 }, alphabet);
  })];
}

// What can be asked of the unit: `act` for everything its own firmware does,
// `firmware` for writing its firmware (which a unit in its bootloader takes).
export function unitCan(unit) {
  const reason = (unit.verdict || {}).reason;
  const answers = unit.state === 'running' && reason !== 'not-answering';
  return { act: answers && reason !== 'wrong-protocol',
           firmware: answers || (unit.state === 'bootloader' && reason !== 'being-updated') };
}

const given = (rows) => rows.filter((r) => r && r[1] != null);

// The few facts of now that stand on the page. Rows as on the board page:
// [label, value] or [label, value, a line of explanation].
export function unitNow(unit) {
  const power = unit.power || {};
  const link = unit.link || {};
  const drum = unit.drum || {};
  const firmware = unit.firmware || {};
  return given([
    ['Supply', power.supplyMv != null ? volt(power.supplyMv) : null,
      power.supplyMinMv != null ? `Lowest since it started: ${volt(power.supplyMinMv)}.` : null],
    ['Last heard', link.heardMsAgo != null ? dur(link.heardMsAgo / 1000) + ' ago' : null],
    ['Home', drum.home ? drum.home.replace('-', ' ') : null],
    ['Firmware', firmware.rev ? `${firmware.rev} (${firmware.status})` : null],
    ['Offset', drum.offset != null ? plural(drum.offset, 'step') : null],
  ]).map((row) => (row[2] == null ? row.slice(0, 2) : row));
}

// Everything else the unit and its board report, grouped by the question it
// answers; what the unit counted over its whole life is the last group.
export function unitFacts(unit) {
  const groups = [];
  const group = (title, rows) => {
    const kept = given(rows);
    if (kept.length) groups.push({ title, rows: kept });
  };
  const power = unit.power || {};
  const link = unit.link || {};
  const drum = unit.drum || {};
  const firmware = unit.firmware || {};
  const boot = unit.bootloader || {};
  const steps = (n) => (n != null ? plural(n, 'step') : null);
  group('Power', [
    ['During its last move', power.supplyDuringLastMoveMv != null ? volt(power.supplyDuringLastMoveMv) : null],
    ['Last start', power.lastStart,
      power.restartedWhileWatched ? 'It restarted by itself while the master was watching.' : null],
    ['Least free memory', power.freeRamMin != null ? power.freeRamMin + ' bytes' : null],
  ]);
  const silences = link.silences;
  group('Link to its board', [
    ['Reads missed in a row', link.missed],
    ['Failed exchanges', link.failed,
      link.failedMsAgo != null ? `The last one ${dur(link.failedMsAgo / 1000)} ago.` : null],
    ['Messages received', link.received != null ? count(link.received) : null],
    ['Replies sent', link.answered != null ? count(link.answered) : null],
    ['Garbled commands', link.badCommands],
    ['Repaired its own bus', link.selfRepairs],
    ['Rescued from its bootloader', link.rescuedFromBootloader],
    ['Silent now', silences && silences.nowMinutes ? plural(silences.nowMinutes, 'minute') : null],
  ]);
  const test = drum.selfTest;
  const against = (last, first) => (last === first ? 'As at its first test.' : `At its first test: ${first}.`);
  group('Drum', [
    ['Last search for home', steps(drum.homeSteps)],
    ['Overshot home', steps(drum.homeExcessSteps),
      drum.homeExcessStepsMax != null ? `At worst since it started: ${steps(drum.homeExcessStepsMax)}.` : null],
    ['Slipped and corrected itself', drum.slips,
      drum.slips ? `The last time by ${plural(drum.lastSlipSteps, 'step')}.` : null],
    ['Last move stalled', drum.jammed != null ? yesNo(drum.jammed) : null],
    ['Home sensor edges in the last turn', drum.hallEdgesLastTurn],
    test && ['Self-test, steps a turn', test.lastStepsPerTurn, against(test.lastStepsPerTurn, test.firstStepsPerTurn)],
    test && ['Self-test, sensor width', test.lastHallWindow, against(test.lastHallWindow, test.firstHallWindow)],
  ]);
  group('Firmware', [
    ['Running since', firmware.uptimeS != null ? dur(firmware.uptimeS) + ' ago' : null],
    ['Protocol', firmware.protocol,
      firmware.protocolSupported === false ? 'The master does not speak this version.' : null],
    ['Bootloader', boot.verdict, boot.crc32 ? `Its checksum is ${boot.crc32}.` : null],
    ['Address kept in its memory', unit.addressStored != null ? yesNo(unit.addressStored) : null,
      unit.addressStored === false ? 'It takes its address from its switches.' : null],
  ]);
  // What the unit itself wrote down about the times nobody addressed it. It
  // keeps this across a power cycle, so it reads after a dead row is back.
  const silenceLines = (s) => {
    if (!s) return [];
    const last = !s.count ? null
      : (s.lastSawTraffic ? 'It saw traffic on the lines that was not for it.'
        : 'It saw nothing on the lines at all.')
        + (s.lastLineHeldLow ? ' A line was held low when it began.' : '')
        + (s.lastEnded ? '' : ' It was still going on when the unit lost power or restarted.')
        + (s.lastRestartedUnit ? ' The unit restarted itself in it.' : '');
    return [
      ['Times its board went silent', s.count,
        s.count ? `The last for ${plural(s.lastMinutes, 'minute')}, the longest for ${plural(s.longestMinutes, 'minute')}.` : null],
      ['During the last silence', s.count ? (s.lastSawTraffic ? 'traffic for others' : 'no traffic') : null, last],
      ['Restarted its bus on silence', s.count ? s.busRestarts : null,
        s.heardAfterBusRestart ? `${plural(s.heardAfterBusRestart, 'time')} its board was heard again right after.` : null],
      ['Restarted itself on silence', s.count ? s.unitRestarts : null],
    ];
  };
  group('Over its lifetime', [
    ['Restarts from low voltage', power.brownouts],
    ['Watchdog resets', power.watchdogResets],
    ['Failed to find home', drum.homeFailures],
    ['Worst overshoot of home', steps(drum.homeExcessStepsEver)],
    ['Turns of the drum', drum.turns != null ? count(drum.turns) : null],
    ...silenceLines(silences),
  ]);
  return groups;
}

// The offset that makes a unit standing at `showsPlace` stand at
// `shouldPlace`: a flap too far means stopping a flap's worth of steps
// earlier. The shorter way round the drum is taken. Returns {offset} or
// {error} when the result is past what a unit accepts.
export function correctedOffset(current, shouldPlace, showsPlace, flaps, stepsPerTurn, limit) {
  let delta = (showsPlace - shouldPlace) % flaps;
  if (delta > flaps / 2) delta -= flaps;
  if (delta < -flaps / 2) delta += flaps;
  const offset = current - Math.round(delta * stepsPerTurn / flaps);
  if (Math.abs(offset) > limit) {
    return { error: `That needs an offset of ${offset} steps; a unit takes ${-limit} to ${limit}.` };
  }
  return { offset };
}

// A self-test's result data in a sentence.
export function selfTestText(data) {
  if (!data || data.state !== 'ok') {
    const why = data && (data.unit_reason || data.reason);
    return 'The self-test failed' + (why ? ': ' + why : '') + '.';
  }
  return `Self-test passed: ${data.steps_per_rev} steps a turn, home sensor ${data.hall_window} steps wide, ` +
         `${(data.rev_time_ms / 1000).toFixed(1)} s a turn.`;
}
