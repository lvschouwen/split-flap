// Pure-logic tests for the Wall Console's model layer (#399).
//
// data/console.js keeps every DOM-free function above its own "pure model"
// banner and exports them under a CommonJS guard, so the rules the design
// spec calls load-bearing can be pinned here instead of only being visible
// on a bench. Run by tests/test_console_assets.py so they ride CI; run
// directly with `node --test tests/`.
//
// The two rules under test, in the spec's words:
//   - two object types, not one — a controller that drives no row renders
//     nothing on the board and never appears as an empty row;
//   - state changes density and attention, never what exists — a silent or
//     reflashing row still occupies its place and still shows its text.

import test from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const C = require("../data/console.js");

// --- padRow ---------------------------------------------------------------

test("padRow left-aligns and pads to the full width", () => {
  assert.equal(C.padRow("HI", 5, "left"), "HI   ");
});

test("padRow centres with the extra space on the right", () => {
  assert.equal(C.padRow("HI", 5, "center"), " HI  ");
});

test("padRow right-aligns", () => {
  assert.equal(C.padRow("HI", 5, "right"), "   HI");
});

test("padRow uppercases the wire text", () => {
  assert.equal(C.padRow("hi", 2, "left"), "HI");
});

test("padRow truncates text longer than the row", () => {
  assert.equal(C.padRow("ABCDEF", 3, "left"), "ABC");
});

test("padRow shows only the first page of a multi-line message", () => {
  // Messages carry a literal backslash-n as the page marker; the physical
  // display pages through them and the mirror shows the page that is up.
  assert.equal(C.padRow("ONE\\nTWO", 3, "left"), "ONE");
});

test("padRow of an empty text is a blank row, not an empty string", () => {
  assert.equal(C.padRow("", 4, "left"), "    ");
});

// --- relativeAge ----------------------------------------------------------

test("relativeAge reads seconds as just now", () => {
  assert.equal(C.relativeAge(0), "just now");
  assert.equal(C.relativeAge(44), "just now");
});

test("relativeAge reads minutes", () => {
  assert.equal(C.relativeAge(120), "2 min ago");
});

test("relativeAge reads hours and minutes", () => {
  assert.equal(C.relativeAge(3600 + 43 * 60), "1 h 43 m ago");
});

test("relativeAge reads days", () => {
  assert.equal(C.relativeAge(2 * 86400 + 3600), "2 days ago");
});

// --- sourceLine -----------------------------------------------------------

test("sourceLine names a browser as the owner's own message", () => {
  const s = C.sourceLine("web", 120, "");
  assert.equal(s.who, "Your message");
  assert.equal(s.how, "sent from a browser");
  assert.equal(s.when, "2 min ago");
});

test("sourceLine names MQTT as Home Assistant", () => {
  assert.equal(C.sourceLine("mqtt", 5, "").who, "Home Assistant");
});

test("sourceLine names the clock and says it keeps itself up to date", () => {
  const s = C.sourceLine("clock", 30, "");
  assert.equal(s.who, "Clock");
  assert.equal(s.when, "updates each minute");
});

test("sourceLine names the leader by name when there is one", () => {
  const s = C.sourceLine("leader", 60, "split-flap-c8a746");
  assert.equal(s.who, "The leader");
  assert.match(s.how, /split-flap-c8a746/);
});

test("sourceLine says nothing is driving a blank wall, with no age", () => {
  const s = C.sourceLine("none", 900, "");
  assert.equal(s.who, "Nothing");
  assert.equal(s.when, "");
});

// --- controllerState ------------------------------------------------------

const ctrl = (over) => Object.assign({
  self: false, width: 16, joined: true, degraded: false, suspect: false,
  renderStuck: false, updating: false, rescue: false, faulty: 0,
  role: "display", clustered: true,
}, over);

test("controllerState is quiet and uncoloured when all is well", () => {
  assert.deepEqual(C.controllerState(ctrl({})), { label: "ok", kind: "" });
});

test("controllerState reports a rescue beacon ahead of everything else", () => {
  const s = C.controllerState(ctrl({ rescue: true, updating: true, degraded: true }));
  assert.equal(s.label, "in rescue");
  assert.equal(s.kind, "bad");
});

test("controllerState reports a firmware push as busy, not as a fault", () => {
  assert.deepEqual(C.controllerState(ctrl({ updating: true })),
                   { label: "taking firmware", kind: "busy" });
});

test("controllerState reports a degraded member as not answering", () => {
  assert.deepEqual(C.controllerState(ctrl({ degraded: true })),
                   { label: "not answering", kind: "bad" });
});

test("controllerState reports a member whose segment will not land", () => {
  assert.deepEqual(C.controllerState(ctrl({ renderStuck: true })),
                   { label: "content stuck", kind: "bad" });
});

test("controllerState reports a member that never joined", () => {
  assert.deepEqual(C.controllerState(ctrl({ joined: false })),
                   { label: "not joined", kind: "bad" });
});

test("controllerState counts faulty units in words", () => {
  assert.deepEqual(C.controllerState(ctrl({ faulty: 2 })),
                   { label: "2 units faulty", kind: "bad" });
  assert.equal(C.controllerState(ctrl({ faulty: 1 })).label, "1 unit faulty");
});

test("controllerState keeps the #385 suspect tier quiet — it is not an alarm", () => {
  assert.deepEqual(C.controllerState(ctrl({ suspect: true })),
                   { label: "contact failing", kind: "" });
});

test("controllerState reports a unit reflash as busy, ahead of a contact fault", () => {
  // The units taking firmware is a different thing from the box taking
  // firmware, and it is the one that holds this box's own sends.
  assert.deepEqual(
    C.controllerState(ctrl({ unitReflash: { active: true, done: 7, total: 16 } })),
    { label: "flashing its units", kind: "busy" });
});

test("controllerState says a box with no row drives nothing, without colour", () => {
  assert.deepEqual(C.controllerState(ctrl({ width: 0, role: "headless-spare" })),
                   { label: "spare", kind: "" });
});

test("controllerState never invents a fault for a standalone box", () => {
  assert.deepEqual(
    C.controllerState(ctrl({ clustered: false, joined: false, self: true })),
    { label: "ok", kind: "" });
});

// --- controllersLine ------------------------------------------------------

test("controllersLine counts a healthy wall and says so plainly", () => {
  const line = C.controllersLine([ctrl({}), ctrl({}), ctrl({ width: 0 })]);
  assert.equal(line.text, "3 · all well");
  assert.equal(line.kind, "");
});

test("controllersLine names the row that needs attention", () => {
  const line = C.controllersLine([
    ctrl({ row: 1 }), ctrl({ row: 0, degraded: true }),
  ]);
  assert.equal(line.text, "row 0 is not answering");
  assert.equal(line.kind, "bad");
});

test("controllersLine prefers a fault over a busy state", () => {
  const line = C.controllersLine([
    ctrl({ row: 1, updating: true }), ctrl({ row: 0, degraded: true }),
  ]);
  assert.equal(line.kind, "bad");
  assert.match(line.text, /row 0/);
});

test("controllersLine reports a busy row when nothing is faulty", () => {
  const line = C.controllersLine([ctrl({ row: 0 }), ctrl({ row: 1, updating: true })]);
  assert.equal(line.text, "row 1 is taking firmware");
  assert.equal(line.kind, "busy");
});

test("controllersLine names a box with no row by name, not by a row it lacks", () => {
  const line = C.controllersLine([
    ctrl({ row: 0 }),
    ctrl({ row: 2, width: 0, degraded: true, name: "split-flap-a47dee" }),
  ]);
  assert.equal(line.text, "split-flap-a47dee is not answering");
});

test("controllersLine says a row is flashing its units", () => {
  const line = C.controllersLine([
    ctrl({ row: 0 }),
    ctrl({ row: 1, unitReflash: { active: true, done: 7, total: 16 } }),
  ]);
  assert.equal(line.text, "row 1 is flashing its units");
  assert.equal(line.kind, "busy");
});

// --- capacityLine ---------------------------------------------------------

test("capacityLine totals the wall and breaks it down per row", () => {
  assert.equal(C.capacityLine([{ index: 0, width: 5 }, { index: 1, width: 16 }]),
               "21 characters fit on this wall — 5 on row 0, 16 on row 1.");
});

test("capacityLine does not break down a single row", () => {
  assert.equal(C.capacityLine([{ index: 0, width: 16 }]),
               "16 characters fit on this wall.");
});

test("capacityLine states plainly when there are no flaps to write to", () => {
  assert.equal(C.capacityLine([]), "This box drives no flaps, so there is nothing to write to.");
});

// --- wallModel ------------------------------------------------------------

const settings = (over) => Object.assign({
  unitCount: 16, alignment: "center", effectiveDeviceName: "split-flap-c8a746",
  clusterLeading: false, clusterState: "standalone", clusterRow: 0,
  deviceRole: "display", plat: "esp32s3", version: "aad1d68",
}, over);

test("wallModel builds one controller and one row for a standalone box", () => {
  const w = C.wallModel({ settings: settings({}), status: null, rows: null, text: "HI" });
  assert.equal(w.controllers.length, 1);
  assert.equal(w.controllers[0].self, true);
  assert.equal(w.rows.length, 1);
  assert.equal(w.rows[0].width, 16);
  assert.equal(w.width, 16);
  assert.equal(w.rows[0].segments.length, 1);
  assert.equal(w.rows[0].segments[0].text, "       HI       ");
});

test("wallModel gives a standalone box with no units a controller and no row", () => {
  // Two object types: a box that drives nothing is still a controller, but an
  // empty row would be a lie about the hardware.
  const w = C.wallModel({
    settings: settings({ unitCount: 0, deviceRole: "headless-spare" }),
    status: null, rows: null, text: "",
  });
  assert.equal(w.controllers.length, 1);
  assert.equal(w.rows.length, 0);
  assert.equal(w.width, 0);
});

const leaderStatus = {
  enabled: true,
  members: [
    { host: "", self: true, row: 1, col: 0, width: 16, joined: true,
      degraded: false, rev: "aad1d68", faulty: 0, detected: 16 },
    { host: "192.168.15.121", self: false, row: 0, col: 0, width: 5,
      joined: true, degraded: false, rev: "7750749", plat: "esp01",
      faulty: 0, detected: 5 },
  ],
};

test("wallModel lays a leader's members out as wall rows, widest row first in width", () => {
  const w = C.wallModel({
    settings: settings({ clusterLeading: true, clusterState: "leading" }),
    status: leaderStatus,
    rows: ["TODAY", "  26 JUL 2026   "],
    text: "  26 JUL 2026   ",
  });
  assert.equal(w.controllers.length, 2);
  assert.deepEqual(w.rows.map((r) => r.index), [0, 1]);
  assert.deepEqual(w.rows.map((r) => r.width), [5, 16]);
  // One flap size for the whole wall: every row draws at the widest row's
  // cell size, so five units next to sixteen read as five next to sixteen.
  assert.equal(w.width, 16);
  assert.equal(w.rows[0].segments[0].text, "TODAY");
});

test("wallModel marks the leader's own row as self", () => {
  const w = C.wallModel({
    settings: settings({ clusterLeading: true }), status: leaderStatus,
    rows: ["TODAY", "  26 JUL 2026   "], text: "",
  });
  const own = w.rows[1].segments[0].controller;
  assert.equal(own.self, true);
  assert.equal(own.row, 1);
});

test("wallModel splits one wall row into a segment per controller", () => {
  const status = {
    enabled: true,
    members: [
      { host: "", self: true, row: 0, col: 0, width: 5, joined: true },
      { host: "10.0.0.2", self: false, row: 0, col: 5, width: 4, joined: true },
    ],
  };
  const w = C.wallModel({
    settings: settings({ clusterLeading: true, unitCount: 5 }), status,
    rows: ["HELLOTHER"], text: "HELLO",
  });
  assert.equal(w.rows.length, 1);
  assert.equal(w.rows[0].width, 9);
  assert.deepEqual(w.rows[0].segments.map((s) => s.text), ["HELLO", "THER"]);
});

test("wallModel keeps a width-0 member off the board but on the controller list", () => {
  const status = {
    enabled: true,
    members: leaderStatus.members.concat([{
      host: "192.168.15.20", self: false, row: 2, col: 0, width: 0,
      joined: true, role: "headless-spare", rev: "aad1d68",
    }]),
  };
  const w = C.wallModel({
    settings: settings({ clusterLeading: true }), status,
    rows: ["TODAY", "  26 JUL 2026   "], text: "",
  });
  assert.equal(w.controllers.length, 3);
  assert.deepEqual(w.rows.map((r) => r.index), [0, 1]);
});

test("wallModel lists controllers down the wall, not self first", () => {
  // #428: the leader holds row 1 and the wire lists it first, but the board
  // draws row 0 on top. The list is one tap from the board and has to agree
  // with it.
  const w = C.wallModel({
    settings: settings({ clusterLeading: true }), status: leaderStatus,
    rows: ["TODAY", "  26 JUL 2026   "], text: "",
  });
  assert.deepEqual(w.controllers.map((c) => c.row), [0, 1]);
  assert.deepEqual(w.rows.map((r) => r.index), [0, 1]);
});

test("wallModel puts a controller that drives no row after the ones that do", () => {
  const status = {
    enabled: true,
    members: [
      { host: "192.168.15.20", self: false, row: 2, col: 0, width: 0,
        joined: true, role: "headless-spare", rev: "aad1d68" },
    ].concat(leaderStatus.members),
  };
  const w = C.wallModel({
    settings: settings({ clusterLeading: true }), status,
    rows: ["TODAY", "  26 JUL 2026   "], text: "",
  });
  assert.deepEqual(w.controllers.map((c) => c.width), [5, 16, 0]);
});

test("wallModel orders two controllers sharing one row by column", () => {
  const status = {
    enabled: true,
    members: [
      { host: "192.168.15.30", self: false, row: 0, col: 8, width: 8,
        joined: true, rev: "aad1d68", faulty: 0, detected: 8 },
      { host: "", self: true, row: 0, col: 0, width: 8, joined: true,
        degraded: false, rev: "aad1d68", faulty: 0, detected: 8 },
    ],
  };
  const w = C.wallModel({
    settings: settings({ clusterLeading: true }), status,
    rows: ["                "], text: "",
  });
  assert.deepEqual(w.controllers.map((c) => c.col), [0, 8]);
});

test("wallModel keeps a degraded row on the board, still holding its text", () => {
  // The boundary rule: state may change how loud a row is, never whether it
  // is there.
  const status = {
    enabled: true,
    members: [
      leaderStatus.members[0],
      Object.assign({}, leaderStatus.members[1], { degraded: true }),
    ],
  };
  const w = C.wallModel({
    settings: settings({ clusterLeading: true }), status,
    rows: ["TODAY", "  26 JUL 2026   "], text: "",
  });
  assert.equal(w.rows.length, 2);
  assert.equal(w.rows[0].segments[0].text, "TODAY");
  assert.equal(w.rows[0].silent, true);
});

test("wallModel keeps a reflashing row on the board", () => {
  const status = {
    enabled: true,
    members: [
      Object.assign({}, leaderStatus.members[0], { updating: true }),
      leaderStatus.members[1],
    ],
  };
  const w = C.wallModel({
    settings: settings({ clusterLeading: true }), status,
    rows: ["TODAY", "                "], text: "",
  });
  assert.equal(w.rows.length, 2);
  assert.equal(w.rows[1].busy, true);
  assert.equal(w.rows[1].silent, false);
});

test("wallModel renders the whole wall for a follower reading the leader's digest", () => {
  const w = C.wallModel({
    settings: settings({
      clusterLeading: false, clusterState: "clustered", clusterRow: 0,
      unitCount: 5,
    }),
    status: leaderStatus, rows: ["TODAY", "  26 JUL 2026   "], text: "TODAY",
  });
  assert.equal(w.rows.length, 2);
  // The digest's `self` flag belongs to the leader; a follower finds itself
  // by the row it was given.
  assert.equal(w.rows[0].segments[0].controller.self, true);
  assert.equal(w.rows[1].segments[0].controller.self, false);
});

test("wallModel falls back to this box's own text when no wall rows arrived", () => {
  // A leader whose SSE stream has not delivered rows yet must still draw its
  // own row rather than a blank board.
  const w = C.wallModel({
    settings: settings({ clusterLeading: true, alignment: "left" }),
    status: leaderStatus, rows: null, text: "HI",
  });
  assert.equal(w.rows[1].segments[0].text, "HI              ");
  assert.equal(w.rows[0].segments[0].text, "     ");
});

test("wallModel folds this box's unit reflash onto its own controller", () => {
  const w = C.wallModel({
    settings: settings({}), status: null, rows: null, text: "HI",
    reflash: { state: "flashing", done: 7, total: 16 },
  });
  assert.deepEqual(w.controllers[0].unitReflash, { active: true, done: 7, total: 16 });
  assert.equal(w.rows[0].busy, true);
});

test("wallModel reads an idle reflash block as not busy", () => {
  const w = C.wallModel({
    settings: settings({}), status: null, rows: null, text: "HI",
    reflash: { state: "idle", done: 0, total: 0 },
  });
  assert.equal(w.controllers[0].unitReflash.active, false);
  assert.equal(w.rows[0].busy, false);
});

test("wallModel gives every controller a stable id derived from its host", () => {
  const w = C.wallModel({
    settings: settings({ clusterLeading: true }), status: leaderStatus,
    rows: null, text: "",
  });
  const ids = w.controllers.map((c) => c.id);
  assert.equal(new Set(ids).size, ids.length);
  assert.equal(w.controllers.find((c) => c.self).id, "self");
});

test("wallModel treats a disabled cluster as a standalone box", () => {
  const w = C.wallModel({
    settings: settings({}), status: { enabled: false, members: [] },
    rows: null, text: "HI",
  });
  assert.equal(w.controllers.length, 1);
  assert.equal(w.controllers[0].self, true);
  assert.equal(w.rows.length, 1);
});

// --- controllerBase -------------------------------------------------------

test("controllerBase addresses a remote controller by host and self by nothing", () => {
  // Every fan-out request the console makes is prefixed with this, so a
  // wrong answer here would send a member's settings to the wrong box.
  assert.equal(C.controllerBase({ self: true, host: "" }), "");
  assert.equal(C.controllerBase({ self: false, host: "192.168.15.121" }),
               "http://192.168.15.121");
  assert.equal(C.controllerBase({ self: false, host: "board.local:8801" }),
               "http://board.local:8801");
});
