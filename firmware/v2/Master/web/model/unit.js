// A unit's page, from GET /api/v2/unit/<board>/<address>. Pure.
import { dur, volt, flapName, plural, count, yesNo } from './format.js';


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

// The unit's facts, grouped by the question they answer. Rows as on the
// board page: [label, value] or [label, value, a line of explanation].
export function unitFacts(unit) {
  const groups = [];
  const group = (title, rows) => {
    const kept = rows.filter((r) => r && r[1] != null);
    if (kept.length) groups.push({ title, rows: kept });
  };
  const power = unit.power || {};
  const link = unit.link || {};
  const drum = unit.drum || {};
  const firmware = unit.firmware || {};
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
      ['Times its board went silent, lifetime', s.count,
        s.count ? `The last for ${plural(s.lastMinutes, 'minute')}, the longest for ${plural(s.longestMinutes, 'minute')}.` : null],
      ['During the last silence', s.count ? (s.lastSawTraffic ? 'traffic for others' : 'no traffic') : null, last],
      ['Restarted its bus on silence', s.count ? s.busRestarts : null,
        s.heardAfterBusRestart ? `${plural(s.heardAfterBusRestart, 'time')} its board was heard again right after.` : null],
      ['Restarted itself on silence', s.count ? s.unitRestarts : null],
      ['Silent now', s.nowMinutes ? plural(s.nowMinutes, 'minute') : null],
    ];
  };
  const boot = unit.bootloader || {};
  const mv = (v) => (v != null ? volt(v) : null);
  group('Power', [
    ['Supply now', mv(power.supplyMv)],
    ['Lowest since it started', mv(power.supplyMinMv)],
    ['During its last move', mv(power.supplyDuringLastMoveMv)],
    ['Restarts from low voltage, lifetime', power.brownouts],
    ['Watchdog resets, lifetime', power.watchdogResets],
    ['Last start', power.lastStart,
      power.restartedWhileWatched ? 'It restarted by itself while the master was watching.' : null],
    ['Least free memory', power.freeRamMin != null ? power.freeRamMin + ' bytes' : null],
  ]);
  group('Link to its board', [
    ['Last heard', link.heardMsAgo != null ? dur(link.heardMsAgo / 1000) + ' ago' : null],
    ['Messages received', link.received != null ? count(link.received) : null],
    ['Replies sent', link.answered != null ? count(link.answered) : null],
    ['Reads missed in a row', link.missed],
    ['Failed exchanges', link.failed,
      link.failedMsAgo != null ? `The last one ${dur(link.failedMsAgo / 1000)} ago.` : null],
    ['Garbled commands, lifetime', link.badCommands],
    ['Repaired its own bus', link.selfRepairs],
    ...silenceLines(link.silences),
    ['Rescued from its bootloader', link.rescuedFromBootloader],
  ]);
  const test = drum.selfTest;
  group('Drum', [
    ['Home', drum.home ? drum.home.replace('-', ' ') : null,
      drum.homeSteps != null ? `The last search took ${plural(drum.homeSteps, 'step')}.` : null],
    ['Failed to find home, lifetime', drum.homeFailures],
    ['Steps past the expected home, last and worst', drum.homeExcessSteps != null
      ? `${drum.homeExcessSteps} and ${drum.homeExcessStepsMax}` : null,
      drum.homeExcessStepsEver != null ? `Worst over its lifetime: ${drum.homeExcessStepsEver}.` : null],
    ['Slipped and corrected itself', drum.slips,
      drum.slips ? `The last time by ${plural(drum.lastSlipSteps, 'step')}.` : null],
    ['Last move stalled', drum.jammed != null ? yesNo(drum.jammed) : null],
    ['Home sensor edges in the last turn', drum.hallEdgesLastTurn],
    ['Turns of the drum, lifetime', drum.turns != null ? count(drum.turns) : null],
    ['Offset', drum.offset != null ? plural(drum.offset, 'step') : null],
    ['Self-test, steps a turn', test ? `${test.firstStepsPerTurn} first, ${test.lastStepsPerTurn} last` : null],
    ['Self-test, home sensor width', test ? `${test.firstHallWindow} first, ${test.lastHallWindow} last` : null],
  ]);
  group('Firmware', [
    ['Unit firmware', firmware.rev ? `${firmware.rev} (${firmware.status})` : null],
    ['Running since', firmware.uptimeS != null ? dur(firmware.uptimeS) + ' ago' : null],
    ['Protocol', firmware.protocol,
      firmware.protocolSupported === false ? 'The master does not speak this version.' : null],
    ['Bootloader', boot.verdict, boot.crc32 ? `Its checksum is ${boot.crc32}.` : null],
    ['Address kept in its memory', unit.addressStored != null ? yesNo(unit.addressStored) : null,
      unit.addressStored === false ? 'It takes its address from its switches.' : null],
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
