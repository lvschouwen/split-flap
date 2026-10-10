// Wall settings: which boards make up the wall and where, how text is shown,
// the time zone, Home Assistant, WiFi.
import { h, fill, pill, segmented, statusLine } from './dom.js';
import { getJson, putJson, action } from './api.js';
import { runJob } from './jobs.js';
import { rowsDraft, arrangeProblem, arrangeArgs, zoneFor, brokerText, foundLine } from '../model/settings.js';

const WALL_SETTINGS = '/api/v2/settings/wall';

// Saves a part of the wall's settings and says on `status` how it went.
// Resolves with the master's answer, or null when it refused.
function save(status, body) {
  return putJson(WALL_SETTINGS, body).then((answer) => {
    status.say(answer.restart ? 'Saved. It takes effect when the master restarts.' : 'Saved.');
    return answer;
  }, (error) => {
    status.say(error.message, true);
    return null;
  });
}

// Which board drives which row. Built again when the wall's rows change;
// `status` and `found` (what a search listed) outlive that.
function rowsPart(app, draft, status, found) {
  // A master on its own has nothing to arrange.
  const alone = draft.length < 2;
  const done = (job) => {
    if (job && job.state === 'done') app.readWall();
    return job;
  };
  const place = (line, key, label) => (alone ? line[key] : h('input', {
    type: 'number', min: 1, max: 255, value: line[key], 'aria-label': `${label} of ${line.name}`,
    oninput: (event) => { line[key] = Number(event.target.value); } }));
  const pair = (host) => runJob(status, 'Pairing with the board', 'pair', { host }).then(done)
    .then((job) => { if (job && job.state === 'done') fill(found); });
  const look = async () => {
    fill(found);
    const job = await runJob(status, 'Looking for row boards', 'find-rows');
    if (!job || job.state !== 'done') return;
    let boards;
    try {
      boards = (await getJson('/api/v2/op/' + job.op)).data.boards;
    } catch (error) {
      status.say('The list of boards could not be read.', true);
      return;
    }
    status.say(boards.length ? '' : 'No row board that is not on this wall answered. One that was just switched on needs a minute.');
    fill(found, boards.length > 0 && h('div', { class: 'list' }, boards.map((board) => {
      const line = foundLine(board);
      return h('div', { class: 'item' },
        h('span', { class: 'main' }, h('div', { class: 't' }, line.title), h('div', { class: 'd' }, line.why)),
        h('button', { type: 'button', class: 'btn', onclick: () => pair(board.address) }, 'Add to the wall'));
    })));
  };
  const address = h('input', { type: 'text', id: 'pairAddress', placeholder: '192.168.1.50',
                               maxlength: 15, autocomplete: 'off', inputmode: 'decimal' });
  return h('div', { class: 'section' }, h('h2', {}, 'Rows'),
    h('p', { class: 'muted small' }, 'Which board drives which row, top to bottom, how many units it has, and where the row starts. Boards with the same row number hang side by side.'),
    h('div', { class: 'scroll' }, h('table', {},
      h('thead', {}, h('tr', {}, ['Row', 'Board', 'Units', 'Starts at column', ''].map((t) => h('th', {}, t)))),
      h('tbody', {}, draft.map((line) => h('tr', {},
        h('td', {}, place(line, 'row', 'Row')),
        h('td', {}, h('a', { href: '#board/' + line.name }, line.name),
          h('div', { class: 'muted small' }, line.own ? 'master' : 'row board')),
        h('td', {}, place(line, 'width', 'Number of units')),
        h('td', {}, place(line, 'col', 'Start column')),
        h('td', {}, !line.own && h('button', { type: 'button', class: 'btn quiet', onclick: () => {
          if (!window.confirm(`Remove ${line.name} from the wall? It shows nothing of the wall until it is added again.`)) return;
          runJob(status, 'Removing the board', 'release', { row: line.id }).then(done);
        } }, 'Remove'))))))),
    h('div', { class: 'rowwrap' },
      h('button', { type: 'button', class: 'btn', onclick: look }, 'Look for more boards'),
      !alone && h('button', { type: 'button', class: 'btn primary', onclick: () => {
        const problem = arrangeProblem(draft);
        if (problem) status.say(problem, true);
        else runJob(status, 'Arranging the wall', 'arrange', null, arrangeArgs(draft)).then(done);
      } }, 'Save rows')),
    status, found,
    h('details', {}, h('summary', {}, 'Add a board by its address'),
      h('form', { class: 'in', onsubmit: (event) => {
        event.preventDefault();
        pair(address.value.trim());
      } },
        h('p', { class: 'muted small' }, 'For a row board the search does not find. Its address is on its own page and in the router’s list.'),
        h('div', { class: 'field wide' }, h('label', { for: 'pairAddress' }, 'Address'), address),
        h('div', { class: 'rowwrap' }, h('button', { type: 'submit', class: 'btn' }, 'Add to the wall')))));
}

// How text is shown and the time zone. Built once: what is being changed is
// never overwritten by a refresh.
function displayPart(settings) {
  const status = statusLine();
  const sits = segmented('Text sits', [['left', 'Left'], ['center', 'Centre'], ['right', 'Right']],
    (value) => { sits.set(value); save(status, { alignment: value }); });
  sits.set(settings.alignment);
  const percent = h('span', { class: 'muted small' }, settings.speed + '%');
  const speed = h('input', { type: 'range', min: 1, max: 100, value: settings.speed, 'aria-label': 'Flap speed',
    oninput: () => { percent.textContent = speed.value + '%'; },
    onchange: () => save(status, { speed: Number(speed.value) }) });
  const atStart = segmented('Update units when a board starts', [[true, 'On'], [false, 'Off']],
    (on) => { atStart.set(on); save(status, { updateUnitsAtStart: on }); });
  atStart.set(settings.updateUnitsAtStart);
  const daily = segmented('Look for a new release every day', [[true, 'On'], [false, 'Off']],
    (on) => { daily.set(on); save(status, { releaseCheck: on }); });
  daily.set(settings.releaseCheck);
  const channel = segmented('Releases to look for', [['stable', 'Releases'], ['test', 'Trial releases']],
    (which) => { channel.set(which); save(status, { releaseChannel: which }); });
  channel.set(settings.releaseChannel);
  // The wall keeps a zone's rule, not its name.
  const zone = h('select', { id: 'zone', disabled: true }, h('option', {}, settings.timezone || 'UTC'));
  getJson('/tz.json').then((zones) => {
    const now = zoneFor(zones, settings.timezone, Intl.DateTimeFormat().resolvedOptions().timeZone);
    fill(zone, !now && h('option', { value: '', selected: true }, settings.timezone || 'UTC'),
      Object.keys(zones).map((name) => h('option', { selected: name === now }, name)));
    zone.disabled = false;
    zone.onchange = () => { if (zone.value) save(status, { timezone: zones[zone.value] }); };
  }, () => {});
  return h('div', { class: 'section' }, h('h2', {}, 'Display'),
    h('div', { class: 'rowwrap' }, h('span', {}, 'Text sits'), sits,
      h('span', {}, 'Flap speed'), speed, percent),
    h('div', { class: 'field wide' }, h('label', { for: 'zone' }, 'Time zone'), zone),
    h('div', { class: 'rowwrap' }, h('span', {}, 'Update units when a board starts'), atStart),
    h('p', { class: 'muted small' }, 'On: every board installs the unit firmware it carries on the units that run another one, each time it starts.'),
    h('div', { class: 'rowwrap' }, h('span', {}, 'Look for a new release every day'), daily, channel),
    h('p', { class: 'muted small' }, 'Looking never installs: an update always starts with the button on Firmware.'),
    status);
}

// The broker Home Assistant listens on. `line` is where the state is shown.
function brokerPart(settings, line, saved) {
  const status = statusLine();
  const mqtt = settings.mqtt;
  const field = (id, label, attrs) => {
    const input = h('input', { id, autocomplete: 'off', ...attrs });
    return { input, row: h('div', { class: 'field wide' }, h('label', { for: id }, label), input) };
  };
  const host = field('mqttHost', 'Host', { type: 'text', value: mqtt.host, maxlength: 64, placeholder: 'empty: not used' });
  const port = field('mqttPort', 'Port', { type: 'number', value: mqtt.port, min: 1, max: 65535 });
  const user = field('mqttUser', 'User', { type: 'text', value: mqtt.user, maxlength: 32 });
  const password = field('mqttPassword', 'Password', { type: 'password', autocomplete: 'new-password',
    maxlength: 64, placeholder: mqtt.passwordSet ? 'empty: keep the stored one' : '' });
  const restart = h('button', { type: 'button', class: 'btn', hidden: true, onclick: () =>
    action('restart').then(() => status.say('The master restarts.'), (error) => status.say(error.message, true)),
  }, 'Restart the master now');
  const submit = async (event) => {
    event.preventDefault();
    const next = { host: host.input.value.trim(), port: Number(port.input.value), user: user.input.value.trim() };
    const answer = await save(status, { mqtt: { ...next, password: password.input.value } });
    if (!answer) return;
    password.input.value = '';
    restart.hidden = !answer.restart;
    saved(next);
  };
  return h('div', { class: 'section' }, h('h2', {}, 'Home Assistant'), line,
    h('details', {}, h('summary', {}, 'Change broker'),
      h('form', { class: 'in', onsubmit: submit },
        h('p', { class: 'muted small' }, 'The MQTT broker Home Assistant listens on. The wall announces itself there.'),
        host.row, port.row, user.row, password.row,
        h('div', { class: 'rowwrap' }, h('button', { type: 'submit', class: 'btn' }, 'Save'), restart),
        status)));
}

export function settingsView(app) {
  const rows = h('div', {});
  const rowsStatus = statusLine();
  const found = h('div', {});
  const parts = h('div', { class: 'view-part' }, h('p', { class: 'muted small' }, 'Reading the settings…'));
  const broker = h('p', {});
  const root = h('div', { class: 'view' }, h('div', { class: 'head' }, h('h1', {}, 'Wall settings')), rows, parts);
  let rowsShape = '';
  let settings = null;
  let connected;
  let asked = false;

  function refresh() {
    const wall = app.state.wall;
    if (wall) {
      const draft = rowsDraft(wall);
      if (JSON.stringify(draft) !== rowsShape) {
        rowsShape = JSON.stringify(draft);
        fill(rows, rowsPart(app, draft, rowsStatus, found));
      }
      if (!asked) {
        asked = true;
        getJson('/api/v2/board/' + encodeURIComponent(wall.master.id)).then((board) => {
          connected = !!board.mqttConnected;
          refresh();
        }, () => {});
      }
    }
    if (settings) {
      const text = brokerText(settings.mqtt, connected);
      fill(broker, pill(text.cls, text.title), ' ', h('span', { class: 'muted small' }, text.why));
    }
  }

  getJson(WALL_SETTINGS).then((read) => {
    settings = read;
    fill(parts, displayPart(settings),
      brokerPart(settings, broker, (mqtt) => {
        // What is connected now is the broker of before the change.
        settings.mqtt = { ...settings.mqtt, ...mqtt };
        connected = undefined;
        asked = false;
        refresh();
      }),
      h('div', { class: 'section' }, h('h2', {}, 'WiFi'),
        h('p', { class: 'muted small' }, 'Each board remembers its own WiFi. The master’s is changed under “Settings for this board” on ',
          h('a', { href: '#board/' + (app.state.wall ? app.state.wall.master.id : '') }, 'its page'),
          '. A row board that cannot join its WiFi opens its own setup network.')));
    refresh();
  }, () => fill(parts, h('p', { class: 'status bad' }, 'The settings could not be read.')));

  return { root, refresh };
}
