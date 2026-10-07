// The page: one state, one view at a time, kept current by the stream.
import { h, fill, pill } from './dom.js';
import { getJson, followStream } from './api.js';
import { wallView } from './view_wall.js';
import { boardView } from './view_board.js';
import { unitView } from './view_unit.js';
import { firmwareView } from './view_firmware.js';
import { historyView, logView } from './view_history.js';
import { settingsView } from './view_settings.js';
import { calibrateView } from './view_calibrate.js';
import { jobsChanged } from './jobs.js';
import { boardId } from '../model/wall.js';

const app = {
  // wall: GET /api/v2/wall. boards: id -> GET /api/v2/board/<id>, for those read.
  // show: the stream's "wall" topic (mode, quiet, every row's text).
  // jobs: the stream's "jobs" topic, the job table.
  state: { wall: null, boards: {}, show: null, jobs: [], link: true },
  view: null,
};

const VIEWS = { wall: wallView, board: boardView, unit: unitView, firmware: firmwareView,
                history: historyView, log: logView, settings: settingsView,
                calibrate: calibrateView };

function route() {
  const parts = (location.hash || '#wall').slice(1).split('/').map(decodeURIComponent);
  const make = VIEWS[parts[0]] || VIEWS.wall;
  if (app.view && app.view.leave) app.view.leave();
  app.view = make(app, ...parts.slice(1));
  fill(document.getElementById('view'), app.view.root);
  for (const link of document.querySelectorAll('#nav a')) {
    if (link.getAttribute('href') === '#' + parts[0]) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  }
  refresh();
  readBoards();
}

function refresh() {
  const wall = app.state.wall;
  const top = document.getElementById('topPill');
  if (!app.state.link) fill(top, pill('bad', 'No contact with the master'));
  else if (wall) fill(top, pill(wall.verdict === 'fault' ? 'bad' : 'ok', wall.verdict === 'fault' ? 'Needs attention' : 'Working'));
  if (wall) document.getElementById('name').textContent = wall.master.id;
  if (app.view) app.view.refresh();
}

let wallAsked = false;
async function readWall() {
  if (wallAsked) return;  // one read answers every event that came meanwhile
  wallAsked = true;
  try {
    app.state.wall = await getJson('/api/v2/wall');
  } catch (error) {
    // The stream's own state says whether the master is there.
  }
  wallAsked = false;
  refresh();
  readBoards();
}

// A board's document is read when the view shows something of it and what
// the wall says about the board has changed since it was read.
const boardSeen = {};
async function readBoards() {
  if (!app.view || !app.view.boardsWanted) return;
  for (const row of app.view.boardsWanted()) {
    const id = boardId(app.state.wall, row);
    const mark = JSON.stringify([row.verdict, row.unitLevels]);
    if (boardSeen[id] === mark) continue;
    boardSeen[id] = mark;
    try {
      app.state.boards[id] = await getJson('/api/v2/board/' + encodeURIComponent(id));
    } catch (error) {
      delete boardSeen[id];
      continue;
    }
    refresh();
  }
}

// A view asks for this after a job that changed which boards the wall has.
app.readWall = readWall;

// A master without a broker, opened by this browser for the first time,
// starts on its settings: that is where a new wall is put together.
async function firstVisit() {
  try {
    if (location.hash || localStorage.getItem('sf-setup-done')) return;
    localStorage.setItem('sf-setup-done', '1');
    if (!(await getJson('/api/v2/settings/wall')).mqtt.host) location.hash = '#settings';
  } catch (error) {
    // Without storage or an answer the page opens on the wall.
  }
}

window.addEventListener('hashchange', route);
route();
readWall();
firstVisit();
followStream((topic, data) => {
  if (topic === 'wall') app.state.show = data;
  if (topic === 'wall' || topic === 'verdict') readWall();
  if (topic === 'jobs') {
    app.state.jobs = data;
    jobsChanged(data);
  }
  // A view of one board or unit reads its document again when anything about
  // the wall's verdicts or jobs moved.
  if ((topic === 'verdict' || topic === 'jobs') && app.view && app.view.reread) app.view.reread();
  if (topic === 'history' && app.view && app.view.historyMoved) app.view.historyMoved();
}, (up) => {
  app.state.link = up;
  refresh();
  if (up) readWall();
});
