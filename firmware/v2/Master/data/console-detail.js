/* Wall Console (#399) — levels three and four: one controller, and one unit.
 *
 * Loaded after console.js and sharing its globals. The mechanisms live there
 * (say, confirmThen, runOp, uploadControl, factList, field); this file is
 * only the arrangement, and the arrangement is the design's whole argument:
 *
 *   The controller page is ordered physically. The box, then how the box is
 *   set up, then the row it drives, then how the row behaves, then the units,
 *   then its place in the wall, then — last, plainly labelled, not locked —
 *   what to do when something has gone wrong.
 *
 * Settings sit on the object they change. Nothing on this page collects them
 * into a settings section, because that is the thing the redesign removed.
 */

var RESET_WORDS = {
	Brownout: "the power dipped",
	"Software reset": "it was asked to restart",
	"Power on": "it was plugged in",
	Watchdog: "a watchdog fired",
	Panic: "it crashed",
	Deepsleep: "it woke from sleep"
};

var UNIT_RESET_WORDS = {
	1: "power dipped",
	2: "reset pin",
	4: "watchdog",
	8: "asked to restart"
};

function rssiWord(dbm) {
	if (!dbm) return "";
	if (dbm >= -60) return "strong";
	if (dbm >= -70) return "fine";
	if (dbm >= -80) return "weak";
	return "very weak";
}

function bytesWord(n) {
	if (n === undefined || n === null || n === "") return "—";
	var v = Number(n) || 0;
	return v >= 1024 ? Math.round(v / 1024) + " KB" : v + " B";
}

function upWord(seconds) {
	var s = Math.max(0, Math.floor(Number(seconds) || 0));
	var d = Math.floor(s / 86400);
	var h = Math.floor((s % 86400) / 3600);
	var m = Math.floor((s % 3600) / 60);
	if (d) return d + "d " + h + "h";
	if (h) return h + "h " + m + "m";
	return m + "m";
}

function unitList(units) {
	return units && Array.isArray(units.units) ? units.units : [];
}

function hex(n) {
	return "0x" + Number(n).toString(16).padStart(2, "0");
}

/* ==========================================================================
 * one controller
 * ========================================================================== */

function drawController(c) {
	var got = view.remote[c.id] || {};
	var s = got.settings;
	byId("ctrl-name").textContent = c.name || c.host || "This box";
	byId("ctrl-sub").textContent = [
		c.host || location.host,
		platLabel(c.plat),
		c.self && view.settings && view.settings.clusterLeading ? "leads the wall"
			: c.width > 0 ? "drives row " + c.row : "drives no row"
	].filter(Boolean).join(" · ");

	var body = clear(byId("ctrl-body"));

	if (!s) {
		body.appendChild(panelOf(el("p", "none",
			"This box is not answering right now, so there is nothing to show "
			+ "about it. The wall still reports what it last knew.")));
		body.appendChild(rareSection(c, null));
		return;
	}

	body.appendChild(boxSection(c, s, got.stats));
	body.appendChild(boxConfigSection(c, s));
	if (c.plat !== "esp01") body.appendChild(brokerSection(c, s));

	if (c.width > 0) {
		body.appendChild(rowSection(c, got.units));
		body.appendChild(rowBehaviourSection(c, s));
		body.appendChild(unitsSection(c, got.units));
	} else {
		body.appendChild(section("The row it drives", panelOf(el("p", "none",
			"This box drives no flaps. It sits on the wall ready to take over "
			+ "if the leader goes quiet — there is nothing here to calibrate."))));
	}

	body.appendChild(wallSection(c, s));
	body.appendChild(rareSection(c, s));
}

/* 1 — the box itself, and the two things you do TO a box */

function boxSection(c, s, stats) {
	var base = controllerBase(c);
	var wrap = el("div", "panel");
	wrap.appendChild(factList([
		["Firmware", s.version || "unknown"],
		["Running for", upWord(s.up), s.lastResetReason
			? "last restart: " + (RESET_WORDS[s.lastResetReason] || s.lastResetReason)
			: null],
		s.rssi ? ["Radio", s.rssi + " dBm", rssiWord(s.rssi)] : null,
		["Free memory", bytesWord(s.heap), s.plat === "esp01"
			? "small board, this is normal" : null],
		s.rescueSlot && s.plat !== "esp01"
			? ["Rescue image", s.rescueRev || "not identified",
				rescueWord(s), s.rescueSlotWarn ? "bad" : ""]
			: null,
		s.otaReverted
			? ["Last update", "rolled back", "the box went back to the image it trusted", "bad"]
			: null
	]));
	if (stats) wrap.appendChild(vitals(stats));

	var status = statusLine();
	wrap.appendChild(section("Update firmware",
		uploadControl({
			id: "box-fw", base: base, path: "/firmware/master",
			label: "Flash this box", expect: "firmware .bin",
			confirm: function (f) {
				return "Flash " + f.name + " (" + Math.round(f.size / 1024)
					+ " KB) to this box?\n\nIt reboots into the new firmware. "
					+ "If the image is bad it goes back to this one by itself.";
			}
		})));
	var acts = el("div", "rowacts");
	acts.appendChild(button("btn quiet small", "Restart this box", function () {
		confirmThen("Restart this box? The wall goes blank for about 15 seconds.",
			function () {
				say(status, "Restarting…", "pending");
				postText(base + "/reboot")
					.then(function (t) { say(status, t || "Restarting.", "ok", 20000); })
					.catch(function (e) { say(status, "Not restarted — " + e.message, "bad", 8000); });
			});
	}));
	wrap.appendChild(acts);
	wrap.appendChild(status);
	return section("The box", wrap);
}

function rescueWord(s) {
	if (s.rescueSlot === "ok") {
		return s.rescueRev === s.version ? "matches what is running"
			: "an older image, kept as a way back";
	}
	if (s.rescueSlot === "stale") return "older than what is running";
	if (s.rescueSlot === "empty") return "the slot is empty — no way back";
	if (s.rescueSlot === "absent") return "this box has no rescue slot";
	return "the slot holds something we cannot identify";
}

function vitals(stats) {
	var now = stats.now || {};
	var hist = stats.hist || {};
	var grid = el("div", "vitals");
	[
		["Signal", now.rssi + " dBm", hist.rssi],
		["Free heap", bytesWord(now.heap), hist.heap],
		["Core 0", now.cpu0 + "%", hist.cpu0],
		["Core 1", now.cpu1 + "%", hist.cpu1],
		["Die temp", (now.temp / 10).toFixed(1) + " °C", hist.temp],
		["Free PSRAM", bytesWord(now.psram), null]
	].forEach(function (t) {
		var tile = el("div", "tile");
		tile.appendChild(el("span", "tile-k", t[0]));
		tile.appendChild(el("span", "tile-v", t[1]));
		if (t[2]) tile.appendChild(sparkline(t[2]));
		grid.appendChild(tile);
	});
	var wrap = el("div");
	wrap.appendChild(grid);
	wrap.appendChild(factList([
		["Unit bus", (now.i2cTx || 0) + " transactions",
			(now.i2cErr || 0) + " of them failed", now.i2cErr ? "bad" : ""],
		now.mqttDrops !== undefined
			? ["Broker drops", now.mqttDrops, "since this box last started"] : null,
		["Clock", now.ntpAge >= 0 ? "synced " + relativeAge(now.ntpAge) : "never synced",
			null, now.ntpAge < 0 ? "bad" : ""],
		["Least free heap", bytesWord(now.minHeap), "the lowest it has ever been"]
	]));
	return wrap;
}

/* 2 — how this box is set up. Name, time zone and role change the BOX, so
 * this is where they live. */

function boxConfigSection(c, s) {
	var base = controllerBase(c);
	var wrap = el("div", "panel");

	if (s.plat === "esp01") {
		wrap.appendChild(el("p", "none",
			"This row's name is baked into its firmware, and it follows the "
			+ "leader's time zone. There is nothing to set here."));
		return section("How this box is set up", wrap);
	}

	var status = statusLine();
	var nameInput = input("box-name", "text", s.deviceName || "");
	nameInput.maxLength = 24;
	nameInput.placeholder = s.effectiveDeviceName || "";
	wrap.appendChild(field("Name", nameInput,
		"The web address, hostname and Home Assistant identity. Applies after a restart."));

	var tz = el("select");
	tz.id = "box-tz";
	tz.disabled = true;
	tz.appendChild(el("option", null, "loading…"));
	wrap.appendChild(field("Time zone", tz,
		"Used by the clock, and handed to every row this box leads."));
	loadTimezones().then(function (table) {
		if (!table || !tz.isConnected) return;
		clear(tz);
		Object.keys(table).forEach(function (name) {
			var o = el("option", null, name);
			o.value = table[name];
			if (table[name] === (s.timezonePosix || "")) o.selected = true;
			tz.appendChild(o);
		});
		tz.disabled = false;
	});

	wrap.appendChild(roleField(c, s, status, base));

	var acts = el("div", "rowacts");
	acts.appendChild(button("btn primary small", "Save", function () {
		say(status, "Saving…", "pending");
		var fields = { deviceName: nameInput.value };
		if (!tz.disabled) fields.timezone = tz.value;
		postFields(base, fields).then(function (result) {
			say(status, result === "ok-reboot"
				? "Saved. The name applies after a restart." : "Saved.", "ok", 6000);
		}).catch(function (e) {
			say(status, "Not saved — " + e.message, "bad", 8000);
		});
	}));
	wrap.appendChild(acts);
	wrap.appendChild(status);
	return section("How this box is set up", wrap);
}

var ROLES = [
	["display", "Drives a row", "shows what it is sent"],
	["headless-backup", "Standing by", "no flaps; takes over the wall first if the leader goes quiet"],
	["headless-spare", "Spare", "no flaps; available, but others take over before it"],
	["headless-monitor", "Monitor", "no flaps; a dashboard, and the last resort for a takeover"]
];

/* The role is saved on its own, because choosing it blanks the box's row —
 * that is not something to fold silently into a Save button next to a name. */
function roleField(c, s, status, base) {
	var wrap = el("div", "field");
	wrap.appendChild(el("span", "fieldlabel", "What this box is for"));
	var choice = el("div", "choice choice-col");
	choice.setAttribute("role", "group");
	choice.setAttribute("aria-label", "What this box is for");
	ROLES.forEach(function (r) {
		var b = button("choicebtn", null, function () {
			if (r[0] === (s.deviceRole || "display")) return;
			var blanks = r[0] !== "display" && c.width > 0;
			confirmThen(blanks
				? "Make this box a " + r[1].toLowerCase() + "?\n\nIts row goes blank — "
					+ "a box with no flaps of its own cannot show anything."
				: null,
				function () {
					choice.querySelectorAll(".choicebtn").forEach(function (x) {
						x.setAttribute("aria-pressed", String(x === b));
					});
					say(status, "Saving…", "pending");
					postFields(base, { deviceRole: r[0] })
						.then(function () { say(status, "Saved.", "ok", 5000); })
						.catch(function (e) { say(status, "Not saved — " + e.message, "bad", 8000); });
				});
		});
		b.appendChild(el("span", "choice-k", r[1]));
		b.appendChild(el("small", null, r[2]));
		b.setAttribute("aria-pressed", String(r[0] === (s.deviceRole || "display")));
		choice.appendChild(b);
	});
	wrap.appendChild(choice);
	if (s.headlessSuggested) {
		wrap.appendChild(el("p", "hint",
			"This box finds no units on its bus. If that is deliberate, pick one of "
			+ "the roles above — it will never switch by itself."));
	}
	return wrap;
}

/* 3 — Home Assistant. The broker is a property of the box, not of the wall. */

function brokerSection(c, s) {
	var base = controllerBase(c);
	var wrap = el("div", "panel");
	var status = statusLine();

	var host = input("mqtt-host", "text", s.mqttHost || "");
	host.maxLength = 64;
	host.placeholder = "e.g. 192.168.1.10";
	wrap.appendChild(field("Broker address", host,
		"Leave empty to switch Home Assistant off."));
	var port = input("mqtt-port", "number", s.mqttPort || "");
	port.placeholder = "1883";
	wrap.appendChild(field("Port", port));
	var user = input("mqtt-user", "text", s.mqttUser || "");
	user.maxLength = 32;
	wrap.appendChild(field("Username", user, "Optional."));
	var pass = input("mqtt-pass", "password", "");
	pass.maxLength = 64;
	pass.autocomplete = "new-password";
	wrap.appendChild(field("Password", pass, s.mqttPasswordSet
		? "A password is stored. Leave empty to keep it." : "Optional."));

	var suggestions = el("div", "suggestions");
	suggestions.hidden = true;
	wrap.appendChild(suggestions);

	var acts = el("div", "rowacts");
	var find = button("btn quiet small", "Find the broker", function () {
		find.disabled = true;
		say(status, "Looking around the network…", "pending");
		postText(base + "/mqtt/discover").catch(function () { })
			.then(function () { return pollDiscovery(base, "/mqtt/discover", 20); })
			.then(function (found) {
				clear(suggestions);
				if (!found.length) {
					say(status, "Nothing answered. Type the address in yourself.", "bad", 8000);
					suggestions.hidden = true;
					return;
				}
				say(status, "Pick one, then Save.", "ok", 8000);
				found.forEach(function (b) {
					suggestions.appendChild(button("chip", b.host + ":" + b.port, function () {
						host.value = b.host;
						port.value = b.port;
					}));
				});
				suggestions.hidden = false;
			})
			.catch(function (e) { say(status, "Search failed — " + e.message, "bad", 8000); })
			.then(function () { find.disabled = false; });
	});
	acts.appendChild(find);
	acts.appendChild(button("btn primary small", "Save", function () {
		say(status, "Saving…", "pending");
		postFields(base, {
			mqttHost: host.value, mqttPort: port.value,
			mqttUser: user.value, mqttPassword: pass.value
		}).then(function (result) {
			say(status, result === "ok-reboot"
				? "Saved. Home Assistant connects after a restart." : "Saved.", "ok", 6000);
		}).catch(function (e) {
			say(status, "Not saved — " + e.message, "bad", 8000);
		});
	}));
	wrap.appendChild(acts);
	wrap.appendChild(status);

	return section("Home Assistant" + (s.mqttConnected ? " — connected" : ""), wrap,
		"Sensors and controls for this box, over MQTT.");
}

function pollDiscovery(base, path, remaining) {
	return getJson(base + path).catch(function () { return null; }).then(function (d) {
		if (d && d.state === "done") return d.brokers || d.results || [];
		if (remaining <= 0) return [];
		return new Promise(function (resolve) {
			setTimeout(function () {
				resolve(pollDiscovery(base, path, remaining - 1));
			}, 500);
		});
	});
}

/* 4 — the row it drives */

function rowSection(c, units) {
	var base = controllerBase(c);
	var list = unitList(units);
	var answering = units ? list.filter(function (u) { return u.v === 1; }).length : null;
	var revs = list.map(function (u) { return u.rev; }).filter(Boolean);
	var same = revs.length > 0 && revs.every(function (r) { return r === revs[0]; });
	var faulty = units ? units.faulty : c.faulty;
	var wrap = el("div", "panel");
	wrap.appendChild(factList([
		["Answering", (answering === null ? "—" : answering) + " of " + c.width,
			null, answering !== null && answering < c.width ? "bad" : ""],
		["Faults", faulty ? faulty + (faulty === 1 ? " unit" : " units") : "none",
			null, faulty ? "bad" : ""],
		units && units.vccMin
			? ["Supply floor", (units.vccMin / 1000).toFixed(2) + " V",
				"lowest any unit has seen since it started"]
			: null,
		revs.length
			? ["Unit firmware", revs[0],
				same ? "same on all " + c.width : "NOT the same on every unit",
				same ? "" : "bad"]
			: null
	]));
	var status = statusLine();
	var acts = el("div", "rowacts");
	acts.appendChild(button("btn quiet small", "Read them again", function () {
		say(status, "Re-reading every unit…", "pending");
		postText(base + "/units/health/refresh")
			.then(function () {
				say(status, "Reading — the numbers land in a moment.", "ok", 6000);
			})
			.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
	}));
	wrap.appendChild(acts);
	wrap.appendChild(status);
	return section("Row " + c.row + " — the " + c.width
		+ (c.width === 1 ? " unit" : " units") + " it drives", wrap);
}

/* 5 — how this row behaves. Mode, speed, line-up and width belong to the
 * row, so they sit on the row. */

var ALIGNMENTS = [["left", "Left"], ["center", "Centred"], ["right", "Right"]];
var MODES = [["text", "Hold a message"], ["clock", "Show the time"]];

function rowBehaviourSection(c, s) {
	var base = controllerBase(c);
	var wrap = el("div", "panel");
	var status = statusLine();

	if (s.plat === "esp01") {
		wrap.appendChild(el("p", "none",
			"This row renders what the leader hands it, already laid out. "
			+ "Speed and line-up are set on the box that leads the wall."));
		return section("How this row behaves", wrap);
	}

	wrap.appendChild(choiceField("When nobody is sending anything", MODES,
		s.deviceMode || "text", function (value, done) {
			postFields(base, { deviceMode: value }).then(done).catch(done);
		}, status));

	var speedOut = el("span", "fieldval", String(s.flapSpeed || "80"));
	var speed = input("row-speed", "range", String(s.flapSpeed || "80"));
	speed.min = "1";
	speed.max = "100";
	speed.step = "1";
	speed.addEventListener("input", function () { speedOut.textContent = speed.value; });
	/* "change" fires once per deliberate adjustment, never while dragging, so
	   a drag cannot churn the stored setting. Speed is the one reversible
	   local preference the design lets apply on change. */
	speed.addEventListener("change", function () {
		postFields(base, { flapSpeed: speed.value })
			.then(function () { say(status, "Speed saved.", "ok", 4000); })
			.catch(function (e) { say(status, "Not saved — " + e.message, "bad", 8000); });
	});
	var speedField = field("Flap speed", speed);
	speedField.querySelector("label").appendChild(speedOut);
	wrap.appendChild(speedField);

	wrap.appendChild(choiceField("Line up", ALIGNMENTS, s.alignment || "left",
		function (value, done) {
			postFields(base, { alignment: value }).then(done).catch(done);
		}, status));

	/* Pinning the width is a bench tool: it makes the box claim units it may
	   not have. It sits last here, and says what it is. */
	var width = input("row-width", "number", String(s.unitCountOverride || 0));
	width.min = "0";
	width.max = String(SFP.maxUnits);
	var widthWrap = field("How many units to claim", width,
		"0 lets the box count them on the bus, which is what you want unless you are "
		+ "working on a bench with some of them missing.");
	var widthActs = el("div", "rowacts");
	widthActs.appendChild(button("btn quiet small", "Set", function () {
		var value = parseInt(width.value, 10);
		if (isNaN(value) || value < 0 || value > SFP.maxUnits) {
			return say(status, "Enter 0 to count them, or 1 to " + SFP.maxUnits + ".", "bad", 5000);
		}
		postFields(base, { unitCount: value })
			.then(function () {
				say(status, value ? "Claiming " + value + " units."
					: "Counting them on the bus again.", "ok", 5000);
			})
			.catch(function (e) { say(status, "Not saved — " + e.message, "bad", 8000); });
	}));
	widthWrap.appendChild(widthActs);
	wrap.appendChild(widthWrap);
	wrap.appendChild(status);

	return section("How this row behaves", wrap,
		"Speed, line-up and what it does when idle belong to the row, not to a settings page.");
}

function choiceField(label, options, current, apply, status) {
	var wrap = el("div", "field");
	wrap.appendChild(el("span", "fieldlabel", label));
	var choice = el("div", "choice");
	choice.setAttribute("role", "group");
	choice.setAttribute("aria-label", label);
	options.forEach(function (o) {
		var b = button("choicebtn", o[1], function () {
			choice.querySelectorAll(".choicebtn").forEach(function (x) {
				x.setAttribute("aria-pressed", String(x === b));
			});
			say(status, "Saving…", "pending");
			apply(o[0], function (e) {
				if (e instanceof Error) say(status, "Not saved — " + e.message, "bad", 8000);
				else say(status, "Saved.", "ok", 4000);
			});
		});
		b.setAttribute("aria-pressed", String(o[0] === current));
		choice.appendChild(b);
	});
	wrap.appendChild(choice);
	return wrap;
}

/* 6 — the units, and the things you do to all of them at once */

function unitsSection(c, units) {
	var base = controllerBase(c);
	var wrap = el("div");
	var grid = el("div", "units");
	var list = unitList(units);
	var worn = units && units.wear && Array.isArray(units.wear.flagged)
		? units.wear.flagged : [];
	for (var i = 0; i < c.width; i++) {
		var u = list[i];
		var cell = button("unit", String(i + 1), unitOpener(c.id, i));
		if (u) {
			if (u.v !== 1 || u.stale) cell.classList.add("bad");
			else if ((u.fl & SFP.flag.homeFailed) || u.mm) cell.classList.add("bad");
			else if (u.st === 2) cell.classList.add("busy");
			else if (worn.indexOf(i) >= 0) cell.classList.add("wear");
			cell.setAttribute("aria-label", "Unit " + (i + 1) + ", address " + hex(u.a));
		} else {
			cell.classList.add("bad");
			cell.setAttribute("aria-label", "Unit " + (i + 1) + ", not answering");
		}
		grid.appendChild(cell);
	}
	wrap.appendChild(grid);

	var legend = el("p", "legend");
	var faulty = units ? units.faulty : 0;
	if (!units) {
		legend.textContent = "This row's health could not be read.";
	} else if (faulty) {
		legend.textContent = "Red means a unit is not answering, has not found its "
			+ "home, or is showing something other than what it was told to. "
			+ faulty + (faulty === 1 ? " unit is" : " units are") + " flagged.";
	} else if (worn.length) {
		legend.textContent = "Amber means a unit has turned far more than the rest — "
			+ "worth knowing, not a fault. Everything is answering.";
	} else {
		legend.textContent = "All answering, none flagged. Tap one to read it or "
			+ "line it up.";
	}
	wrap.appendChild(legend);

	if (units && units.reflash && reflashProgress(units.reflash, wrap)) {
		/* progress bar appended by reflashProgress */
	}

	var status = statusLine();
	var acts = el("div", "rowacts");
	acts.appendChild(button("btn quiet small", "Home them all", function () {
		confirmThen("Home every unit on this row? The wall blanks, then the message "
			+ "comes back.", function () {
			say(status, "Homing…", "pending");
			postText(base + "/reset-units")
				.then(function () { say(status, "Homing — the message returns by itself.", "ok", 8000); })
				.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
		});
	}));
	acts.appendChild(button("btn quiet small", "Update unit firmware", function () {
		confirmThen("Flash the bundled unit firmware onto every unit on this row?"
			+ "\n\nThe row leaves the bus while it works, and nothing can be sent to "
			+ "it until it finishes.", function () {
			say(status, "Starting…", "pending");
			postText(base + "/reflash-units")
				.then(function () { say(status, "Flashing — progress shows above.", "ok", 8000); })
				.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
		});
	}));
	wrap.appendChild(acts);
	wrap.appendChild(status);

	wrap.appendChild(lineUpPanel(c, units));

	return section("The units", wrap);
}

function unitOpener(ctrlId, index) {
	return function () {
		navigate("#u/" + encodeURIComponent(ctrlId) + "/" + index);
	};
}

function reflashProgress(block, wrap) {
	var f = reflashFacts(block);
	if (!f.active) return false;
	var p = el("div", "progress");
	p.appendChild(el("span", null, "Flashing unit " + (f.done + 1) + " of " + f.total));
	var bar = el("div", "bar");
	var fill = el("div", "fill");
	fill.style.width = (f.total ? Math.round((f.done / f.total) * 100) : 0) + "%";
	bar.appendChild(fill);
	p.appendChild(bar);
	wrap.appendChild(p);
	return true;
}

/* Lining the drums up: show the same letter on every unit, then say what
 * each one is ACTUALLY showing. The correction is arithmetic from there. */
function lineUpPanel(c, units) {
	var base = controllerBase(c);
	var wrap = el("details", "lineup");
	wrap.appendChild(el("summary", null, "Line up the drums"));
	wrap.appendChild(el("p", "hint",
		"Send one letter to the whole row, look at the wall, and type what each drum "
		+ "is really showing. Anything you leave empty is left alone."));

	var status = statusLine();
	var letter = el("select");
	letter.id = "lineup-letter";
	CALIBRATION_LETTERS.forEach(function (ch) {
		if (ch === " ") return;
		var o = el("option", null, wireToGlyph(ch));
		o.value = ch;
		if (ch === "A") o.selected = true;
		letter.appendChild(o);
	});
	wrap.appendChild(field("Test letter", letter));

	var rows = el("div", "lineup-rows");
	var list = unitList(units);
	for (var i = 0; i < c.width; i++) {
		var u = list[i];
		if (!u) continue;
		var r = el("div", "lineup-row");
		r.appendChild(el("span", "lineup-n", String(i + 1)));
		var real = input("lineup-" + i, "text", "");
		real.maxLength = 1;
		real.className = "lineup-real";
		real.setAttribute("aria-label", "What unit " + (i + 1) + " is showing");
		real.dataset.address = String(u.a);
		r.appendChild(real);
		rows.appendChild(r);
	}
	wrap.appendChild(rows);

	var acts = el("div", "rowacts");
	acts.appendChild(button("btn quiet small", "Show the letter", function () {
		var ch = letter.value;
		say(status, "Showing " + wireToGlyph(ch) + " on every unit…", "pending");
		postFields(base, { transientText: ch.repeat(c.width) })
			.then(function () {
				say(status, "Look at the wall, then fill in what you see.", "ok", 12000);
			})
			.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
	}));
	acts.appendChild(button("btn primary small", "Line them up", function () {
		applyLineUp(base, rows, letterToIndex(letter.value), status);
	}));
	wrap.appendChild(acts);
	wrap.appendChild(status);
	return wrap;
}

function applyLineUp(base, rows, expectIndex, status) {
	if (expectIndex < 0) return say(status, "Pick a test letter first.", "bad", 5000);
	var pending = [];
	rows.querySelectorAll(".lineup-real").forEach(function (node) {
		var typed = node.value.trim();
		if (!typed) return;
		var index = letterToIndex(typed);
		if (index < 0) {
			pending.push({ bad: typed });
			return;
		}
		pending.push({ node: node, address: Number(node.dataset.address), index: index });
	});
	var bad = pending.filter(function (p) { return p.bad; })[0];
	if (bad) return say(status, "'" + bad.bad + "' is not a letter a drum can show.", "bad", 6000);
	if (!pending.length) return say(status, "Type what at least one drum is showing.", "bad", 5000);

	say(status, "Lining up " + pending.length + "…", "pending");
	(function next(i) {
		if (i >= pending.length) {
			return say(status, "Lined up " + pending.length
				+ ". Look again and repeat if any are still off.", "ok", 10000);
		}
		var p = pending[i];
		getJson(base + "/unit/offset?address=" + p.address)
			.then(function (d) {
				return runOp(base, "/unit/offset", {
					address: p.address,
					value: correctedOffset(d.offset, p.index, expectIndex)
				});
			})
			.then(function () { return runOp(base, "/unit/home", { address: p.address }); })
			.then(function () {
				p.node.value = "";
				next(i + 1);
			})
			.catch(function (e) {
				say(status, "Stopped at unit " + hex(p.address) + " — " + e.message, "bad");
			});
	})(0);
}

/* 7 — its place in the wall */

function wallSection(c, s) {
	var rows = view.wall.rows.length;
	var leading = c.self && (view.settings || {}).clusterLeading;
	var clustered = view.wall.controllers.length > 1;
	var wrap = el("div", "panel");

	if (!clustered) {
		wrap.appendChild(factList([
			["Position", "Stands on its own", "not part of a multi-row wall"]
		]));
		wrap.appendChild(el("p", "hint",
			"Several boxes can drive one wall together, each holding a row. "
			+ "Add another box below to start one."));
	} else {
		wrap.appendChild(factList([
			["Position", c.width > 0 ? "Row " + c.row + " of " + rows : "No row of its own"],
			["Wall role", leading ? "Leads" : c.width > 0 ? "Follows" : "Standing by",
				leading ? "hands out the text and keeps the clock"
					: c.width > 0 ? "renders what the leader hands it"
						: "takes over the wall if the leader goes quiet"],
			leading ? null : ["Leader", leaderName() || "unknown"]
		]));
	}

	/* The wall is edited from the box that leads it. A follower shows what it
	   was told and the one thing it can do about it. */
	if (c.self && (view.settings || {}).clusterLeading) {
		wrap.appendChild(memberEditor());
	} else if (c.self && clustered) {
		wrap.appendChild(followerWallActions());
	} else if (c.self) {
		wrap.appendChild(memberEditor());
	}

	return section("Its place in the wall", wrap);
}

function memberEditor() {
	var wrap = el("div", "editor");
	var status = statusLine();
	var members = parseClusterTable((view.status && view.status.members) || []);
	if (!members.length) members = [{ host: "", row: 0, col: 0, width: view.settings.unitCount || 0 }];

	var table = el("div", "members");
	function redraw() {
		clear(table);
		members.forEach(function (m, i) {
			var r = el("div", "member");
			var host = input("member-host-" + i, "text", m.host);
			host.placeholder = "this box";
			host.setAttribute("aria-label", "Address of the box on row " + m.row);
			host.addEventListener("change", function () { m.host = host.value.trim(); });
			var row = input("member-row-" + i, "number", String(m.row));
			row.min = "0";
			row.setAttribute("aria-label", "Row");
			row.addEventListener("change", function () { m.row = Number(row.value); });
			var width = input("member-width-" + i, "number", String(m.width));
			width.min = "0";
			width.setAttribute("aria-label", "Units on this row");
			width.addEventListener("change", function () { m.width = Number(width.value); });
			r.appendChild(host);
			r.appendChild(row);
			r.appendChild(width);
			r.appendChild(button("btn quiet small", "Remove", function () {
				members.splice(i, 1);
				redraw();
			}));
			table.appendChild(r);
		});
	}
	redraw();
	wrap.appendChild(el("p", "hint",
		"One line per row: the box's address (empty means this box), which row it "
		+ "holds, and how many units are on it."));
	wrap.appendChild(table);

	var suggestions = el("div", "suggestions");
	suggestions.hidden = true;
	wrap.appendChild(suggestions);

	var acts = el("div", "rowacts");
	var scan = button("btn quiet small", "Find boxes", function () {
		scan.disabled = true;
		say(status, "Looking around the network…", "pending");
		postText("/cluster/discover").catch(function () { })
			.then(function () { return pollDiscovery("", "/cluster/discover", 20); })
			.then(function (found) {
				clear(suggestions);
				var fresh = found.filter(function (b) {
					return !members.some(function (m) { return m.host === b.host; });
				});
				if (!fresh.length) {
					say(status, "No other boxes answered.", "bad", 8000);
					suggestions.hidden = true;
					return;
				}
				say(status, "Tap one to add it as a row.", "ok", 10000);
				fresh.forEach(function (b) {
					suggestions.appendChild(button("chip",
						(b.name || b.host) + " · " + (b.width || 0) + " units", function () {
							members.push({
								host: b.host, row: members.length, col: 0,
								width: Number(b.width) || 0
							});
							redraw();
						}));
				});
				suggestions.hidden = false;
			})
			.catch(function (e) { say(status, "Search failed — " + e.message, "bad", 8000); })
			.then(function () { scan.disabled = false; });
	});
	acts.appendChild(scan);
	acts.appendChild(button("btn quiet small", "Add by address", function () {
		members.push({ host: "", row: members.length, col: 0, width: 0 });
		redraw();
	}));
	acts.appendChild(button("btn primary small", "Save the wall", function () {
		confirmThen("Save this wall? Every box listed here is told its place, and "
			+ "each one is brought onto this box's firmware.", function () {
			say(status, "Saving…", "pending");
			var body = new URLSearchParams();
			body.append("members", clusterTableSpec(members));
			fetch("/cluster/config", { method: "POST", body: body })
				.then(function (r) {
					return r.text().then(function (t) {
						if (!r.ok) throw new Error(t.trim() || ("HTTP " + r.status));
						say(status, "Saved — the other boxes join within seconds.", "ok", 8000);
						setTimeout(refresh, 1500);
					});
				})
				.catch(function (e) { say(status, "Not saved — " + e.message, "bad", 10000); });
		});
	}));
	if (view.status && view.status.enabled) {
		acts.appendChild(button("btn quiet small", "Break up the wall", function () {
			confirmThen("Break up this wall? Every box goes back to standing on its own.",
				function () {
					var body = new URLSearchParams();
					body.append("members", "");
					fetch("/cluster/config", { method: "POST", body: body })
						.then(function () {
							say(status, "Broken up.", "ok", 6000);
							setTimeout(refresh, 1500);
						})
						.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
				});
		}));
	}
	wrap.appendChild(acts);
	wrap.appendChild(status);
	wrap.appendChild(followerImagePanel(status));
	return wrap;
}

/* The stored ESP-01 row image. It lives on the leader and is pushed from
 * here, because an ESP-01 cannot be updated any other way. */
function followerImagePanel(sharedStatus) {
	var stored = (view.status && view.status.followerImage) || {};
	var wrap = el("div", "sub-panel");
	wrap.appendChild(el("h3", null, "Firmware for ESP-01 rows"));
	wrap.appendChild(el("p", "hint", stored.present
		? "Stored here: " + (stored.rev || "unidentified")
			+ ". Rows on that firmware are brought to it automatically."
		: "No ESP-01 image stored. Upload one and every ESP-01 row is brought to it."));
	wrap.appendChild(uploadControl({
		id: "follower-fw", base: "", path: "/cluster/follower-firmware",
		label: "Store the image", expect: "follower-*.bin",
		nameCheck: /^follower-/i,
		confirm: function (f) {
			return "Store " + f.name + " as the image for ESP-01 rows?\n\n"
				+ "Every ESP-01 row on this wall is brought to it.";
		},
		done: refresh
	}));
	return wrap;
}

function followerWallActions() {
	var wrap = el("div", "editor");
	var status = statusLine();
	wrap.appendChild(el("p", "hint",
		"This box follows another. Take it out of the wall to run it on its own, "
		+ "or take over the wall if the box that leads it has gone quiet."));
	var acts = el("div", "rowacts");
	acts.appendChild(button("btn quiet small", "Take over the wall", function () {
		confirmThen("Take over this wall?\n\nOnly do this if the box that leads it is "
			+ "gone — two leaders would fight over the same rows.", function () {
			say(status, "Taking over…", "pending");
			postText("/cluster/promote")
				.then(function () {
					say(status, "This box leads the wall now.", "ok", 8000);
					setTimeout(refresh, 1500);
				})
				.catch(function (e) { say(status, "Could not — " + e.message, "bad", 10000); });
		});
	}));
	acts.appendChild(button("btn quiet small", "Take out of the wall", function () {
		confirmThen("Take this box out of the wall? It goes back to standing on its own.",
			function () {
				postText("/cluster/leave")
					.then(function () {
						say(status, "Out of the wall.", "ok", 6000);
						setTimeout(refresh, 1500);
					})
					.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
			});
	}));
	wrap.appendChild(acts);
	wrap.appendChild(status);
	return wrap;
}

/* 8 — rare and consequential, last, plainly labelled. Not a gated service
 * mode: there is no lock, only last position and honest copy. */

function rareSection(c, s) {
	var base = controllerBase(c);
	var wrap = el("section", "rare");
	wrap.appendChild(el("h2", null, "When something has gone wrong"));
	wrap.appendChild(el("p", "lede", "Rarely needed, and each one interrupts the wall. "
		+ "Nothing here happens without asking you first."));

	wrap.appendChild(logPanel(c));

	var status = statusLine();
	var acts = el("div", "rowacts");

	if (s && c.plat !== "esp01") {
		acts.appendChild(button("btn quiet small", "Boot the rescue image", function () {
			confirmThen("Start this box in its rescue image?\n\nThe wall goes dark and "
				+ "the box comes back as a bare page for putting firmware on it.",
				function () {
					say(status, "Switching…", "pending");
					postText(base + "/firmware/rescue-boot")
						.then(function (t) { say(status, t || "Restarting into rescue.", "ok"); })
						.catch(function (e) { say(status, "Could not — " + e.message, "bad", 10000); });
				});
		}));
		acts.appendChild(button("btn quiet small", "Reinstall the rescue image", function () {
			confirmThen("Write the current firmware into the rescue slot?\n\nThat is the "
				+ "image the box falls back to when it cannot start.", function () {
				say(status, "Writing…", "pending");
				postText(base + "/firmware/rescue")
					.then(function (t) { say(status, t || "Written.", "ok", 10000); })
					.catch(function (e) { say(status, "Could not — " + e.message, "bad", 10000); });
			});
		}));
		acts.appendChild(button("btn quiet small", "Forget the Wi-Fi", function () {
			confirmThen("Forget this network?\n\nThe box restarts and opens its own "
				+ "network so you can point it at another one. You will lose this page.",
				function () {
					postText(base + "/reset-wifi")
						.then(function () { say(status, "Forgotten — the box is restarting.", "ok"); })
						.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
				});
		}));
	}
	wrap.appendChild(acts);
	wrap.appendChild(status);
	return wrap;
}

function logPanel(c) {
	var base = controllerBase(c);
	var wrap = el("details", "logs");
	wrap.appendChild(el("summary", null, "Read the log"));
	var pre = el("pre", "log");
	var status = statusLine();
	var acts = el("div", "rowacts");
	function load(url, label) {
		say(status, "Loading…", "pending");
		fetch(base + url, { cache: "no-store" })
			.then(function (r) { return r.text(); })
			.then(function (t) {
				pre.textContent = t || "(nothing yet)";
				pre.scrollTop = pre.scrollHeight;
				say(status, label, "ok", 4000);
			})
			.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
	}
	acts.appendChild(button("btn quiet small", "Since it started", function () {
		load("/log", "Loaded.");
	}));
	acts.appendChild(button("btn quiet small", "Kept on flash", function () {
		load("/log/flash", "Loaded.");
	}));
	acts.appendChild(button("btn quiet small", "The boot before", function () {
		load("/log/flash?prev=1", "Loaded.");
	}));
	var download = el("a", "btn quiet small", "Download");
	download.href = base + "/log/flash";
	download.setAttribute("download", "split-flap-log.txt");
	acts.appendChild(download);
	wrap.appendChild(acts);
	wrap.appendChild(status);
	wrap.appendChild(pre);
	return wrap;
}

/* ==========================================================================
 * one unit
 *
 * Level four. The unit is the smallest object on the wall and the only one
 * you touch physically, so its page answers two questions in order: is it
 * well, and is it lined up. Everything that can only be done to ONE unit
 * lives here and nowhere else.
 * ========================================================================== */

function drawUnit(c, index) {
	var got = view.remote[c.id] || {};
	var u = unitList(got.units)[index];
	byId("unit-name").textContent = "Unit " + (index + 1);
	byId("unit-back-lbl").textContent = c.name || c.host || "This box";
	byId("unit-sub").textContent = u
		? ["Row " + c.row, "address " + hex(u.a), u.rev || ""].filter(Boolean).join(" · ")
		: "Row " + c.row;

	var body = clear(byId("unit-body"));
	if (!u) {
		body.appendChild(panelOf(el("p", "none",
			"This unit is not answering, so it cannot say anything about itself. "
			+ "Check its power and its place on the bus.")));
		return;
	}

	var base = controllerBase(c);
	body.appendChild(section("How it is", panelOf(factList(unitFacts(u)))));
	body.appendChild(section("Is it lined up", unitLineUp(base, u, index)));
	body.appendChild(section("What it remembers", panelOf(factList(unitLifetime(u)))));
	body.appendChild(unitActions(base, u));
}

function unitFacts(u) {
	var homed = (u.fl & SFP.flag.homed) !== 0;
	var homeFailed = (u.fl & SFP.flag.homeFailed) !== 0;
	var hallNever = (u.fl & SFP.flag.hallNever) !== 0;
	return [
		["Answering", u.v === 1 ? "yes" : "no", null, u.v === 1 ? "" : "bad"],
		["Showing", u.mm ? "not what it was told" : "what it was told",
			u.mm ? "the drum and the box disagree" : null, u.mm ? "bad" : ""],
		["Found its home", homeFailed ? "no" : homed ? "yes" : "not yet",
			hallNever ? "its sensor has never seen the magnet" : null,
			homeFailed || hallNever ? "bad" : ""],
		["Running for", upWord(u.up)],
		["Supply now", (u.vcc / 1000).toFixed(2) + " V",
			u.vmin ? "lowest since it started: " + (u.vmin / 1000).toFixed(2) + " V" : null,
			u.vmin && u.vmin < SFP.vccFloorMv ? "bad" : ""],
		u.sag ? ["Supply while moving", (u.sag / 1000).toFixed(2) + " V",
			"the dip under load"] : null,
		["Free memory", bytesWord(u.ram), "the least it has had"],
		u.age !== undefined ? ["Last heard from", relativeAge(Math.round(u.age / 1000)),
			u.stale ? "it has missed several checks" : null, u.stale ? "bad" : ""] : null
	];
}

function unitLineUp(base, u, index) {
	var wrap = el("div", "panel");
	var status = statusLine();
	wrap.appendChild(factList([
		["Steps past home", u.se !== undefined ? u.se : "—",
			"how far it overshot the last time it homed"],
		u.sx !== undefined ? ["Worst overshoot", u.sx,
			u.sxl !== undefined ? "worst it has ever managed: " + u.sxl : null,
			u.sx > 200 ? "bad" : ""] : null,
		u.he !== undefined ? ["Magnet seen", u.he + " times last turn",
			u.he === 1 ? "once per turn is right" : "once per turn is right",
			u.he === 1 ? "" : "bad"] : null,
		["Drifted", u.de ? u.de + (u.de === 1 ? " time" : " times") : "never",
			u.ds ? "last drift: " + u.ds + " steps" : null, u.de ? "" : ""]
	]));

	var offset = input("unit-offset", "number", "");
	offset.placeholder = "reading…";
	getJson(base + "/unit/offset?address=" + u.a)
		.then(function (d) {
			if (offset.isConnected) {
				offset.value = String(d.offset);
				offset.placeholder = "";
			}
		})
		.catch(function () { offset.placeholder = "could not read it"; });
	wrap.appendChild(field("Where it stops", offset,
		"Steps between the magnet and the first letter. Bigger turns the drum further."));

	var acts = el("div", "rowacts");
	acts.appendChild(button("btn quiet small", "Nudge back", function () {
		jog(base, u.a, -10, status);
	}));
	acts.appendChild(button("btn quiet small", "Nudge on", function () {
		jog(base, u.a, 10, status);
	}));
	acts.appendChild(button("btn primary small", "Save", function () {
		var value = parseInt(offset.value, 10);
		if (isNaN(value)) return say(status, "Type a number of steps.", "bad", 5000);
		say(status, "Saving…", "pending");
		runOp(base, "/unit/offset", { address: u.a, value: value })
			.then(function () { return runOp(base, "/unit/home", { address: u.a }); })
			.then(function () { say(status, "Saved and homed.", "ok", 6000); })
			.catch(function (e) { say(status, "Not saved — " + e.message, "bad", 10000); });
	}));
	wrap.appendChild(acts);
	wrap.appendChild(el("p", "hint",
		"Nudging moves the drum now but is not remembered — use it to find the right "
		+ "number, then save."));
	wrap.appendChild(status);
	return wrap;
}

function jog(base, address, steps, status) {
	say(status, "Nudging…", "pending");
	runOp(base, "/unit/jog", { address: address, value: steps })
		.then(function () { say(status, "Nudged " + Math.abs(steps) + " steps.", "ok", 4000); })
		.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
}

function unitLifetime(u) {
	var causes = Object.keys(UNIT_RESET_WORDS)
		.filter(function (bit) { return u.mc & Number(bit); })
		.map(function (bit) { return UNIT_RESET_WORDS[bit]; });
	return [
		["Turns", u.odo !== undefined ? u.odo.toLocaleString() : "—",
			"since it was last zeroed"],
		["Power dips", u.br || 0, "times it browned out, ever"],
		["Watchdogs", u.wd || 0, "times it hung and reset itself, ever"],
		u.hf !== undefined ? ["Failed homings", u.hf, "ever", u.hf ? "bad" : ""] : null,
		["Started because", causes.length ? causes.join(", ") : "unknown"],
		u.bc ? ["Commands it did not understand", u.bc,
			"usually a sign of firmware older than this box"] : null,
		["Address from", u.ae ? "its own memory" : "the switches on the board"]
	];
}

function unitActions(base, u) {
	var wrap = el("section", "rare");
	wrap.appendChild(el("h2", null, "Things to do to this unit"));
	var status = statusLine();
	var result = el("p", "note");
	result.hidden = true;
	var acts = el("div", "rowacts");

	acts.appendChild(button("btn quiet small", "Blink its light", function () {
		say(status, "Blinking…", "pending");
		runOp(base, "/unit/identify", { address: u.a })
			.then(function () { say(status, "Look for the blinking one.", "ok", 8000); })
			.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
	}));
	acts.appendChild(button("btn quiet small", "Home it", function () {
		say(status, "Homing…", "pending");
		runOp(base, "/unit/home", { address: u.a })
			.then(function () { say(status, "Homed.", "ok", 5000); })
			.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
	}));
	acts.appendChild(button("btn quiet small", "Test it", function () {
		say(status, "Testing — this turns the drum a full circle…", "pending");
		runOp(base, "/unit/self-test", { address: u.a })
			.then(function () { return getJson(base + "/unit/self-test-result?address=" + u.a); })
			.then(function (r) { showSelfTest(result, r); say(status, "Tested.", "ok", 4000); })
			.catch(function (e) { say(status, "Could not — " + e.message, "bad", 10000); });
	}));
	acts.appendChild(button("btn quiet small", "Zero its turn count", function () {
		confirmThen("Zero this unit's turn count? Its record of how far it has "
			+ "travelled starts again from nothing.", function () {
			say(status, "Zeroing…", "pending");
			runOp(base, "/unit/reset-odometer", { address: u.a })
				.then(function () { say(status, "Zeroed.", "ok", 5000); })
				.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
		});
	}));
	acts.appendChild(button("btn quiet small", "Restart it", function () {
		confirmThen("Restart this unit? It re-homes, so its drum turns.", function () {
			say(status, "Restarting…", "pending");
			postText(base + "/unit/reboot" + query({ address: u.a }))
				.then(function () { say(status, "Restarting.", "ok", 8000); })
				.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
		});
	}));
	acts.appendChild(button("btn quiet small", "Update its firmware", function () {
		confirmThen("Flash the bundled firmware onto this unit alone?", function () {
			say(status, "Flashing…", "pending");
			postText(base + "/reflash-units" + query({ address: u.a }))
				.then(function () { say(status, "Flashing — watch the row's progress.", "ok", 10000); })
				.catch(function (e) { say(status, "Could not — " + e.message, "bad", 8000); });
		});
	}));
	wrap.appendChild(acts);
	wrap.appendChild(status);
	wrap.appendChild(result);
	wrap.appendChild(addressPanel(base, u, status));
	return wrap;
}

function showSelfTest(node, r) {
	if (!r || r.state === "pending") {
		return say(node, "The test is still running.", "pending", 8000);
	}
	if (r.outcome && r.outcome !== "ok") {
		return say(node, "It failed: " + r.outcome + ".", "bad");
	}
	var parts = [];
	if (r.window !== undefined) parts.push("magnet seen over " + r.window + " steps");
	if (r.steps !== undefined) parts.push(r.steps + " steps to the turn");
	say(node, parts.length ? "It passed — " + parts.join(", ") + "." : "It passed.", "ok");
}

/* Burning the address is the one thing here that can make a unit
 * unreachable, so it says exactly that before it does it. */
function addressPanel(base, u, sharedStatus) {
	var wrap = el("details", "danger-detail");
	wrap.appendChild(el("summary", null, "Where its address comes from"));
	wrap.appendChild(el("p", "hint",
		"A unit normally takes its address from the switches on its board. It can "
		+ "remember one instead, which is what you want before the switches are ever "
		+ "removed. An address that differs from the switches cannot be reached for a "
		+ "firmware update until it is cleared again."));
	var status = statusLine();
	var acts = el("div", "rowacts");
	acts.appendChild(button("btn quiet small", "Remember this address", function () {
		confirmThen("Have this unit remember address " + hex(u.a) + "?\n\nIt restarts "
			+ "and re-homes, and is checked on the bus afterwards.", function () {
			say(status, "Writing…", "pending");
			runOp(base, "/unit/set-address", { address: u.a, value: u.a })
				.then(function () { say(status, "Remembered and checked.", "ok", 8000); })
				.catch(function (e) { say(status, "Could not — " + e.message, "bad", 10000); });
		});
	}));
	acts.appendChild(button("btn quiet small", "Go back to the switches", function () {
		confirmThen("Clear the remembered address on this unit?", function () {
			say(status, "Clearing…", "pending");
			runOp(base, "/unit/clear-address", { address: u.a })
				.then(function () { say(status, "Cleared.", "ok", 8000); })
				.catch(function (e) { say(status, "Could not — " + e.message, "bad", 10000); });
		});
	}));
	wrap.appendChild(acts);
	wrap.appendChild(status);
	return wrap;
}
