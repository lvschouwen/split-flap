// Jobs: started with an action, followed on the stream's job table.
import { action } from './api.js';

const watching = new Map();  // op id -> {say(text, bad), ended(job)}

// Starts a job and reports on `status` (statusLine) until it has ended.
// `doing` is what to say meanwhile, e.g. "Finding home on all units".
// Resolves with the job's line of the job table once it has ended ({state:
// "done" | "failed"}), or with null when it was refused.
export function runJob(status, doing, name, target, args) {
  status.say(doing + '…');
  return action(name, target, args).then((answer) => {
    if (answer.op == null) {
      status.say('Done.');
      return { state: 'done' };
    }
    return new Promise((ended) => {
      watching.set(answer.op, { say: (text, bad) => status.say(text, bad), ended });
    });
  }, (error) => {
    status.say(error.message, true);
    return null;
  });
}

// The stream's "jobs" topic: the job table.
export function jobsChanged(table) {
  for (const job of table) {
    const watch = watching.get(job.op);
    if (!watch || job.state === 'running') continue;
    watching.delete(job.op);
    if (job.state === 'done') watch.say(job.detail && job.detail !== 'ok' ? 'Done: ' + job.detail + '.' : 'Done.');
    else watch.say('Failed: ' + (job.detail || 'no reason given') + '.', true);
    watch.ended(job);
  }
}

// An action's target for a board id: the master's own row is "".
export function targetRow(app, id, unit) {
  const target = { row: app.state.wall && id === app.state.wall.master.id ? '' : id };
  if (unit != null) target.unit = unit;
  return target;
}
