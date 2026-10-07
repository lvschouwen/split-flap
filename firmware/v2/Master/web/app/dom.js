// Building the page's elements. Text from a board always becomes a text
// node: nothing here parses a string as HTML.

export function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs || {})) {
    if (value == null || value === false) continue;
    if (key.startsWith('on')) el.addEventListener(key.slice(2), value);
    else if (key === 'class') el.className = value;
    else el.setAttribute(key, value === true ? '' : value);
  }
  add(el, kids);
  return el;
}

function add(el, kids) {
  for (const kid of kids) {
    if (kid == null || kid === false) continue;
    if (Array.isArray(kid)) add(el, kid);
    else el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
}

export function fill(el, ...kids) {
  el.replaceChildren();
  add(el, kids);
  return el;
}

export function pill(cls, text) {
  return h('span', { class: 'pill ' + cls }, text);
}

// A list of {cls, title, why, href, when} as linked lines.
export function itemList(items) {
  return h('div', { class: 'list' }, items.map((item) =>
    h(item.href ? 'a' : 'div', { class: 'item', href: item.href },
      item.when && h('span', { class: 'when' }, item.when),
      h('span', { class: 'dot ' + item.cls }),
      h('span', { class: 'main' },
        h('div', { class: 't' }, item.title),
        item.why && h('div', { class: 'd' }, item.why)),
      item.pill)));
}
