// How numbers read on the page. Pure.

export function dur(seconds) {
  const s = Math.max(0, Math.floor(seconds));
  if (s < 60) return s + ' s';
  if (s < 3600) return Math.floor(s / 60) + ' min';
  if (s < 86400) return Math.floor(s / 3600) + ' h ' + Math.floor((s % 3600) / 60) + ' min';
  return Math.floor(s / 86400) + ' d ' + Math.floor((s % 86400) / 3600) + ' h';
}

export function volt(millivolts) {
  return (millivolts / 1000).toFixed(2) + ' V';
}

export function kb(bytes) {
  return Math.round(bytes / 1024) + ' KB';
}

export function plural(n, one, many) {
  return n + ' ' + (n === 1 ? one : many || one + 's');
}

// "1, 2, 3 and 7" for a list of unit addresses.
export function listOf(items) {
  if (items.length < 2) return items.join('');
  return items.slice(0, -1).join(', ') + ' and ' + items[items.length - 1];
}

// A flap's place on the drum as the character it carries; 0 is the blank one.
export function flapName(alphabet, place) {
  const ch = alphabet[place];
  if (ch === undefined) return '?';
  return ch === ' ' ? 'blank' : ch;
}
