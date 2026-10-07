// The master's /api/v2 as the page uses it.

export async function getJson(path) {
  const reply = await fetch(path, { headers: { Accept: 'application/json' } });
  if (!reply.ok) throw new Error(`${path}: ${reply.status}`);
  return reply.json();
}

// Follows GET /api/v2/stream. `onTopic(name, data)` runs for every event;
// `onLink(up)` when the stream connects or is lost. The browser reconnects
// by itself.
export function followStream(onTopic, onLink) {
  const source = new EventSource('/api/v2/stream');
  for (const topic of ['wall', 'verdict', 'jobs', 'history']) {
    source.addEventListener(topic, (event) => {
      let data;
      try { data = JSON.parse(event.data); } catch (error) { return; }
      onTopic(topic, data);
    });
  }
  source.onopen = () => onLink(true);
  source.onerror = () => onLink(false);
  return source;
}

// Sends a JSON body. Resolves with the master's answer; rejects with the
// master's own words when it refuses.
async function send(method, path, body) {
  let reply;
  try {
    reply = await fetch(path, {
      method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    });
  } catch (error) {
    throw new Error('The master did not answer.');
  }
  let answer = {};
  try { answer = await reply.json(); } catch (error) { /* an answer without a body */ }
  if (!reply.ok) throw new Error(answer.error || `The master refused (${reply.status}).`);
  return answer;
}

// POST /api/v2/action: the answer is {done} or {op}.
export function action(name, target, args) {
  const body = { name };
  if (target) body.target = target;
  if (args) body.args = args;
  return send('POST', '/api/v2/action', body);
}

// PUT /api/v2/settings/...: the answer is {done} and, when the change waits
// for a restart, {restart}.
export function putJson(path, body) {
  return send('PUT', path, body);
}
