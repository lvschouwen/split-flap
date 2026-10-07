// The Firmware view, from GET /api/v2/firmware. Pure.
import { plural, kb } from './format.js';

// What should be running and what is: rows of {what, note, shouldBe, is, cls, state}.
export function firmwareRows(fw) {
  const rows = [];
  const rowBoards = (fw.boards || []).filter((b) => b.kind === 'row');
  const image = fw.rowImage;
  rows.push({ what: 'Master firmware', shouldBe: '', is: fw.master.rev, cls: 'ok', state: 'running',
              note: 'What was last installed on the master.' });
  if (rowBoards.length) {
    // A row board should run the image the master holds for it.
    const behind = rowBoards.filter((b) => !b.current).length;
    rows.push({
      what: 'Row board firmware', shouldBe: image ? image.rev : '',
      is: `${rowBoards.length - behind} of ${plural(rowBoards.length, 'row board')}`,
      cls: behind ? 'note' : 'ok', state: behind ? plural(behind, 'board') + ' behind' : 'current',
      note: behind ? 'A row board on other firmware is offered the stored image by itself.'
        : 'Row boards get their copy from the master by themselves.',
    });
  }
  if (rowBoards.length || image) {
    // Built from the same commit, the two speak the same link.
    const same = image && image.rev === fw.master.rev;
    rows.push({
      what: 'Stored image for row boards', shouldBe: fw.master.rev, is: image ? image.rev : 'none',
      cls: same ? 'ok' : 'note', state: same ? 'matches' : image ? 'another build' : 'missing',
      note: image ? `Kept on the master, ${kb(image.size)}${image.packed ? ' packed' : ''}.`
        + (same ? '' : ' It is from another build than the master\u2019s firmware.')
        : 'Upload a row board image so the master can hand it out.',
    });
  }
  const units = fw.units || {};
  if (units.total) {
    rows.push({
      what: 'Unit firmware', shouldBe: units.shouldBe, is: `${units.current} of ${plural(units.total, 'unit')}`,
      cls: units.outdated ? 'note' : units.unknown ? 'unknown' : 'ok',
      state: units.outdated ? plural(units.outdated, 'unit') + ' behind' : units.unknown ? 'not all read' : 'current',
      note: units.unknown ? `${plural(units.unknown, 'unit')} could not be read.` : 'The same image on every unit.',
    });
  }
  const boot = fw.bootloaders || {};
  if (boot.total) {
    rows.push({
      what: 'Unit bootloader', shouldBe: boot.shouldBe, is: `${boot.ok} of ${plural(boot.total, 'unit')} intact`,
      cls: boot.damaged ? 'bad' : boot.outdated ? 'note' : boot.unread ? 'unknown' : 'ok',
      state: boot.damaged ? plural(boot.damaged, 'unit') + ' damaged'
        : boot.outdated ? plural(boot.outdated, 'unit') + ' behind' : boot.unread ? 'not all read' : 'current',
      note: 'Every unit checks its own bootloader every 10 minutes.',
    });
  }
  const rescue = fw.rescue || {};
  rows.push({
    // No rev is the right one: an older rescue image that starts is a good one.
    what: 'Rescue image (master)', shouldBe: '', is: rescue.rev || 'none',
    cls: rescue.warn ? 'bad' : 'ok',
    state: rescue.warn ? 'install again' : rescue.state === 'stale' ? 'older, fine' : rescue.state || 'unknown',
    note: rescue.warn ? 'The rescue image is missing or does not match what the master expects.'
      : 'What the master starts when its own firmware does not.',
  });
  return rows;
}

export function firmwareVerdict(rows) {
  if (rows.some((r) => r.cls === 'bad')) return { cls: 'bad', title: 'Something needs installing' };
  if (rows.some((r) => r.cls === 'note')) return { cls: 'note', title: 'Not everything is up to date' };
  if (rows.some((r) => r.cls === 'unknown')) return { cls: 'unknown', title: 'Not everything could be read' };
  return { cls: 'ok', title: 'Everything is up to date' };
}

// What a firmware file is for, from the name the build gives it:
// {kind, rev, route, label} or null.
const FIRMWARE_FILES = [
  [/^firmware-([0-9a-f]{7,}(?:-dirty)?)-master\.bin$/, 'master', '/firmware/master', 'the master’s firmware'],
  [/^follower-([0-9a-f]{7,}(?:-dirty)?)(?:-gz)?\.bin$/, 'row', '/firmware/row', 'the image for row boards'],
  [/^rescue-([0-9a-f]{7,}(?:-dirty)?)\.bin$/, 'rescue', '/firmware/rescue', 'the master’s rescue image'],
];
export function firmwareFile(name) {
  for (const [pattern, kind, route, label] of FIRMWARE_FILES) {
    const match = pattern.exec(name);
    if (match) return { kind, rev: match[1], route, label };
  }
  return null;
}

// What happens when the file is installed, for the question before it is.
export function installQuestion(file) {
  if (file.kind === 'master') return `Install ${file.rev} on the master? It restarts, and the wall is blank for about half a minute.`;
  if (file.kind === 'row') return `Store ${file.rev} for the row boards? Each row board on other firmware installs it and restarts.`;
  return `Install ${file.rev} as the master’s rescue image? The running firmware is not touched.`;
}

// Boards with something an operator can start from here.
export function firmwareJobs(fw) {
  const jobs = [];
  for (const board of fw.boards || []) {
    const row = board.kind === 'master' ? '' : board.id;
    if (board.units && board.units.outdated) {
      jobs.push({ id: board.id, name: 'update-units', target: { row },
                  label: `Update ${plural(board.units.outdated, 'unit')}`, doing: 'Updating units' });
    }
    if (board.updateBlocked) {
      jobs.push({ id: board.id, name: 'update', target: { row: board.id },
                  label: 'Offer the stored image again', doing: 'Offering the image' });
    }
  }
  return jobs;
}
