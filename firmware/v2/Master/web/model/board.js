// A board's page, from GET /api/v2/board/<id>. Pure.
import { dur, volt, kb, flapName, count, yesNo } from './format.js';

// The units table as objects.
export function boardUnits(board) {
  const table = (board && board.units) || {};
  const fields = table.fields || [];
  return (table.rows || []).map((row) => Object.fromEntries(fields.map((f, i) => [f, row[i]])));
}

// What a unit's tile says under its name.
export function tileLines(unit, alphabet) {
  const lines = [];
  if (unit.state !== 'running') lines.push(unit.state === 'bootloader' ? 'in bootloader' : 'no reply');
  else if (unit.firmware == null) lines.push('no reply');
  else if (unit.shows != null) lines.push('shows ' + flapName(alphabet, unit.shows));
  else lines.push('flap unknown');
  if (unit.supplyMv != null) lines.push(volt(unit.supplyMv));
  return lines;
}

// The supply chart: a bar a unit for the lowest supply it has seen, on a
// scale that starts at the warning floor. `lowest` marks the weakest.
export const SUPPLY_FLOOR_MV = 4000;
export const SUPPLY_TOP_MV = 5300;
export function supplyBars(units) {
  const read = units.filter((u) => u.supplyMinMv != null);
  const lowest = Math.min(...read.map((u) => u.supplyMinMv));
  return {
    lowestMv: read.length ? lowest : null,
    bars: read.map((u) => ({
      address: u.address, mv: u.supplyMinMv, lowest: u.supplyMinMv === lowest,
      percent: Math.round(Math.min(100, Math.max(6,
        (u.supplyMinMv - SUPPLY_FLOOR_MV) / (SUPPLY_TOP_MV - SUPPLY_FLOOR_MV) * 100))),
    })),
  };
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
// [label, value] or [label, value, a line of explanation]; a fact the board
// did not give is left out.
export function boardFacts(board, units) {
  const groups = [];
  const group = (title, rows) => groups.push({ title, rows: rows.filter((r) => r && r[1] != null) });
  const supply = supplyBars(units).lowestMv;
  if (board.kind === 'master') {
    const now = (board.stats && board.stats.now) || {};
    const net = board.network || {};
    group('Connection', [
      ['Address', board.address],
      ['WiFi signal', now.rssi != null ? now.rssi + ' dBm' : null],
      ['Transmit power', now.txPower != null ? now.txPower / 4 + ' dBm' : null],
      ['Router reachable', net.gw ? yesNo(net.gw === 'ok') : null],
      ['Own web server answers', net.self ? yesNo(net.self === 'ok') : null],
      ['Home Assistant (MQTT)', board.mqttConnected ? 'connected' : 'not connected'],
      ['Clock set from the network', now.ntpAge != null ? dur(now.ntpAge) + ' ago' : null],
    ]);
    group('Unit bus', [
      ['Exchanges since start', now.i2cTx != null ? count(now.i2cTx) : null],
      ['Failed', now.i2cErr],
      ['Lowest unit supply', supply != null ? volt(supply) : null],
    ]);
    const last = board.lastStart || {};
    group('Running', [
      ['Since', now.uptime != null ? dur(now.uptime) + ' ago' : null],
      ['Last start', last.reset, last.cause],
      ['Free memory', now.heap != null ? kb(now.heap) : null],
      ['Lowest it has been', now.minHeap != null ? kb(now.minHeap) : null],
      ['Chip temperature', now.temp != null ? (now.temp / 10).toFixed(1) + ' °C' : null],
    ]);
    const rescue = board.rescue || {};
    group('Firmware', [
      ['Running', board.rev],
      ['Rescue image', rescue.rev, rescue.warn
        ? 'The rescue image should be installed again.'
        : rescue.state === 'stale' ? 'Older than the running firmware; nothing to do unless the rescue code changed.' : null],
    ]);
    return groups;
  }
  const status = board.status || {};
  group('Connection', [
    ['Address', board.address || board.pairedAt],
    ['Link to the master', board.reach === 'up' ? 'up' : board.reach],
    ['Last heard', board.heardMsAgo != null ? dur(board.heardMsAgo / 1000) + ' ago' : null],
    ['WiFi signal', status.rssi != null ? status.rssi + ' dBm' : null],
    ['Transmit power', status.txPowerDbm != null ? status.txPowerDbm + ' dBm' : null],
    ['Clock set', status.timeSynced != null ? yesNo(status.timeSynced) : null],
    ['Connections since the master started', board.connects],
  ]);
  group('Unit bus', [
    ['Exchanges since start', status.busTx != null ? count(status.busTx) : null],
    ['Failed', status.busErrors != null ? count(status.busErrors) : null],
    ['Dead now', status.busDead != null ? yesNo(status.busDead) : null],
    ['Times the bus went dead', status.busEpisodes],
    ['Restarts forced by faults', status.escalations],
    ['Lowest unit supply', supply != null ? volt(supply) : null],
  ]);
  group('Running', [
    ['Since', status.uptimeS != null ? dur(status.uptimeS) + ' ago' : null],
    ['Restarts seen by the master', board.restarts],
    ['Free memory', status.heap != null ? kb(status.heap) : null],
    ['Lowest it has been', status.heapMin != null ? kb(status.heapMin) : null],
    ['Row flips late by', board.lastLateMs != null ? board.lastLateMs + ' ms' : null,
      board.worstLateMs != null ? `Worst since it connected: ${board.worstLateMs} ms.` : null],
  ]);
  group('Firmware', [
    ['Running', board.rev, board.rescue ? 'This is its rescue image.' : null],
    ['Image size', status.imageSize != null ? kb(status.imageSize) : null],
    ['Update offers that failed', board.updateAttempts,
      board.updateBlocked ? 'Blocked: offer the stored image again from Firmware.' : null],
  ]);
  return groups;
}

// One square a start, oldest first: was the power cut, or did software ask?
export function startMarks(board) {
  return (board.starts || []).map((start) => ({
    cls: /power/i.test(start.reset) ? 'pwr' : /software/i.test(start.reset) ? '' : 'odd',
    title: start.reset + (start.stage && start.stage !== 'online' ? `, reached ${start.stage}` : ''),
  }));
}
