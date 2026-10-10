// Firmware: what should be running and what is, and installing an update.
import { h, fill, pill, statusLine, itemList, segmented } from './dom.js';
import { getJson, putJson } from './api.js';
import { runJob } from './jobs.js';
import { wallLayout, boardTitle } from '../model/wall.js';
import { firmwareRows, firmwareVerdict, firmwareFile, installQuestion,
         releaseText, releaseProgress, updateQuestion } from '../model/firmware.js';
import { md5Hex } from '../model/md5.js';

// Sends a file to an upload route; `onProgress(fraction)` while it goes.
// Resolves with the master's words, rejects with its refusal.
function upload(route, file, md5, rev, onProgress) {
  return new Promise((resolve, reject) => {
    const form = new FormData();
    form.append('firmware', file, file.name);
    const request = new XMLHttpRequest();
    request.open('POST', `${route}?md5=${md5}&v=${encodeURIComponent(rev)}`);
    request.upload.onprogress = (event) => event.lengthComputable && onProgress(event.loaded / event.total);
    request.onload = () => (request.status === 200 ? resolve(request.responseText)
      : reject(new Error(request.responseText || `The master refused (${request.status}).`)));
    request.onerror = () => reject(new Error('The upload broke off. What was running is untouched.'));
    request.send(form);
  });
}

// What steers the look for a release and the units' update. Built once: what
// is being changed is never overwritten by a refresh.
function policyPart(settings, changed) {
  const status = statusLine();
  const save = (body) => putJson('/api/v2/settings/wall', body).then(() => {
    status.say('Saved.');
    changed();
  }, (error) => status.say(error.message, true));
  const pick = (label, options, key) => {
    const seg = segmented(label, options, (value) => { seg.set(value); save({ [key]: value }); });
    seg.set(settings[key]);
    return h('div', { class: 'rowwrap' }, h('span', {}, label), seg);
  };
  return [
    pick('Look for a new release every day', [[true, 'On'], [false, 'Off']], 'releaseCheck'),
    pick('Releases to look for', [['stable', 'Releases'], ['test', 'Trial releases']], 'releaseChannel'),
    h('p', { class: 'muted small' }, 'Looking never installs: an update always starts with a button on this page.'),
    pick('Update units when a board starts', [[true, 'On'], [false, 'Off']], 'updateUnitsAtStart'),
    h('p', { class: 'muted small' }, 'On: every board installs the unit firmware it carries on the units that run another one, each time it starts.'),
    status];
}

export function firmwareView(app) {
  const head = h('div', { class: 'head' });
  const table = h('div', {});
  const releaseLine = h('div', {});
  const lookStatus = statusLine();
  const check = h('button', { type: 'button', class: 'btn', onclick: () => look() }, 'Look for a release');
  const fileStatus = statusLine();
  const picker = h('input', { type: 'file', accept: '.bin', hidden: true, onchange: () => chosen() });
  const choose = h('button', { type: 'button', class: 'btn', onclick: () => pickFile(fileStatus) },
    'Choose a firmware file…');
  const policy = h('div', { class: 'in' });
  const parts = [
    releaseLine, h('div', { class: 'rowwrap' }, check, lookStatus), table,
    h('details', {}, h('summary', {}, 'Settings'), policy),
    h('details', {}, h('summary', {}, 'Install from a file'), h('div', { class: 'in' },
      h('p', { class: 'muted small' },
        'Choose a file from the build: firmware-…-master.bin, follower-…-gz.bin or rescue-….bin. ',
        'The page knows from its name what it is for and checks it before it is sent.'),
      h('div', { class: 'rowwrap' }, choose, picker), fileStatus))];
  const root = h('div', { class: 'view' }, head);
  const said = new Map();  // an action's key -> its status line, kept over a redraw
  let fw = null;
  let busy = false;        // one thing at a time: every button waits for what runs
  let answerTo = fileStatus;

  function statusFor(action) {
    const key = [action.type, action.name, action.id, action.label].join('/');
    if (!said.has(key)) said.set(key, statusLine());
    return said.get(key);
  }

  // Runs one thing with every button held, rereading so the page follows it.
  async function during(work) {
    busy = true;
    refresh();
    const timer = setInterval(read, 2000);
    try { return await work(); } finally {
      clearInterval(timer);
      busy = false;
      read();
    }
  }

  function look() {
    during(() => runJob(lookStatus, 'Looking for a release', 'check-release'));
  }

  function updateNow(status) {
    const release = releaseText(fw, Date.now());
    if (!release.canUpdate || !window.confirm(updateQuestion(release))) return;
    const before = fw.master.rev;
    during(async () => {
      const job = await runJob(status, 'Updating', 'update-from-release');
      if (!job || job.state !== 'done' || !/restarting/.test(job.detail || '')) return;
      // The page is part of the firmware: load the new one once it answers.
      for (let tries = 0; tries < 60; tries++) {
        await new Promise((wait) => setTimeout(wait, 3000));
        try {
          if ((await getJson('/api/v2/firmware')).master.rev !== before) return window.location.reload();
        } catch (error) { /* restarting */ }
      }
      status.say('The master did not come back on the release. What it runs is shown above.', true);
    });
  }

  function pickFile(status) {
    answerTo = status;
    picker.click();
  }

  function chosen() {
    const file = picker.files[0];
    const status = answerTo;
    picker.value = '';
    if (!file) return;
    const what = firmwareFile(file.name);
    if (!what) {
      return status.say(`${file.name} is not a file this page knows: it must be named as the build names it.`, true);
    }
    if (!window.confirm(installQuestion(what))) return;
    during(async () => {
      try {
        status.say('Checking the file…');
        const md5 = md5Hex(new Uint8Array(await file.arrayBuffer()));
        const answer = await upload(what.route, file, md5, what.rev,
          (done) => status.say(`Sending ${what.label}: ${Math.round(done * 100)} %`));
        status.say(answer.trim() || 'Installed.');
      } catch (error) {
        status.say(error.message, true);
      }
    });
  }

  // What can be started from a row: its button, and next to it how it went.
  function actionLine(action, held, layout) {
    const where = action.id ? `${layout ? boardTitle(layout, action.id) : action.id}: ` : '';
    if (action.type === 'link') {
      return h('div', { class: 'rowwrap' },
        h('a', { class: 'btn', href: '#board/' + action.id }, `${where}${action.label} →`));
    }
    const status = statusFor(action);
    const press = {
      release: () => updateNow(status),
      file: () => pickFile(status),
      job: () => {
        if (!window.confirm(`${action.label} on ${where.slice(0, -2)}?`)) return;
        during(() => runJob(status, action.doing, action.name, action.target));
      },
    }[action.type];
    const label = action.type === 'release' ? `Update to ${action.tag}`
      : where ? where + action.label.toLowerCase() : action.label;
    return h('div', { class: 'rowwrap' },
      h('button', { type: 'button', class: action.primary ? 'btn primary' : 'btn', disabled: held, onclick: press }, label),
      status);
  }

  function refresh() {
    if (!fw) {
      fill(root, fill(head, h('h1', {}, 'Firmware'), h('span', { class: 'muted small' }, 'Reading…')));
      return;
    }
    const rows = firmwareRows(fw);
    const verdict = firmwareVerdict(rows);
    fill(head, h('h1', {}, 'Firmware'), pill(verdict.cls, verdict.title),
      verdict.why ? h('span', { class: 'muted small' }, verdict.why) : null);
    const release = releaseText(fw, Date.now());
    const progress = releaseProgress(fw);
    const held = busy || !!progress;
    fill(releaseLine, itemList([{ cls: release.cls, title: release.title, why: release.why,
      pill: release.notes && h('a', { class: 'btn', href: release.notes, target: '_blank', rel: 'noopener' }, 'Release notes') }]));
    check.disabled = choose.disabled = held;
    const layout = app.state.wall && wallLayout(app.state.wall);
    fill(table, h('div', { class: 'scroll' }, h('table', { class: 'cards' },
      h('thead', {}, h('tr', {}, ['What', 'Should be', 'Is', ''].map((t) => h('th', {}, t)))),
      h('tbody', {}, rows.map((row) => h('tr', {},
        h('td', { class: 'lead' }, h('b', {}, row.what), h('div', { class: 'muted small' }, row.note),
          row.todo ? h('div', { class: 'small' }, row.todo) : null,
          row.actions.map((action) => {
            // An update somebody else started is followed here too.
            if (action.type === 'release' && progress) statusFor(action).say(progress);
            return actionLine(action, held, layout);
          })),
        h('td', { 'data-l': 'Should be' }, row.shouldBe), h('td', { 'data-l': 'Is' }, row.is),
        h('td', { class: 'side' }, pill(row.cls, row.state))))))));
    if (!root.contains(table)) fill(root, head, parts);
  }

  async function read() {
    try { fw = await getJson('/api/v2/firmware'); } catch (error) { /* the last one stays */ }
    refresh();
  }
  read();
  getJson('/api/v2/settings/wall').then((settings) => fill(policy, policyPart(settings, read)), () => {});
  return { root, refresh, reread: read };
}
