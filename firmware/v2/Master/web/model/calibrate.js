// Calibrating the wall: one letter on every flap, the wrong ones marked and
// their offsets corrected. Pure.
import { correctedOffset } from './unit.js';

// The text that puts `ch` on every flap; `lines` as composeLines gives them.
export function testText(lines, ch) {
  return lines.map((line) => ch.repeat(line.width)).join('\n');
}

// What to set for the marked units. A mark is {id, title, unit, shows,
// offset}: the place on the drum the unit is seen to show and the offset it
// has now (null when it could not be read). Returns {sets, problems}: the
// offsets to set, and a sentence for every unit that is left as it is.
export function calibrationPlan(marks, shouldPlace, flaps, stepsPerTurn, limit) {
  const sets = [];
  const problems = [];
  for (const mark of marks) {
    const name = `${mark.title}, unit ${mark.unit}`;
    if (mark.shows === shouldPlace) {
      problems.push(name + ' shows the test letter already.');
    } else if (mark.offset == null) {
      problems.push(name + ': its offset could not be read.');
    } else {
      const next = correctedOffset(mark.offset, shouldPlace, mark.shows, flaps, stepsPerTurn, limit);
      if (next.error) problems.push(`${name}: ${next.error}`);
      else sets.push({ id: mark.id, title: mark.title, unit: mark.unit, offset: next.offset });
    }
  }
  return { sets, problems };
}
