// The Firmware view, from GET /api/v2/firmware. Pure.
import { plural, kb } from './format.js';

// What should be running and what is: rows of {what, note, shouldBe, is, cls,
// state, todo}. A row that is not `ok` says in `todo` what to do about it, or
// that there is nothing to do.
export function firmwareRows(fw) {
  const rows = [];
  const rowBoards = (fw.boards || []).filter((b) => b.kind === 'row');
  const image = fw.rowImage;
  const held = fw.rowImageHeld;
  const release = fw.release || {};
  rows.push({ what: 'Master firmware', shouldBe: '', is: fw.master.rev, cls: 'ok', state: 'running',
              note: 'What was last installed on the master.' });
  if (rowBoards.length && image) {
    // A row board should run the image the master holds for it.
    const behind = rowBoards.filter((b) => !b.current);
    const blocked = behind.filter((b) => b.updateBlocked).length;
    rows.push({
      what: 'Row board firmware', shouldBe: image.rev,
      is: `${rowBoards.length - behind.length} of ${plural(rowBoards.length, 'row board')}`,
      cls: behind.length ? 'note' : 'ok',
      state: behind.length ? plural(behind.length, 'board') + ' behind' : 'current',
      note: 'Row boards get their copy from the master by themselves.',
      todo: !behind.length ? '' : blocked
        ? `On ${plural(blocked, 'board')} the stored image failed to install three times: press “offer the stored image again” below.`
          + (blocked < behind.length ? ' The others are offered it by the master, one at a time.' : '')
        : 'Nothing to do: the master offers the stored image by itself, one board at a time.',
    });
  } else if (rowBoards.length) {
    // Nothing to hold them against: what they run is all there is to say.
    rows.push({ what: 'Row board firmware', shouldBe: '', cls: 'ok', state: 'running',
                is: [...new Set(rowBoards.map((b) => b.rev || 'not heard yet'))].join(', '),
                note: 'Row boards get their copy from the master by themselves.' });
  }
  if (image) {
    // No rev is the right one: the row boards run this image whatever build
    // the master is on, and a board that cannot talk to the master reads lost.
    const same = image.rev === fw.master.rev;
    const released = release.tag && release.rowImage === image.rev;
    rows.push({
      what: 'Stored image for row boards', shouldBe: '', is: image.rev, cls: 'ok',
      state: same ? 'the master’s build' : released ? `release ${release.tag}` : 'another build, fine',
      note: `Kept on the master, ${kb(image.size)}${image.packed ? ' packed' : ''}. The row boards install this one.`,
    });
  } else if (held) {
    rows.push({
      what: 'Stored image for row boards', shouldBe: '', is: held.rev, cls: 'note', state: 'waiting',
      note: `A release stored it; it is kept back until the master runs ${held.until}.`,
      todo: 'Update to the release under Release, or store a row board image from a file below.',
    });
  } else if (rowBoards.length) {
    rows.push({
      what: 'Stored image for row boards', shouldBe: '', is: 'none', cls: 'note', state: 'missing',
      note: 'Without it the master cannot bring a row board back to working firmware.',
      todo: 'Choose follower-…-gz.bin under “Install an update” below.',
    });
  }
  const units = fw.units || {};
  if (units.total) {
    rows.push({
      what: 'Unit firmware', shouldBe: units.shouldBe, is: `${units.current} of ${plural(units.total, 'unit')}`,
      cls: units.outdated ? 'note' : units.unknown ? 'unknown' : 'ok',
      state: units.outdated ? plural(units.outdated, 'unit') + ' behind' : units.unknown ? 'not all read' : 'current',
      note: units.unknown ? `${plural(units.unknown, 'unit')} could not be read.` : 'The same image on every unit.',
      todo: units.outdated ? 'Press the update button of each board below.'
        : units.unknown ? 'Nothing to do here: the page of their board says why those units do not answer.' : '',
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
      todo: boot.damaged || boot.outdated
        ? 'The page of its board marks the unit; “Update bootloader” is on the unit’s own page, under Service.'
        : boot.unread ? 'Nothing to do: a unit that answers is read within 10 minutes.' : '',
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
    todo: rescue.warn ? 'Choose rescue-….bin under “Install an update” below.' : '',
  });
  return rows;
}

// The release the master found, from the `release` part: {cls, title, why,
// notes, tag, changes, canUpdate}. `changes` is what an update would install.
const LOOK_FAILED = 'The master could not look for a release';
export function releaseText(fw, now) {
  const r = fw.release || {};
  const looked = r.lookedAt ? ` Looked ${ago(now / 1000 - r.lookedAt)}.` : '';
  const daily = r.check === false ? ' The daily look is off.' : '';
  if (r.state === 'failed') {
    return { cls: 'note', title: LOOK_FAILED, why: `${r.why || 'no reason given'}.${looked}${daily}`, changes: [] };
  }
  if (r.state !== 'newer' && r.state !== 'up-to-date') {
    return { cls: 'unknown', title: 'Not looked for a release yet',
             why: (r.check === false ? 'The daily look is off.' : 'The master looks two minutes after it starts, then once a day.'),
             changes: [] };
  }
  if (r.state === 'up-to-date') {
    return { cls: 'ok', title: `Nothing newer than what runs`, tag: r.tag, notes: r.notes,
             why: `The newest release is ${r.tag}.${looked}${daily}`, changes: [] };
  }
  const changes = [`Master firmware: ${fw.master.rev} becomes ${r.master}.`];
  const stored = fw.rowImageHeld ? fw.rowImageHeld.rev : fw.rowImage ? fw.rowImage.rev : '';
  if (r.rowImage !== stored) {
    changes.push(`Image for row boards: ${stored || 'none'} becomes ${r.rowImage}; the row boards install it once the master runs ${r.master}.`);
  }
  if (r.rescue !== (fw.rescue || {}).rev) {
    changes.push(`Rescue image: ${(fw.rescue || {}).rev || 'none'} becomes ${r.rescue}.`);
  }
  const unitsNow = (fw.units || {}).shouldBe;
  if (r.unitRevs && unitsNow && !r.unitRevs.split(',').includes(unitsNow)) {
    changes.push('Unit firmware: the units will read behind afterwards. Updating them stays a press of its own.');
  }
  return { cls: 'note', title: `New release ${r.tag}`, tag: r.tag, notes: r.notes,
           why: `${looked.trim()}${daily}`, changes, canUpdate: true };
}

function ago(seconds) {
  if (seconds < 90) return 'just now';
  if (seconds < 5400) return `${Math.round(seconds / 60)} minutes ago`;
  if (seconds < 129600) return `${Math.round(seconds / 3600)} hours ago`;
  return `${Math.round(seconds / 86400)} days ago`;
}

// What an update from a release is doing, '' when none runs.
const UPDATE_STEPS = {
  looking: 'Looking at the release', rescue: 'Downloading the rescue image',
  'row-image': 'Downloading the image for row boards', master: 'Downloading the master’s firmware',
  restarting: 'Restarting into the release',
};
export function releaseProgress(fw) {
  const update = (fw.release || {}).update;
  if (!update) return '';
  const step = UPDATE_STEPS[update.step] || 'Updating';
  return update.size ? `${step}: ${Math.floor(100 * update.done / update.size)} %` : `${step}…`;
}

export function updateQuestion(release) {
  return `Update to ${release.tag}? ${release.changes.join(' ')} The master restarts, and the wall is blank for about half a minute.`;
}

// The headline says no more than its rows: `why` names each row that is not
// green, with its state.
const VERDICT_TITLES = { bad: 'Something needs installing', note: 'Not everything is up to date',
                         unknown: 'Not everything could be read' };
export function firmwareVerdict(rows) {
  const cause = rows.filter((r) => r.cls !== 'ok');
  const cls = ['bad', 'note', 'unknown'].find((level) => cause.some((r) => r.cls === level));
  if (!cls) return { cls: 'ok', title: 'Everything is up to date', why: '' };
  return { cls, title: VERDICT_TITLES[cls], why: cause.map((r) => `${r.what}: ${r.state}`).join('; ') + '.' };
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
