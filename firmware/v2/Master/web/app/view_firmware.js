// Firmware: what should be running and what is, and installing an update.
import { h, fill, pill, statusLine } from './dom.js';
import { getJson } from './api.js';
import { runJob } from './jobs.js';
import { wallLayout, boardTitle } from '../model/wall.js';
import { firmwareRows, firmwareVerdict, firmwareFile, installQuestion, firmwareJobs } from '../model/firmware.js';
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

export function firmwareView(app) {
  const head = h('div', { class: 'head' });
  const table = h('div', {});
  const jobs = h('div', { class: 'rowwrap' });
  const status = statusLine();
  const picker = h('input', { type: 'file', accept: '.bin', hidden: true, onchange: () => chosen() });
  const choose = h('button', { type: 'button', class: 'btn primary', onclick: () => picker.click() },
    'Choose a firmware file…');
  const root = h('div', { class: 'view' }, head,
    h('p', { class: 'muted' }, 'What should be running, and what is.'), table, jobs,
    h('div', { class: 'section' }, h('h2', {}, 'Install an update'),
      h('p', { class: 'muted small' },
        'Choose a file from the build: firmware-…-master.bin, follower-…-gz.bin or rescue-….bin. ',
        'The page knows from its name what it is for and checks it before it is sent.'),
      h('div', { class: 'rowwrap' }, choose, picker), status));
  let fw = null;

  async function chosen() {
    const file = picker.files[0];
    picker.value = '';
    if (!file) return;
    const what = firmwareFile(file.name);
    if (!what) {
      return status.say(`${file.name} is not a file this page knows: it must be named as the build names it.`, true);
    }
    if (!window.confirm(installQuestion(what))) return;
    choose.disabled = true;
    try {
      status.say('Checking the file…');
      const md5 = md5Hex(new Uint8Array(await file.arrayBuffer()));
      const answer = await upload(what.route, file, md5, what.rev,
        (done) => status.say(`Sending ${what.label}: ${Math.round(done * 100)} %`));
      status.say(answer.trim() || 'Installed.');
    } catch (error) {
      status.say(error.message, true);
    }
    choose.disabled = false;
    read();
  }

  function refresh() {
    if (!fw) {
      fill(head, h('h1', {}, 'Firmware'), h('span', { class: 'muted small' }, 'Reading…'));
      return;
    }
    const rows = firmwareRows(fw);
    const verdict = firmwareVerdict(rows);
    fill(head, h('h1', {}, 'Firmware'), pill(verdict.cls, verdict.title));
    fill(table, h('div', { class: 'scroll' }, h('table', {},
      h('thead', {}, h('tr', {}, ['What', 'Should be', 'Is', ''].map((t) => h('th', {}, t)))),
      h('tbody', {}, rows.map((row) => h('tr', {},
        h('td', {}, h('b', {}, row.what), h('div', { class: 'muted small' }, row.note)),
        h('td', {}, row.shouldBe), h('td', {}, row.is), h('td', {}, pill(row.cls, row.state))))))));
    const layout = app.state.wall && wallLayout(app.state.wall);
    fill(jobs, firmwareJobs(fw).map((job) =>
      h('button', { type: 'button', class: 'btn', onclick: () => {
        if (!window.confirm(`${job.label} on ${layout ? boardTitle(layout, job.id) : job.id}?`)) return;
        runJob(status, job.doing, job.name, job.target).then(read);
      } }, `${layout ? boardTitle(layout, job.id) : job.id}: ${job.label.toLowerCase()}`)));
  }

  async function read() {
    try { fw = await getJson('/api/v2/firmware'); } catch (error) { /* the last one stays */ }
    refresh();
  }
  read();
  return { root, refresh, reread: read };
}
