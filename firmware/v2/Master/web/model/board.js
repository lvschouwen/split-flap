// A board's page, from GET /api/v2/board/<id>. Pure.
import { dur, volt, kb, flapName, count, yesNo } from './format.js';
import { unitVerdictText } from './verdict.js';

// The units table as objects.
export function boardUnits(board) {
  const table = (board && board.units) || {};
  const fields = table.fields || [];
  return (table.rows || []).map((row) => Object.fromEntries(fields.map((f, i) => [f, row[i]])));
}

// What a unit's tile says under its name: a unit with a fault says the fault.
export function tileLines(unit, alphabet) {
  const lines = [];
  if (unit.level === 'fault' && unit.reason) lines.push(unitVerdictText(unit, alphabet).title);
  else if (unit.state !== 'running') lines.push(unit.state === 'bootloader' ? 'in bootloader' : 'no reply');
  else if (unit.firmware == null) lines.push('no reply');
  else if (unit.shows != null) lines.push('shows ' + flapName(alphabet, unit.shows));
  else lines.push('flap unknown');
  if (unit.supplyMv != null) lines.push(volt(unit.supplyMv));
  return lines;
}

// The weakest supply any unit of the board has seen since it started:
// {mv, address}, or null when no unit gave one.
export function lowestSupply(units) {
  const read = units.filter((u) => u.supplyMinMv != null);
  if (!read.length) return null;
  const unit = read.reduce((low, u) => (u.supplyMinMv < low.supplyMinMv ? u : low));
  return { mv: unit.supplyMinMv, address: unit.address };
}

// How many of its units run other unit firmware than the board holds.
export function unitsBehind(units) {
  return units.filter((u) => u.firmware === 'outdated').length;
}

// What can be started on a board in the state its verdict gives: a board that
// is not there takes nothing, one on its rescue image leaves its units alone.
const OUT_OF_REACH = ['lost', 'never-seen', 'away'];
export function boardReach(board) {
  const reason = (board.verdict || {}).reason;
  if (OUT_OF_REACH.includes(reason)) {
    return { board: false, units: false, why: 'The board is out of reach: nothing can be started on it from here.' };
  }
  if (reason === 'rescue' || board.rescue === true) {
    return { board: true, units: false, why: 'On its rescue image the board leaves its units alone.' };
  }
  return { board: true, units: true, why: '' };
}

// Why a board last started, in the reader's words; what the chip said stays
// as the line under it.
export function startWords(reset, cause) {
  if (!reset) return null;
  const said = [reset, cause && !/unattributed/.test(cause) ? cause : null].filter(Boolean).join(': ');
  const words = /power/i.test(reset) ? 'The power was switched on'
    : /brown/i.test(reset) ? 'Its supply voltage dropped too low'
    : /panic|exception|abort/i.test(reset) ? 'Its firmware crashed'
    : /watchdog|wdt/i.test(reset) ? 'It hung and its watchdog restarted it'
    : /software|deep ?sleep|external/i.test(reset) ? 'It was restarted on purpose' : null;
  return words ? [words, `As the board says it: ${said}.`] : [said, null];
}

// The usual reading of a WiFi signal strength.
function signal(dbm) {
  return `${dbm} dBm, ${dbm >= -67 ? 'good' : dbm >= -75 ? 'fair' : 'weak'}`;
}

// A history ring as the points of a polyline in a w x h box, newest right.
export function sparkPoints(values, w, h) {
  if (!values || values.length < 2) return '';
  const min = Math.min(...values);
  const span = (Math.max(...values) - min) || 1;
  return values.map((v, i) =>
    `${(i / (values.length - 1) * w).toFixed(1)},${(h - 4 - (v - min) / span * (h - 8)).toFixed(1)}`)
    .join(' ');
}


// The facts of a board, grouped by the question they answer. A row is
// [label, value, a line of explanation, 'bad' when the value is plainly
// wrong]; a fact the board did not give is left out. Numbers nobody judges
// are in the last group, which the page keeps folded.
export function boardFacts(board, units) {
  const groups = [];
  const group = (title, rows, folded) => {
    const given = rows.filter((r) => r && r[1] != null);
    if (given.length || !folded) groups.push({ title, rows: given, folded: !!folded });
  };
  const low = lowestSupply(units);
  const supply = low && ['Lowest unit supply', `${volt(low.mv)} (unit ${low.address})`,
                         'The lowest any unit has seen since it started.'];
  const bad = (wrong) => (wrong ? 'bad' : undefined);
  if (board.kind === 'master') {
    const now = (board.stats && board.stats.now) || {};
    const net = board.network || {};
    group('Connection', [
      ['Address', board.address],
      ['WiFi signal', now.rssi != null ? signal(now.rssi) : null],
      ['Router reachable', net.gw ? yesNo(net.gw === 'ok') : null, null, bad(net.gw && net.gw !== 'ok')],
      ['Home Assistant (MQTT)', board.mqttConnected ? 'connected' : 'not connected'],
      ['Clock set from the network', now.ntpAge != null ? dur(now.ntpAge) + ' ago' : null],
    ]);
    group('Unit bus', [supply]);
    const last = board.lastStart || {};
    const start = startWords(last.reset, last.cause);
    group('Running', [
      ['Since', now.uptime != null ? dur(now.uptime) + ' ago' : null],
      start && ['Last start', start[0], start[1], bad(/crashed|hung|dropped/.test(start[0]))],
    ]);
    const rescue = board.rescue || {};
    group('Firmware', [
      ['Running', board.rev],
      ['Rescue image', rescue.rev || (rescue.warn ? 'none' : null), rescue.warn
        ? 'The rescue image should be installed again, from Firmware.'
        : rescue.state === 'stale' ? 'Older than the running firmware, which is fine.' : null, bad(rescue.warn)],
    ]);
    group('Counters', [
      ['Unit bus exchanges since start', now.i2cTx != null ? count(now.i2cTx) : null],
      ['of which failed', now.i2cErr != null ? count(now.i2cErr) : null],
      ['Free memory', now.heap != null ? kb(now.heap) : null],
      ['Lowest it has been', now.minHeap != null ? kb(now.minHeap) : null],
      ['Chip temperature', now.temp != null ? (now.temp / 10).toFixed(1) + ' °C' : null],
      ['Transmit power', now.txPower != null ? now.txPower / 4 + ' dBm' : null],
    ], true);
    return groups;
  }
  const status = board.status || {};
  group('Connection', [
    ['Address', board.address || board.pairedAt],
    ['Link to the master', board.reach, null, bad(board.reach && board.reach !== 'up')],
    ['Last heard', board.heardMsAgo != null ? dur(board.heardMsAgo / 1000) + ' ago' : null],
    ['WiFi signal', status.rssi != null ? signal(status.rssi) : null],
    ['Clock set', status.timeSynced != null ? yesNo(status.timeSynced) : null],
  ]);
  group('Unit bus', [
    ['Answers now', status.busDead != null ? yesNo(!status.busDead) : null, null, bad(status.busDead)],
    supply,
  ]);
  group('Running', [
    ['Since', status.uptimeS != null ? dur(status.uptimeS) + ' ago' : null],
    ['Row flips late by', board.lastLateMs != null ? board.lastLateMs + ' ms' : null,
      board.worstLateMs != null ? `Worst since it connected: ${board.worstLateMs} ms.` : null],
  ]);
  group('Firmware', [
    ['Running', board.rev, board.rescue ? 'This is its rescue image.' : null, bad(board.rescue)],
  ]);
  group('Counters', [
    ['Unit bus exchanges since start', status.busTx != null ? count(status.busTx) : null],
    ['of which failed', status.busErrors != null ? count(status.busErrors) : null],
    ['Times the bus went dead', status.busEpisodes],
    ['Restarts forced by faults', status.escalations],
    ['Restarts seen by the master', board.restarts],
    ['Connections since the master started', board.connects],
    ['Free memory', status.heap != null ? kb(status.heap) : null],
    ['Lowest it has been', status.heapMin != null ? kb(status.heapMin) : null],
    ['Transmit power', status.txPowerDbm != null ? status.txPowerDbm + ' dBm' : null],
    ['Image size', status.imageSize != null ? kb(status.imageSize) : null],
    ['Update offers that failed', board.updateAttempts],
  ], true);
  return groups;
}

// One square a start, oldest first: was the power cut, or did software ask?
export function startMarks(board) {
  return (board.starts || []).map((start) => ({
    cls: /power/i.test(start.reset) ? 'pwr' : /software/i.test(start.reset) ? '' : 'odd',
    title: start.reset + (start.stage && start.stage !== 'online' ? `, reached ${start.stage}` : ''),
  }));
}
