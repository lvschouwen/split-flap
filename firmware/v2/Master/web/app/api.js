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
