//Departure-board web UI (#176). Vanilla JS, served gzipped from PROGMEM.
//Views: Home / Settings / Maintenance behind a persistent live flap mirror,
//plus a first-run wizard. All writes go through the same endpoints as the
//previous UI (POST / with provided-field gating, calibration opcodes,
///firmware/master, /mqtt/discover, /units/health).

//Must match SFP_ALPHABET in shared/SplitFlapProtocol.h byte-for-byte (index 0
//is blank). build_assets.py verifies this at build time and fails on drift (#149).
//ä/ö/ü are stored as $ & # (wire encoding); user-facing inputs go through
//translateLetterToIndex() which normalizes Unicode umlauts to those ASCII
//glyphs before lookup, so users can type either form.
const CALIBRATION_LETTERS = [' ','A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','$','&','#','0','1','2','3','4','5','6','7','8','9',':','.','!','?','-'];

//Curated POSIX TZ strings for the timezone dropdown (issue #48). Kept
//intentionally short — the ESP-01 serves this page from PROGMEM and the
//flash budget is tight. Strings sourced from:
//https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
const TIMEZONE_OPTIONS = [
	{ value: "",                               label: "UTC" },
	{ value: "GMT0BST,M3.5.0/1,M10.5.0",       label: "Europe/London" },
	{ value: "CET-1CEST,M3.5.0,M10.5.0/3",     label: "Europe/Amsterdam (CET/CEST)" },
	{ value: "EET-2EEST,M3.5.0/3,M10.5.0/4",   label: "Europe/Helsinki (EET/EEST)" },
	{ value: "EST5EDT,M3.2.0,M11.1.0",         label: "America/New_York" },
	{ value: "CST6CDT,M3.2.0,M11.1.0",         label: "America/Chicago" },
	{ value: "MST7MDT,M3.2.0,M11.1.0",         label: "America/Denver" },
	{ value: "PST8PDT,M3.2.0,M11.1.0",         label: "America/Los_Angeles" },
	{ value: "<-03>3",                         label: "America/Sao_Paulo" },
	{ value: "JST-9",                          label: "Asia/Tokyo" },
	{ value: "CST-8",                          label: "Asia/Shanghai" },
	{ value: "IST-5:30",                       label: "Asia/Kolkata" },
	{ value: "<+04>-4",                        label: "Asia/Dubai" },
	{ value: "AEST-10AEDT,M10.1.0,M4.1.0/3",   label: "Australia/Sydney" }
];
const CALIBRATION_STEPS_PER_FLAP = 2038 / 45;

var unitCount = 0;
var currentAlignment = "left";
var currentMode = "text";
var calibrationUnits = [];  //[{address, versionStatus, version}]
var settingsPollTimer = null;
var lastBusKey = "";
var healthPollTimer = null;
var initialised = false;

// ===================== board mirror =====================

var mirrorTiles = [];
var lastHealthUnits = [];
var mirrorShown = "";

//The device stores umlauts as $ & # on the wire; show the real glyphs.
function wireToGlyph(ch) {
	if (ch === '$') return 'Ä';
	if (ch === '&') return 'Ö';
	if (ch === '#') return 'Ü';
	return ch;
}

function buildMirror(width) {
	var mirror = document.getElementById("mirror");
	var strip = document.getElementById("healthStrip");
	while (mirror.firstChild) mirror.removeChild(mirror.firstChild);
	while (strip.firstChild) strip.removeChild(strip.firstChild);
	mirrorTiles = [];
	for (var i = 0; i < width; i++) {
		var t = document.createElement("div");
		t.className = "tile";
		t.textContent = " ";
		mirror.appendChild(t);
		mirrorTiles.push(t);
		strip.appendChild(document.createElement("span"));
	}
	mirrorShown = "";
}

//Pad the way the firmware lays out a single frame: honour the persisted
//alignment. Messages can hold literal "\n" line breaks — the physical
//display pages through lines; the mirror shows the first one.
function padForMirror(text, width, alignment) {
	text = String(text || "").split("\\n")[0].toUpperCase();
	if (text.length > width) text = text.slice(0, width);
	var spare = width - text.length;
	var left = alignment === "right" ? spare : alignment === "center" ? Math.floor(spare / 2) : 0;
	var out = "";
	for (var i = 0; i < left; i++) out += " ";
	out += text;
	while (out.length < width) out += " ";
	return out;
}

function renderMirror(text) {
	if (mirrorTiles.length === 0) return;
	var frame = padForMirror(text, mirrorTiles.length, currentAlignment);
	if (frame === mirrorShown) return;
	mirrorShown = frame;
	mirrorTiles.forEach(function(tile, i) {
		var glyph = wireToGlyph(frame[i]);
		if (glyph === " ") glyph = " ";
		if (tile.textContent === glyph) return;
		//Two renders <1 s apart must not interleave: kill this tile's
		//pending flip before scheduling the new one.
		(tile._timers || []).forEach(clearTimeout);
		tile._timers = [setTimeout(function() {
			tile.classList.remove("flip");
			void tile.offsetWidth;
			tile.classList.add("flip");
			tile._timers.push(setTimeout(function() { tile.textContent = glyph; }, 120));
		}, i * 45)];
	});
}

function setBoardStatus(text, offline) {
	var el = document.getElementById("boardStatus");
	el.textContent = text;
	el.style.color = offline ? "var(--warn)" : "";
}

// ===================== settings poll =====================

function applySettings(s) {
	unitCount = s.unitCount || 0;
	currentAlignment = s.alignment || "left";
	currentMode = s.deviceMode || "text";

	if (mirrorTiles.length !== unitCount) buildMirror(unitCount);
	renderMirror(s.lastWrittenText || "");

	document.getElementById("boardName").textContent = (s.effectiveDeviceName || "split-flap").toUpperCase();
	setBoardStatus("● LIVE · " + (currentMode === "clock" ? "CLOCK" : "TEXT"), false);
	document.getElementById("labelLastMessageReceived").textContent = s.lastTimeReceivedMessageDateTime || "—";
	setSegValue("segMode", currentMode);
	setSegValue("segAlignment", currentAlignment);

	//Calibration rows follow the bus (a reflash can re-probe), but never
	//rebuild while a Reality field holds text — that would wipe the user's
	//in-progress readings.
	var busKey = JSON.stringify([s.detectedUnitAddresses, s.detectedUnitVersionStatus]);
	if (initialised && busKey !== lastBusKey && !anyRealityFilled()) {
		setCalibrationUnitsFromSettings(s);
	}
	lastBusKey = busKey;

	//One-time form population — a 5 s poll must never stomp fields mid-edit.
	if (!initialised) {
		document.getElementById("rangeFlapSpeed").value = s.flapSpeed;
		updateSpeedSlider();
		populateTimezoneOptions();
		setTimezone(s.timezonePosix || "");
		setDeviceName(s.deviceName || "", s.effectiveDeviceName || "");
		setMqttFields(s.mqttHost || "", s.mqttPort || "", s.mqttUser || "", s.mqttPasswordSet === true);
		document.getElementById("labelVersion").textContent = s.version || "—";
		if (!s.wifiSettingsResettable) document.getElementById("cardWifi").classList.add("hidden");
		setCalibrationUnitsFromSettings(s);
	}
	setMqttPill(s.mqttHost || "", s.mqttConnected === true);
	window.lastSettings = s;  // System tab reuses version etc. (#245)
}

function loadPage() {
	document.getElementById("loadError").classList.add("hidden");
	fetch("/settings", { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(function(s) {
			applySettings(s);
			if (!initialised) {
				initialised = true;
				startUi(s);
			}
		})
		.catch(function() {
			setBoardStatus("○ OFFLINE", true);
			if (!initialised) document.getElementById("loadError").classList.remove("hidden");
		});
}

function startUi(s) {
	initTabs();
	initSegControls();
	initLogPanel();
	initSystemTab();
	initMasterFirmwareUpload();
	loadUnitHealth();

	//First run: portal-fresh device with neither a name nor a broker set.
	var fresh = !(s.deviceName) && !(s.mqttHost) && !localStorage.getItem("sf-setup-done");
	if (fresh) {
		showView("setup");
		wizardGoto(1);
	} else {
		document.getElementById("tabbar").classList.remove("hidden");
		showView(currentTabFromHash());
	}
	document.getElementById("loadedContent").classList.remove("hidden");

	settingsPollTimer = setInterval(function() {
		if (document.hidden) return;
		fetch("/settings", { cache: "no-store" })
			.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
			.then(applySettings)
			.catch(function() { setBoardStatus("○ OFFLINE", true); });
	}, 5000);

	healthPollTimer = setInterval(function() {
		if (!document.hidden) loadUnitHealth();
	}, 30000);
}

window.addEventListener("load", loadPage);

// ===================== views / tabs =====================

var TAB_NAMES = ["home", "settings", "maintenance", "system", "logs"];

function currentTabFromHash() {
	var name = location.hash.replace("#", "");
	return TAB_NAMES.indexOf(name) >= 0 ? name : "home";
}

//"setup" is a view but not a tab: the tabbar stays hidden while it's up.
function showView(name) {
	["home", "settings", "maintenance", "system", "logs", "setup"].forEach(function(view) {
		var section = document.getElementById("section-" + view);
		if (section) section.classList.toggle("on", view === name);
	});
	document.querySelectorAll(".tab-btn").forEach(function(button) {
		var on = button.dataset.tab === name;
		button.classList.toggle("on", on);
		button.setAttribute("aria-selected", on ? "true" : "false");
	});
	document.dispatchEvent(new CustomEvent("sf-tabchange", { detail: name }));
}

function initTabs() {
	document.querySelectorAll(".tab-btn").forEach(function(button) {
		button.addEventListener("click", function() {
			location.hash = button.dataset.tab;
		});
	});
	window.addEventListener("hashchange", function() {
		showView(currentTabFromHash());
	});
}

// ===================== shared post helpers =====================

//Per-card saves (#128): POST the given fields to / with ajax=1, so the
//backend answers "ok" / "ok-reboot" / 400 instead of redirecting. Only the
//posted fields are applied server-side (provided-gating), so each card can
//save independently.
function postSettingsFields(fields, callback) {
	var body = new URLSearchParams();
	Object.keys(fields).forEach(function(key) { body.append(key, fields[key]); });
	body.append("ajax", "1");
	fetch("/", { method: "POST", body: body })
		.then(function(response) {
			return response.text().then(function(text) {
				callback(response.ok, text.trim());
			});
		})
		.catch(function() { callback(false, ""); });
}

function showStatus(elementId, message, kind, hideAfterMs) {
	var el = document.getElementById(elementId);
	if (!el) return;
	el.className = "status " + (kind || "");
	el.classList.remove("hidden");
	el.innerHTML = message;
	if (hideAfterMs) {
		setTimeout(function() { el.classList.add("hidden"); }, hideAfterMs);
	}
}

//candidate.name/host come off the mDNS wire — escape anything interpolated
//into an innerHTML-based status line.
function escapeHtml(value) {
	return String(value).replace(/[&<>"']/g, function(c) {
		return "&#" + c.charCodeAt(0) + ";";
	});
}

function showBanner(message, hideAfterMs) {
	var el = document.getElementById("banner");
	el.textContent = message;
	el.classList.remove("hidden");
	if (hideAfterMs) {
		setTimeout(function() { el.classList.add("hidden"); }, hideAfterMs);
	}
}

//POST a state-changing endpoint (#145): confirm, fire, banner the response.
function postAction(url, confirmMsg) {
	if (confirmMsg && !confirm(confirmMsg)) return false;
	fetch(url, { method: "POST" })
		.then(function(r) {
			return r.text().then(function(text) {
				showBanner(r.ok ? (text || "Done") : "Request failed: HTTP " + r.status, 8000);
			});
		})
		.catch(function() { showBanner("Request failed — no connection.", 5000); });
	return false;
}

var REBOOT_NOW_LINK = ' <a href="#" onclick="return postAction(\'/reboot\', \'Reboot the display now?\');">Reboot now</a>';

// ===================== Home: composer + display =====================

function normalizeUmlauts(text) {
	return text.replace(/ä/gi, '$').replace(/ö/gi, '&').replace(/ü/gi, '#');
}

function sendMessage() {
	var text = normalizeUmlauts(document.getElementById("inputText").value);
	var dwell = document.getElementById("selectDuration").value;
	var button = document.getElementById("buttonSend");
	button.disabled = true;
	showStatus("sendStatus", "Sending…", "pending");

	var done = function(ok) {
		button.disabled = false;
		if (!ok) {
			showStatus("sendStatus", "✘ Send failed — check the message.", "error");
		} else if (dwell) {
			showStatus("sendStatus", "✔ Showing for " + (dwell / 60) + " min, then back to " + currentMode + ".", "success", 6000);
		} else {
			showStatus("sendStatus", "✔ Sent.", "success", 5000);
		}
	};

	if (dwell) {
		//Timed message: rides the firmware's show-then-revert state (#176) —
		//the persisted mode is untouched and the display reverts by itself.
		postSettingsFields({ transientText: text, transientDwell: dwell }, done);
	} else {
		//Permanent message: explicit intent to show text, so switching a
		//clock display into text mode is part of the send.
		postSettingsFields({ deviceMode: "text", inputText: text }, done);
	}
}

//Character/line meter. Messages use a literal "\n" (backslash n) as the
//line-break marker — the firmware splits frames on it.
function updateCharacterCount() {
	var text = document.getElementById("inputText").value;
	var length = text.replaceAll("\\n", "").length;
	document.getElementById("labelCharacterCount").textContent = length;
	document.getElementById("labelLineCount").textContent =
		(unitCount ? Math.ceil(length / unitCount) : 1) + text.split("\\n").length - 1;
}

function addNewline() {
	document.getElementById("inputText").value += "\\n";
	updateCharacterCount();
}

function updateSpeedSlider() {
	document.getElementById("rangeFlapSpeedValue").textContent =
		document.getElementById("rangeFlapSpeed").value;
}

function setSegValue(segId, value) {
	document.querySelectorAll("#" + segId + " button").forEach(function(b) {
		var on = b.dataset.value === value;
		b.classList.toggle("on", on);
		b.setAttribute("aria-pressed", on ? "true" : "false");
	});
}

//Mode/alignment segments and the speed slider live-apply (#128): each posts
//only its own field; the poll loop reflects the device's answer back.
function initSegControls() {
	document.querySelectorAll("#segMode button").forEach(function(b) {
		b.addEventListener("click", function() {
			setSegValue("segMode", b.dataset.value);
			postSettingsFields({ deviceMode: b.dataset.value }, function(ok) {
				showStatus("displayStatus", ok ? "✔ Mode saved." : "✘ Mode save failed.", ok ? "success" : "error", 4000);
			});
		});
	});
	document.querySelectorAll("#segAlignment button").forEach(function(b) {
		b.addEventListener("click", function() {
			setSegValue("segAlignment", b.dataset.value);
			currentAlignment = b.dataset.value;
			postSettingsFields({ alignment: b.dataset.value }, function(ok) {
				showStatus("displayStatus", ok ? "✔ Alignment saved." : "✘ Alignment save failed.", ok ? "success" : "error", 4000);
			});
		});
	});
	//"change" fires once per deliberate adjustment — never while dragging,
	//so no EEPROM churn.
	document.getElementById("rangeFlapSpeed").addEventListener("change", function(event) {
		postSettingsFields({ flapSpeed: event.target.value }, function(ok) {
			showStatus("displayStatus", ok ? "✔ Speed saved." : "✘ Speed save failed.", ok ? "success" : "error", 4000);
		});
	});
}

//Kill switch (#35): the abort flag skips the in-flight frame's waits, then
//the queued Stop broadcast-homes every unit and clears the retained text.
//Awaited (#204): the banner reports the broadcast's real outcome.
function stopDisplay() {
	if (!confirm("Stop the display? All detected units re-home to blank and the current message is cleared.")) return;
	showBanner("Stopping…", 8000);
	postCalibrationAwait("/stop", {}, function(ok, reason) {
		showBanner(ok ? "Stopped — all units homed." : "Stop failed: " + reason, 5000);
	});
}

//Blank-out recalibration (#204): '-' row → '.' row forces every drum
//through its wrap-around re-home, then the current text returns. Queued;
//fire-and-forget (the effect is watched, not polled).
function resetUnits() {
	if (!confirm("Are you sure you want to reset the units?")) return;
	fetch("/reset-units", { method: "POST" })
		.then(function(r) {
			showBanner(r.ok
				? "Display is resetting/re-calibrating — it blanks out, then the message returns."
				: "Reset request failed: HTTP " + r.status, 8000);
		})
		.catch(function() { showBanner("Reset request failed — no connection.", 5000); });
}

// ===================== Settings =====================

function populateTimezoneOptions() {
	var select = document.getElementById("selectTimezone");
	if (select.children.length > 0) return;
	TIMEZONE_OPTIONS.forEach(function(tz) {
		var opt = document.createElement("option");
		opt.value = tz.value;
		opt.textContent = tz.label;
		select.appendChild(opt);
	});
}

//If the stored POSIX TZ isn't in the dropdown (compile-time fallback or an
//older firmware), surface it as a "Custom" option so it isn't lost on save.
function setTimezone(tz) {
	var select = document.getElementById("selectTimezone");
	var match = Array.prototype.find.call(select.options, function(opt) { return opt.value === tz; });
	if (match) { select.value = tz; return; }
	var custom = document.createElement("option");
	custom.value = tz;
	custom.textContent = "Custom: " + tz;
	select.insertBefore(custom, select.firstChild);
	select.value = tz;
}

function setDeviceName(storedName, effectiveName) {
	var input = document.getElementById("inputDeviceName");
	input.value = storedName;
	input.placeholder = effectiveName;
}

function setMqttFields(host, port, user, passwordSet) {
	document.getElementById("inputMqttHost").value = host;
	document.getElementById("inputMqttPort").value = port;
	document.getElementById("inputMqttUser").value = user;
	var pw = document.getElementById("inputMqttPassword");
	pw.value = "";
	pw.placeholder = passwordSet ? "(unchanged)" : "";
}

function setMqttPill(host, connected) {
	var pill = document.getElementById("labelMqttStatus");
	if (!host) { pill.className = "pill off"; pill.textContent = "off"; }
	else if (connected) { pill.className = "pill ok"; pill.textContent = "connected"; }
	else { pill.className = "pill bad"; pill.textContent = "not connected"; }
}

function saveDeviceCard() {
	showStatus("deviceCardStatus", "Saving…", "pending");
	postSettingsFields({
		deviceName: document.getElementById("inputDeviceName").value,
		timezone: document.getElementById("selectTimezone").value
	}, function(ok, result) {
		if (!ok) showStatus("deviceCardStatus", "✘ Save failed — check the device name.", "error");
		else if (result === "ok-reboot") showStatus("deviceCardStatus", "✔ Saved. The device name applies after a reboot." + REBOOT_NOW_LINK, "success");
		else showStatus("deviceCardStatus", "✔ Saved.", "success", 5000);
	});
}

//Detect and Save share the MQTT card's fields and status line — while a
//discovery poll is in flight the save button (and vice versa) is disabled
//so the async result can't stomp a save in progress.
function setMqttCardBusy(busy) {
	document.getElementById("buttonMqttDetect").disabled = busy;
	document.getElementById("buttonMqttSave").disabled = busy;
}

function saveMqttCard() {
	setMqttCardBusy(true);
	showStatus("mqttCardStatus", "Saving…", "pending");
	postSettingsFields({
		mqttHost: document.getElementById("inputMqttHost").value,
		mqttPort: document.getElementById("inputMqttPort").value,
		mqttUser: document.getElementById("inputMqttUser").value,
		mqttPassword: document.getElementById("inputMqttPassword").value
	}, function(ok, result) {
		setMqttCardBusy(false);
		if (!ok) showStatus("mqttCardStatus", "✘ Save failed — check host and port.", "error");
		else if (result === "ok-reboot") showStatus("mqttCardStatus", "✔ Saved. MQTT settings apply after a reboot." + REBOOT_NOW_LINK, "success");
		else showStatus("mqttCardStatus", "✔ Saved (no changes).", "success", 5000);
	});
}

//MQTT broker auto-detect (#129). POST arms the discovery on the master (the
//blocking mDNS queries run in its loop()), then poll GET until done. The
//result only prefills the host/port fields — nothing persists until Save.
function detectMqttBroker() {
	var suggestions = document.getElementById("mqttSuggestions");
	setMqttCardBusy(true);
	suggestions.classList.add("hidden");
	showStatus("mqttCardStatus", "Searching the LAN for a broker…", "pending");

	fetch("/mqtt/discover", { method: "POST" })
		.then(function(response) {
			if (!response.ok && response.status !== 409) throw new Error();
			var deadline = Date.now() + 10000;
			(function poll() {
				fetch("/mqtt/discover", { cache: "no-store" })
					.then(function(r) { return r.json(); })
					.then(function(result) {
						if (result.status === "done") {
							handleDiscoverResult(result);
							setMqttCardBusy(false);
						}
						else if (Date.now() > deadline) {
							showStatus("mqttCardStatus", "✘ Discovery timed out.", "error", 5000);
							setMqttCardBusy(false);
						}
						else setTimeout(poll, 500);
					})
					.catch(function() {
						showStatus("mqttCardStatus", "✘ Discovery failed.", "error", 5000);
						setMqttCardBusy(false);
					});
			})();
		})
		.catch(function() {
			showStatus("mqttCardStatus", "✘ Discovery failed.", "error", 5000);
			setMqttCardBusy(false);
		});
}

function applyBrokerSuggestion(candidate) {
	document.getElementById("inputMqttHost").value = candidate.host;
	document.getElementById("inputMqttPort").value = candidate.port;
	showStatus("mqttCardStatus", "Prefilled " + escapeHtml(candidate.name) + " — add credentials if needed, then Save MQTT.", "success");
}

function handleDiscoverResult(result) {
	var candidates = result.candidates || [];
	var suggestions = document.getElementById("mqttSuggestions");
	while (suggestions.firstChild) suggestions.removeChild(suggestions.firstChild);
	suggestions.classList.add("hidden");

	if (candidates.length === 0) {
		showStatus("mqttCardStatus", "No broker found on the LAN. Enter the host manually.", "error", 7000);
		return;
	}
	applyBrokerSuggestion(candidates[0]);
	if (candidates.length > 1) {
		candidates.forEach(function(candidate) {
			var chip = document.createElement("button");
			chip.type = "button";
			chip.textContent = candidate.name + " (" + candidate.host + ":" + candidate.port + ")";
			chip.addEventListener("click", function() { applyBrokerSuggestion(candidate); });
			suggestions.appendChild(chip);
		});
		suggestions.classList.remove("hidden");
	}
}

// ===================== Maintenance: unit health =====================

var unitHealthPollTimer = null;

function refreshUnitHealth(probe) {
	setUnitHealthSummary("polling…", "off");
	fetch("/units/health/refresh" + (probe ? "?probe=1" : ""), { method: "POST" })
		.then(function(r) {
			if (r.status === 503) {
				//Busy = a firmware flash owns the bus right now — not an error.
				setUnitHealthSummary("busy (flashing)", "off");
				return;
			}
			//Give loop() a beat to drain the flag + poll every unit over I2C
			//before reading the fresh cache.
			if (unitHealthPollTimer !== null) clearTimeout(unitHealthPollTimer);
			unitHealthPollTimer = setTimeout(loadUnitHealth, 1200);
		})
		.catch(function() { setUnitHealthSummary("refresh failed", "bad"); });
}

function loadUnitHealth() {
	fetch("/units/health", { cache: "no-store" })
		.then(function(r) { return r.json(); })
		.then(renderUnitHealth)
		.catch(function() { setUnitHealthSummary("unreachable", "bad"); });
}

function setUnitHealthSummary(text, kind) {
	var el = document.getElementById("unitHealthSummary");
	el.textContent = text;
	el.className = "pill " + (kind || "off");
}

//MCUSR reset-cause bits on the ATmega328P. Decoded in JS so the device
//payload stays a single small integer.
var MCUSR_CAUSES = [
	[0x01, "Power-on"], [0x02, "External"], [0x04, "Brownout"], [0x08, "Watchdog"], [0x10, "JTAG"]
];
function decodeMcusr(mc) {
	var out = [];
	for (var i = 0; i < MCUSR_CAUSES.length; i++) {
		if (mc & MCUSR_CAUSES[i][0]) out.push(MCUSR_CAUSES[i][1]);
	}
	return out.length ? out.join(", ") : "—";
}

function formatUptime(sec) {
	var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
	if (h > 0) return h + "h " + m + "m";
	if (m > 0) return m + "m " + s + "s";
	return s + "s";
}

//Revolution odometer (#231): compact thousands, one decimal below 100k.
function formatOdometer(revs) {
	if (revs < 1000) return "" + revs;
	if (revs < 100000) return (revs / 1000).toFixed(1) + "k";
	if (revs < 1000000) return Math.round(revs / 1000) + "k";
	return (revs / 1000000).toFixed(1) + "M";
}

//Wear cell: odometer count, flagged units marked (server-side WearPolicy —
//the flagged list arrives in data.wear).
function wearCellText(u, wearFlagged) {
	if (typeof u.odo !== "number") return "—";
	return formatOdometer(u.odo) + (wearFlagged ? " ⚠" : "");
}

var UNIT_STATE_LABELS = { 0: "silent", 1: "sketch", 2: "bootloader" };
var UNIT_FW_LABELS = { 0: "ok", 1: "OUTDATED", 2: "unknown" };

//Mirror UnitHealth.h's unitStatusIsFaulty(): home-failed / hall-never /
//any lifetime brownout or watchdog. Only meaningful for a read unit.
function unitRowIsFaulty(u) {
	if (!u.v) return false;
	if (u.fl & 0x02) return true;   // last home failed
	if (u.fl & 0x04) return true;   // hall never triggered
	if (u.br > 0) return true;
	if (u.wd > 0) return true;
	return false;
}

function homeCellText(u) {
	if (!u.v) return "—";
	if (u.fl & 0x02) return "FAILED (" + u.hs + ")";
	if (u.fl & 0x04) return "no hall";
	return "OK (" + u.hs + ")";
}

function appendCell(row, text, cls) {
	var td = document.createElement("td");
	td.textContent = text;
	if (cls) td.className = cls;
	row.appendChild(td);
}

function removeAllChildren(element) {
	while (element.firstChild) element.removeChild(element.firstChild);
}

function renderUnitHealth(data) {
	//A (re)loaded page mid-reflash resumes the progress display (#205).
	if (data && data.reflash) trackReflashProgress(data.reflash);

	var table = document.getElementById("unitHealthTable");
	var body = document.getElementById("unitHealthBody");
	removeAllChildren(body);

	var units = (data && data.units) || [];
	var responding = 0;
	for (var k = 0; k < units.length; k++) {
		if (units[k].st !== 0) responding++;
	}
	var faulty = (data && typeof data.faulty === "number") ? data.faulty : 0;
	var width = (data && typeof data.width === "number") ? data.width : units.length;

	//Board header strip: one segment per unit slot, aligned under its tile.
	var strip = document.getElementById("healthStrip");
	var silent = [];
	if (strip.children.length === width) {
		for (var i = 0; i < width; i++) {
			var u = units[i];
			var cls = "";
			if (!u || u.st !== 1) { cls = "bad"; silent.push(i + 1); }
			else if (unitRowIsFaulty(u) || u.fw === 1) cls = "warn";
			strip.children[i].className = cls;
		}
	}
	var note = document.getElementById("healthNote");
	if (units.length === 0) {
		note.textContent = "no units detected";
	} else if (silent.length > 0) {
		note.innerHTML = responding + "/" + width + " units responding · <b>unit " +
			silent.map(function(n) { return ("0" + n).slice(-2); }).join(", ") + " silent</b>";
	} else {
		note.textContent = width + " units responding" + (faulty > 0 ? " · " + faulty + " flagged" : "");
	}

	if (units.length === 0) {
		table.classList.add("hidden");
		setUnitHealthSummary("no units", "bad");
		return;
	}
	table.classList.remove("hidden");

	var wear = (data && data.wear) || { median: 0, flagged: [] };

	for (var idx = 0; idx < units.length; idx++) {
		var unit = units[idx];
		var flagged = wear.flagged.indexOf(unit.i) !== -1;
		var row = document.createElement("tr");
		if (unitRowIsFaulty(unit)) row.className = "uh-bad-row";
		appendCell(row, unit.i);
		appendCell(row, "0x" + ("0" + unit.a.toString(16)).slice(-2));
		appendCell(row, UNIT_STATE_LABELS[unit.st] || unit.st, unit.st === 1 ? "" : "uh-bad");
		if (unit.v) {
			appendCell(row, UNIT_FW_LABELS[unit.fw] || unit.fw, unit.fw === 0 ? "" : "uh-warn");
			appendCell(row, formatUptime(unit.up));
			appendCell(row, unit.br, unit.br > 0 ? "uh-bad" : "");
			appendCell(row, unit.wd, unit.wd > 0 ? "uh-bad" : "");
			appendCell(row, unit.bc, unit.bc > 0 ? "uh-warn" : "");
			appendCell(row, homeCellText(unit), (unit.fl & 0x06) ? "uh-bad" : "");
			appendCell(row, decodeMcusr(unit.mc));
		} else {
			//Unread unit (bootloader / silent): no diagnostics.
			for (var c = 0; c < 7; c++) appendCell(row, "—");
		}
		//Wear rides its own valid flag ("odo" present) — a unit can report
		//status but run pre-odometer firmware (#231).
		appendCell(row, wearCellText(unit, flagged), flagged ? "uh-bad" : "");
		body.appendChild(row);
	}

	var summary = responding + "/" + width + (faulty > 0 ? " · " + faulty + " faulty" : " healthy");
	if (wear.flagged.length > 0 && wear.median > 0) {
		//Name the worst offender with its ratio to the display median.
		var worst = null;
		for (var w = 0; w < units.length; w++) {
			if (wear.flagged.indexOf(units[w].i) === -1) continue;
			if (worst === null || (units[w].odo || 0) > (worst.odo || 0)) worst = units[w];
		}
		if (worst) {
			summary += " · unit " + (worst.i + 1) + " wear " +
				(worst.odo / wear.median).toFixed(1) + "× median";
		}
	}
	setUnitHealthSummary(summary,
		faulty > 0 || responding < width || wear.flagged.length > 0 ? "bad" : "ok");

	lastHealthUnits = units;
	renderProvisioning(units);
}

// ===================== Maintenance: provisioning (#56) =====================

//Rows come from the health snapshot: only sketch-running units answer the
//provisioning opcodes. Address mutations are compound display commands —
//burn, settle through the unit's reboot, reprobe — so by the time the
//awaited op-result lands, the health facts are already fresh (#204).
function renderProvisioning(units) {
	var container = document.getElementById("containerProvisioning");
	removeAllChildren(container);
	units.forEach(function(u) {
		if (u.st !== 1) return;
		var row = document.createElement("div");
		row.className = "calibration-row";

		var label = document.createElement("span");
		label.className = "calibration-label";
		label.textContent = "Unit " + formatHexAddress(u.a);
		row.appendChild(label);

		var badge = document.createElement("span");
		badge.className = "calibration-ok";
		badge.textContent = u.rev ? "(fw " + u.rev + ")" : "";
		row.appendChild(badge);

		appendButton(row, "Identify", function() { identifyUnitUi(u.a); });

		var addrInput = document.createElement("input");
		addrInput.type = "number";
		addrInput.className = "calibration-offset";
		addrInput.min = "1"; addrInput.max = "16";
		addrInput.placeholder = "new";
		row.appendChild(addrInput);

		appendButton(row, "Change…", function() { changeUnitAddressUi(u.a, addrInput); });
		appendButton(row, "Clear", function() { clearUnitAddressUi(u.a); });
		appendButton(row, "Reset odo…", function() { resetOdometerUi(u.a); });
		container.appendChild(row);
	});
}

function showProvisioningStatus(message, kind) {
	var el = document.getElementById("provisioningStatus");
	el.className = "status " + (kind || "");
	el.classList.remove("hidden");
	el.textContent = message;
}

function identifyUnitUi(address) {
	postCalibration("/unit/identify", { address: address }, function(ok) {
		showProvisioningStatus(ok
			? "Unit " + formatHexAddress(address) + " is blinking its LED for ~3 s."
			: "Identify failed for " + formatHexAddress(address), ok ? "success" : "error");
	});
}

//Physical-rebuild bookkeeping (#231): the odometer is the unit's wear
//history — only zero it when the mechanics were actually replaced.
function resetOdometerUi(address) {
	if (!confirm("Reset the wear odometer of unit " + formatHexAddress(address) + "?\n\n" +
		"Only do this after physically rebuilding the unit (flap swap, new motor) — " +
		"the lifetime revolution count cannot be recovered.")) return;
	postCalibration("/unit/reset-odometer", { address: address }, function(ok) {
		showProvisioningStatus(ok
			? "Odometer of unit " + formatHexAddress(address) + " reset to 0."
			: "Odometer reset failed for " + formatHexAddress(address), ok ? "success" : "error");
		if (ok) loadUnitHealth();
	});
}

function changeUnitAddressUi(address, input) {
	var value = parseInt(input.value, 10);
	if (isNaN(value) || value < 1 || value > 16) {
		showProvisioningStatus("Enter a new address 1..16 first.", "error");
		return;
	}
	if (!confirm("Change unit " + formatHexAddress(address) + " to " + formatHexAddress(value) + "?\n\n" +
		"The unit reboots and re-homes. If the new address differs from its DIP switches, " +
		"over-I2C reflash of this unit stops working until the address is cleared.")) return;
	showProvisioningStatus("Burning address " + formatHexAddress(value) + "… (unit reboots, bus re-probes)", "pending");
	postCalibrationAwait("/unit/set-address", { address: address, value: value }, function(ok, reason) {
		if (!ok) {
			showProvisioningStatus("Address change failed for " + formatHexAddress(address) + ": " + reason, "error");
			loadUnitHealth();
			return;
		}
		//"ok" means the reprobe SAW the unit answering at its new address —
		//the facts behind /units/health are already fresh.
		showProvisioningStatus("Burned & verified — unit answers at " + formatHexAddress(value) + ".", "success");
		loadUnitHealth();
	});
}

function clearUnitAddressUi(address) {
	if (!confirm("Clear the EEPROM address of unit " + formatHexAddress(address) + "?\n\n" +
		"The unit reboots and falls back to its DIP-switch address — make sure that " +
		"address is FREE on the bus, or the unit rejoins into a collision " +
		"(recoverable by setting the DIP switches and power-cycling).")) return;
	showProvisioningStatus("Clearing… (unit reboots, bus re-probes)", "pending");
	postCalibrationAwait("/unit/clear-address", { address: address }, function(ok, reason) {
		if (!ok) {
			showProvisioningStatus("Clear failed for " + formatHexAddress(address) + ": " + reason, "error");
			loadUnitHealth();
			return;
		}
		showProvisioningStatus("Cleared — unit rejoined at its DIP address.", "success");
		loadUnitHealth();
	});
}

//Bulk migration: write each unit's CURRENT address into its own EEPROM.
//EEPROM == DIP afterwards, so nothing moves and reflash keeps working —
//this is the prep step for ever removing the DIP switches.
function burnAllAddresses() {
	//Live health snapshot, not the /settings-derived calibration list — that
	//one deliberately freezes while Reality inputs hold text.
	var targets = lastHealthUnits.filter(function(u) { return u.st === 1; })
		.map(function(u) { return u.a; });
	if (targets.length === 0) {
		showProvisioningStatus("No units detected.", "error");
		return;
	}
	if (!confirm("Burn the current address of all " + targets.length + " unit(s) into their EEPROM?\n\n" +
		"Each unit reboots, re-homes and is re-verified on the bus (~5 s per unit) — the display blanks briefly.")) return;
	showProvisioningStatus("Burning 0/" + targets.length + "…", "pending");
	var done = 0, failures = 0;
	(function next(i) {
		if (i >= targets.length) {
			showProvisioningStatus(failures === 0
				? "Burned & verified " + done + " unit(s). Addresses are now EEPROM-backed."
				: "Burned " + done + " with " + failures + " failure(s) — refresh unit health.", failures ? "error" : "success");
			loadUnitHealth();
			return;
		}
		//Awaited per unit: each burn is verified by its own reprobe before
		//the next starts, so a failure is caught immediately.
		postCalibrationAwait("/unit/set-address", { address: targets[i], value: targets[i] }, function(ok) {
			if (ok) done++; else failures++;
			showProvisioningStatus("Burning " + (i + 1) + "/" + targets.length + "…", "pending");
			next(i + 1);
		});
	})(0);
}

//Bulk unit reflash (#205): queue-native job with live progress. The POST
//answers {"seq":N}; progress rides /units/health's reflash object, polled
//every 2 s while the job runs. Every display-mutating control answers 409
//during the job (disabled here to make that visible) — Stop on the Home
//tab stays live: it doubles as the cancel.
var reflashPollTimer = null;

function setReflashStatus(text, kind) {
	var el = document.getElementById("reflashStatus");
	el.textContent = text;
	el.className = "pill " + (kind || "off");
}

function setMaintenanceControlsDisabled(disabled) {
	var btns = document.querySelectorAll("#section-maintenance button");
	for (var i = 0; i < btns.length; i++) btns[i].disabled = disabled;
}

function reflashIsRunning(rf) {
	return rf.state === "entering" || rf.state === "flashing" || rf.state === "settling";
}

function reflashProgressLabel(rf) {
	var counters = rf.done + "/" + rf.total + (rf.failed > 0 ? " (" + rf.failed + " failed)" : "");
	if (rf.state === "entering") return "entering bootloaders…";
	if (rf.state === "flashing") return "flashing 0x" + rf.cur.toString(16) + " — " + counters;
	if (rf.state === "settling") return "units homing — " + counters;
	return rf.state + " — " + counters;
}

//Also called from renderUnitHealth() so a page (re)load mid-job resumes
//the progress display instead of showing enabled controls that 409.
function trackReflashProgress(rf) {
	if (reflashIsRunning(rf)) {
		setMaintenanceControlsDisabled(true);
		setReflashStatus(reflashProgressLabel(rf), "off");
		if (reflashPollTimer !== null) clearTimeout(reflashPollTimer);
		reflashPollTimer = setTimeout(pollReflashProgress, 2000);
		return true;
	}
	return false;
}

function pollReflashProgress() {
	reflashPollTimer = null;
	fetch("/units/health", { cache: "no-store" })
		.then(function(r) { return r.json(); })
		.then(function(json) {
			var rf = json.reflash;
			if (!rf) {
				//Key missing (unexpected mid-job): keep watching rather than
				//stranding the locked controls.
				reflashPollTimer = setTimeout(pollReflashProgress, 2000);
				return;
			}
			if (trackReflashProgress(rf)) return;
			//Job over: unlock, grade, and render the job's final reprobe.
			setMaintenanceControlsDisabled(false);
			if (rf.state === "done") {
				setReflashStatus("done — " + rf.done + "/" + rf.total + " flashed", "ok");
			} else if (rf.state === "cancelled") {
				setReflashStatus("cancelled — bootloader unit(s) recover at reboot or retry", "bad");
			} else if (rf.state === "failed") {
				setReflashStatus(reflashProgressLabel(rf), "bad");
			}
			renderUnitHealth(json);
		})
		.catch(function() {
			//Transient poll failure mid-job: keep watching.
			reflashPollTimer = setTimeout(pollReflashProgress, 2000);
		});
}

function reflashAllUnits() {
	if (!confirm("Push every unit not on the bundled firmware through its bootloader and re-flash it?\n\n" +
		"Takes a few minutes (2 units at a time, then a homing pause). The display freezes and " +
		"other controls lock until it finishes. Stop cancels.")) return;
	fetch("/reflash-units", { method: "POST" })
		.then(function(r) {
			if (r.status === 409) { showBanner("A reflash is already running.", 5000); return; }
			if (!r.ok) { showBanner("Reflash request failed: HTTP " + r.status, 8000); return; }
			setMaintenanceControlsDisabled(true);
			setReflashStatus("queued…", "off");
			if (reflashPollTimer !== null) clearTimeout(reflashPollTimer);
			reflashPollTimer = setTimeout(pollReflashProgress, 1000);
		})
		.catch(function() { showBanner("Request failed — no connection.", 5000); });
}

// ===================== Maintenance: master firmware =====================

//Derive "&v=<rev>" for /firmware/master from a build-stamped filename
//(firmware-<rev>[-dirty]-<size>.bin, see build_assets.py). The rev feeds
//the boot-time silent-revert check (intendedVersion, #52).
function firmwareVersionParam(fileName) {
	var match = fileName.match(/^firmware-([0-9a-f]{7,40}(?:-dirty)?)(?:-[0-9]+m[0-9]*m?)?\.bin$/i);
	return match ? "&v=" + encodeURIComponent(match[1]) : "";
}

function initMasterFirmwareUpload() {
	var form = document.getElementById("masterFirmwareForm");

	form.addEventListener("submit", function(event) {
		event.preventDefault();

		var fileInput = document.getElementById("inputMasterFirmwareFile");
		var submitButton = document.getElementById("buttonMasterFirmwareSubmit");

		var file = fileInput.files[0];
		if (!file) return;

		if (!confirm("Flash " + file.name + " (" + file.size + " bytes) to the master?\n\n" +
			"The ESP reboots into the new firmware on success. On failure, the current firmware keeps running.")) return;

		submitButton.disabled = true;
		fileInput.disabled = true;
		showStatus("masterFirmwareStatus", "Computing MD5…", "pending");

		//?md5= is mandatory on /firmware/master (#144): hash the image
		//client-side (SparkMD5, md5.js) so the handler can verify the upload
		//arrived intact before committing it (#160).
		var reader = new FileReader();
		reader.onerror = function() {
			submitButton.disabled = false;
			fileInput.disabled = false;
			showStatus("masterFirmwareStatus", "✘ Could not read the selected file.", "error");
		};
		reader.onload = function() {
			var md5 = SparkMD5.ArrayBuffer.hash(reader.result);
			showStatus("masterFirmwareStatus", "Uploading master firmware…", "pending");

			var formData = new FormData();
			formData.append("firmware", file);

			var xhr = new XMLHttpRequest();
			xhr.open("POST", "/firmware/master?md5=" + md5 + firmwareVersionParam(file.name));
			xhr.onreadystatechange = function() {
				if (xhr.readyState !== 4) return;
				submitButton.disabled = false;
				fileInput.disabled = false;
				if (xhr.status === 200) {
					showStatus("masterFirmwareStatus", "✔ " + escapeHtml(xhr.responseText) + " The master will be offline ~15 s while rebooting.", "success");
				} else if (xhr.status === 0) {
					showStatus("masterFirmwareStatus", "✘ Upload failed — lost connection to master.", "error");
				} else {
					showStatus("masterFirmwareStatus", "✘ HTTP " + xhr.status + ": " + escapeHtml(xhr.responseText), "error");
				}
			};
			xhr.send(formData);
		};
		reader.readAsArrayBuffer(file);
	});
}

// ===================== Logs tab =====================

//Polls GET /log every 2 s, but only while the log is actually visible:
//<details> open, browser tab foreground AND the Logs view active.
function initLogPanel() {
	var details = document.getElementById("logDetails");
	var pre = document.getElementById("logContent");
	var pollHandle = null;
	var onLogsTab = false;

	function fetchLog() {
		fetch("/log", { cache: "no-store" })
			.then(function(r) { if (!r.ok) throw new Error(); return r.text(); })
			.then(function(text) {
				//If the user was already at the bottom, stay pinned there.
				var atBottom = (pre.scrollTop + pre.clientHeight) >= (pre.scrollHeight - 8);
				pre.textContent = text;
				if (atBottom) pre.scrollTop = pre.scrollHeight;
			})
			.catch(function() { /* network hiccups shouldn't blow up the UI */ });
	}

	function syncPolling() {
		var want = details.open && !document.hidden && onLogsTab;
		if (want && pollHandle === null) {
			fetchLog();
			pollHandle = setInterval(fetchLog, 2000);
		} else if (!want && pollHandle !== null) {
			clearInterval(pollHandle);
			pollHandle = null;
		}
	}

	details.addEventListener("toggle", syncPolling);
	document.addEventListener("sf-tabchange", function(event) {
		onLogsTab = event.detail === "logs";
		syncPolling();
	});
	document.addEventListener("visibilitychange", syncPolling);
}

// ===================== System tab (#245) =====================
//Polls /system/stats at 2 s while the tab is visible; the device keeps
//~10 min of 5 s samples server-side, so the charts have depth immediately.

function formatBytes(n) {
	if (n >= 1048576) return (n / 1048576).toFixed(1) + " MB";
	if (n >= 1024) return Math.round(n / 1024) + " KB";
	return n + " B";
}

//Min-max normalized polyline in a 100x28 viewBox. Flat series draw a
//midline instead of dividing by zero.
function drawSparkline(svgId, series) {
	var svg = document.getElementById(svgId);
	if (!svg) return;
	removeAllChildren(svg);
	if (!series || series.length < 2) return;
	var min = Math.min.apply(null, series), max = Math.max.apply(null, series);
	var span = max - min;
	var points = [];
	for (var i = 0; i < series.length; i++) {
		var x = (i / (series.length - 1)) * 100;
		var y = span === 0 ? 14 : 26 - ((series[i] - min) / span) * 24;
		points.push(x.toFixed(1) + "," + y.toFixed(1));
	}
	var line = document.createElementNS("http://www.w3.org/2000/svg", "polyline");
	line.setAttribute("points", points.join(" "));
	svg.appendChild(line);
}

function setStat(id, text) {
	var el = document.getElementById(id);
	if (el) el.textContent = text;
}

function renderSystemStats(data) {
	var now = data.now || {}, hist = data.hist || {};
	document.getElementById("systemStatsStatus").className = "pill ok";
	document.getElementById("systemStatsStatus").textContent = "live";
	setStat("statRssi", now.rssi === 0 ? "—" : now.rssi + " dBm");
	setStat("statHeap", formatBytes(now.heap || 0));
	setStat("statCpu0", (now.cpu0 || 0) + "%");
	setStat("statCpu1", (now.cpu1 || 0) + "%");
	setStat("statTemp", ((now.temp || 0) / 10).toFixed(1) + " °C");
	setStat("statPsram", formatBytes(now.psram || 0));
	setStat("statMaxAlloc", "largest block " + formatBytes(now.maxAlloc || 0));
	setStat("statI2cTx", now.i2cTx);
	setStat("statI2cErr", now.i2cErr);
	setStat("statMqttDrops", now.mqttDrops);
	setStat("statNtpAge", now.ntpAge < 0 ? "never" : formatUptime(now.ntpAge) + " ago");
	setStat("statUptime", formatUptime(now.uptime || 0));
	setStat("statReset", now.reset || "—");
	setStat("statMinHeap", formatBytes(now.minHeap || 0));
	drawSparkline("sparkRssi", hist.rssi);
	drawSparkline("sparkHeap", hist.heap);
	drawSparkline("sparkCpu0", hist.cpu0);
	drawSparkline("sparkCpu1", hist.cpu1);
	drawSparkline("sparkTemp", hist.temp);
}

function initSystemTab() {
	var pollHandle = null;
	var onSystemTab = false;

	function fetchStats() {
		fetch("/system/stats", { cache: "no-store" })
			.then(function(r) { return r.json(); })
			.then(renderSystemStats)
			.catch(function() {
				var pill = document.getElementById("systemStatsStatus");
				pill.className = "pill off";
				pill.textContent = "unreachable";
			});
	}

	function syncPolling() {
		var want = onSystemTab && !document.hidden;
		if (want && pollHandle === null) {
			fetchStats();
			pollHandle = setInterval(fetchStats, 2000);
		} else if (!want && pollHandle !== null) {
			clearInterval(pollHandle);
			pollHandle = null;
		}
	}

	document.addEventListener("sf-tabchange", function(event) {
		onSystemTab = event.detail === "system";
		syncPolling();
		//The firmware version already rides /settings — reuse the cached copy.
		if (onSystemTab && window.lastSettings && window.lastSettings.version) {
			setStat("statVersion", window.lastSettings.version);
		}
	});
	document.addEventListener("visibilitychange", syncPolling);
}

//Flash log (#206): manual load — a file up to 1 MB has no business auto-polling.
function showFlashLogStatus(message, kind) {
	var el = document.getElementById("flashLogStatus");
	el.className = "status " + (kind || "");
	el.classList.remove("hidden");
	el.textContent = message;
}

function loadFlashLog(previous) {
	var pre = document.getElementById("flashLogContent");
	showFlashLogStatus("Loading…", "pending");
	fetch("/log/flash" + (previous ? "?prev=1" : ""), { cache: "no-store" })
		.then(function(r) {
			if (!r.ok) return r.text().then(function(t) { throw new Error(t || ("HTTP " + r.status)); });
			return r.text();
		})
		.then(function(text) {
			pre.textContent = text;
			pre.classList.remove("hidden");
			pre.scrollTop = pre.scrollHeight;
			showFlashLogStatus((previous ? "Previous" : "Current") + " file, " +
				Math.round(text.length / 1024) + " KB (flushed every ~5 s).", "success");
		})
		.catch(function(e) {
			pre.classList.add("hidden");
			showFlashLogStatus("Load failed: " + e.message, "error");
		});
}

function clearFlashLog() {
	if (!confirm("Delete the flash log (current + previous file)?")) return;
	fetch("/log/flash/clear", { method: "POST" })
		.then(function(r) {
			if (!r.ok) throw new Error("HTTP " + r.status);
			document.getElementById("flashLogContent").classList.add("hidden");
			showFlashLogStatus("Clear queued — files are deleted within a few seconds.", "success");
		})
		.catch(function(e) { showFlashLogStatus("Clear failed: " + e.message, "error"); });
}

// ===================== Maintenance: calibration =====================

function anyRealityFilled() {
	return Array.prototype.some.call(
		document.querySelectorAll("#containerCalibrationUnits .calibration-reality"),
		function(el) { return el.value.trim() !== ""; });
}

function setCalibrationUnitsFromSettings(s) {
	//Keyed off detectedUnitAddresses — a sketch-running unit that didn't
	//reply to CMD_GET_VERSION predates the calibration opcodes; filter it.
	var calUnits = [];
	var addresses = s.detectedUnitAddresses || [];
	var versionStatuses = s.detectedUnitVersionStatus || [];
	var versions = s.detectedUnitVersions || [];
	for (var i = 0; i < addresses.length; i++) {
		var addr = addresses[i];
		calUnits.push({
			address: addr,
			versionStatus: versionStatuses[addr - 1],
			version: versions[addr - 1] || ""
		});
	}
	setCalibrationUnits(calUnits);
}

function setCalibrationUnits(units) {
	calibrationUnits = units || [];
	var card = document.getElementById("calibrationCard");
	var container = document.getElementById("containerCalibrationUnits");
	var advancedContainer = document.getElementById("containerCalibrationAdvanced");

	removeAllChildren(container);
	removeAllChildren(advancedContainer);

	if (calibrationUnits.length === 0) {
		card.classList.add("hidden");
		return;
	}
	card.classList.remove("hidden");

	//Populate the shared <datalist> (referenced by every Reality input) and
	//the test-letter <select>. Skip blank (index 0) — "expect blank" is
	//ambiguous to eyeball. Default to "A".
	var datalist = document.getElementById("calibrationLetters");
	var select = document.getElementById("selectCalibrationLetter");
	if (datalist.children.length === 0) {
		for (var i = 0; i < CALIBRATION_LETTERS.length; i++) {
			var opt = document.createElement("option");
			opt.value = CALIBRATION_LETTERS[i];
			datalist.appendChild(opt);
		}
	}
	if (select.children.length === 0) {
		for (var j = 1; j < CALIBRATION_LETTERS.length; j++) {
			var sopt = document.createElement("option");
			sopt.value = CALIBRATION_LETTERS[j];
			sopt.textContent = CALIBRATION_LETTERS[j];
			select.appendChild(sopt);
		}
		select.value = "A";
	}

	calibrationUnits.forEach(function(unit) {
		container.appendChild(buildCalibrationRow(unit, false));
		advancedContainer.appendChild(buildCalibrationRow(unit, true));
	});
}

function buildCalibrationRow(unit, advanced) {
	var row = document.createElement("div");
	row.className = "calibration-row";
	row.dataset.address = unit.address;

	var label = document.createElement("span");
	label.className = "calibration-label";
	label.textContent = "Unit " + formatHexAddress(unit.address);
	row.appendChild(label);

	//Version badge. Green = on the bundled firmware (#120). Amber =
	//outdated/unknown: older firmware silently drops the opcodes.
	var badge = document.createElement("span");
	if (unit.versionStatus === 0) {
		badge.className = "calibration-ok";
		badge.textContent = unit.version ? "(fw " + unit.version + ")" : "(fw ok)";
	} else {
		badge.className = "calibration-warn";
		badge.textContent = unit.versionStatus === 1 ? "(outdated fw)" : "(fw unknown)";
	}
	row.appendChild(badge);

	if (advanced) buildAdvancedControls(row, unit);
	else buildExpectRealityControls(row);
	return row;
}

function buildExpectRealityControls(row) {
	var expect = document.createElement("span");
	expect.className = "calibration-expect";
	expect.textContent = "Expect: —";
	row.appendChild(expect);

	var realityLabel = document.createElement("span");
	realityLabel.textContent = "Reality: ";
	row.appendChild(realityLabel);

	var reality = document.createElement("input");
	reality.type = "text";
	reality.maxLength = 1;
	reality.className = "calibration-reality";
	reality.setAttribute("list", "calibrationLetters");
	reality.autocomplete = "off";
	row.appendChild(reality);
}

function buildAdvancedControls(row, unit) {
	var offsetLabel = document.createElement("span");
	offsetLabel.textContent = "offset ";
	row.appendChild(offsetLabel);

	var offsetInput = document.createElement("input");
	offsetInput.type = "number";
	offsetInput.className = "calibration-offset";
	offsetInput.placeholder = "?";
	offsetInput.step = "1";
	row.appendChild(offsetInput);

	appendButton(row, "Read", function() { readCalibrationOffset(row); });
	appendButton(row, "Save", function() { saveCalibrationOffset(row); });
	appendButton(row, "Home", function() { homeCalibrationUnit(unit.address); });

	var jogLabel = document.createElement("span");
	jogLabel.textContent = " Jog: ";
	row.appendChild(jogLabel);

	[-10, -1, 1, 10].forEach(function(n) {
		appendButton(row, (n > 0 ? "+" : "") + n, function() {
			jogCalibrationUnit(unit.address, n);
		});
	});
}

function appendButton(parent, label, handler) {
	var btn = document.createElement("button");
	btn.type = "button";
	btn.className = "btn mini";
	btn.textContent = label;
	btn.addEventListener("click", handler);
	parent.appendChild(btn);
}

function formatHexAddress(addr) {
	return "0x" + addr.toString(16).padStart(2, "0").toUpperCase();
}

function getSelectedCalibrationLetter() {
	var select = document.getElementById("selectCalibrationLetter");
	return select ? select.value : "A";
}

function translateLetterToIndex(ch) {
	if (!ch) return -1;
	//Normalize to the ASCII wire encoding used by the master's letters[].
	//Users may type either Ä or $; both resolve to index 27.
	if (ch === 'ä' || ch === 'Ä') ch = '$';
	else if (ch === 'ö' || ch === 'Ö') ch = '&';
	else if (ch === 'ü' || ch === 'Ü') ch = '#';
	else ch = ch.toUpperCase();
	return CALIBRATION_LETTERS.indexOf(ch);
}

//"Send to all" shows the chosen letter repeated unitCount times as a
//TRANSIENT (#165): the firmware routes transientText through its
//show-then-revert state, so the persisted device mode is never touched.
function sendCalibrationLetter() {
	if (unitCount === 0) {
		showCalibrationStatus("No units detected.", "error");
		return;
	}
	var letter = getSelectedCalibrationLetter();
	showCalibrationStatus("Sending '" + letter + "' to all detected units…", "pending");

	var padded = "";
	for (var i = 0; i < unitCount; i++) padded += letter;

	postSettingsFields({ transientText: padded }, function(ok) {
		if (ok) {
			showCalibrationStatus("Sent '" + letter + "'. Fill Reality for each unit and click Apply All.", "success");
			document.querySelectorAll("#containerCalibrationUnits .calibration-expect").forEach(function(el) {
				el.textContent = "Expect: " + letter;
			});
		} else {
			showCalibrationStatus("Failed to send test letter.", "error");
		}
	});
}

function homeAllCalibrationUnits() {
	var remaining = calibrationUnits.length;
	if (remaining === 0) {
		showCalibrationStatus("No units detected.", "error");
		return;
	}
	showCalibrationStatus("Re-homing all detected units…", "pending");
	var failures = 0;
	calibrationUnits.forEach(function(unit) {
		postCalibration("/unit/home", { address: unit.address }, function(ok) {
			if (!ok) failures++;
			remaining--;
			if (remaining === 0) {
				if (failures === 0) showCalibrationStatus("Re-homed " + calibrationUnits.length + " unit(s).", "success");
				else showCalibrationStatus("Re-homed with " + failures + " failure(s).", "error");
			}
		});
	});
}

function homeCalibrationUnit(address) {
	postCalibration("/unit/home", { address: address }, function(ok) {
		showCalibrationStatus(ok
			? "Homed " + formatHexAddress(address)
			: "Home failed for " + formatHexAddress(address), ok ? "success" : "error");
	});
}

function jogCalibrationUnit(address, steps) {
	postCalibration("/unit/jog", { address: address, steps: steps }, function(ok) {
		showCalibrationStatus(ok
			? "Jogged " + formatHexAddress(address) + " by " + steps + " step(s)"
			: "Jog failed for " + formatHexAddress(address), ok ? "success" : "error");
	});
}

function readCalibrationOffset(row) {
	var address = parseInt(row.dataset.address, 10);
	fetch("/unit/offset?address=" + address, { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
		.then(function(data) {
			row.querySelector(".calibration-offset").value = data.offset;
			showCalibrationStatus("Read offset " + data.offset + " from " + formatHexAddress(address), "success");
		})
		.catch(function() {
			showCalibrationStatus("Read offset failed for " + formatHexAddress(address), "error");
		});
}

function saveCalibrationOffset(row) {
	var address = parseInt(row.dataset.address, 10);
	var value = parseInt(row.querySelector(".calibration-offset").value, 10);
	if (isNaN(value)) {
		showCalibrationStatus("Offset for " + formatHexAddress(address) + " is not a number", "error");
		return;
	}
	postCalibrationAwait("/unit/offset", { address: address, value: value }, function(ok, reason) {
		showCalibrationStatus(ok
			? "Saved offset " + value + " to " + formatHexAddress(address)
			: "Save offset failed for " + formatHexAddress(address) + ": " + reason, ok ? "success" : "error");
	});
}

//Apply All flow: for every row with a filled Reality input, compute the
//corrective offset, write it, then trigger a re-home. Runs one unit at a
//time (no concurrent I2C). Rows with empty Reality are skipped.
function applyCalibrationAll() {
	var expectLetter = getSelectedCalibrationLetter();
	var expectIndex = translateLetterToIndex(expectLetter);
	if (expectIndex < 0) {
		showCalibrationStatus("Invalid test letter '" + expectLetter + "'", "error");
		return;
	}
	var rows = Array.prototype.slice.call(document.querySelectorAll("#containerCalibrationUnits .calibration-row"));
	var pending = rows
		.map(function(row) {
			var reality = row.querySelector(".calibration-reality").value.trim();
			if (!reality) return null;
			var realityIndex = translateLetterToIndex(reality);
			if (realityIndex < 0) return { error: "Invalid Reality '" + reality + "'", row: row };
			return { row: row, realityIndex: realityIndex };
		})
		.filter(function(x) { return x !== null; });

	if (pending.length === 0) {
		showCalibrationStatus("Fill at least one Reality input first.", "error");
		return;
	}
	var invalid = pending.filter(function(p) { return p.error; });
	if (invalid.length > 0) {
		showCalibrationStatus(invalid[0].error, "error");
		return;
	}

	showCalibrationStatus("Applying corrections to " + pending.length + " unit(s)…", "pending");
	applyCalibrationNext(pending, 0, expectIndex);
}

function applyCalibrationNext(pending, index, expectIndex) {
	if (index >= pending.length) {
		showCalibrationStatus("Applied " + pending.length + " correction(s). Check displays and re-apply if still off.", "success");
		return;
	}
	var entry = pending[index];
	var address = parseInt(entry.row.dataset.address, 10);

	//Pull current offset, compute correction, write, re-home.
	fetch("/unit/offset?address=" + address, { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(function(data) {
			//new_offset = current − (reality − expect) × steps_per_flap.
			//If the drum shows reality = expect + 1 flap, it over-rotated by
			//one flap, so stop one flap earlier.
			var delta = entry.realityIndex - expectIndex;
			var correction = Math.round(delta * CALIBRATION_STEPS_PER_FLAP);
			var newOffset = data.offset - correction;

			postCalibrationAwait("/unit/offset", { address: address, value: newOffset }, function(ok, reason) {
				if (!ok) {
					showCalibrationStatus("Save offset failed for " + formatHexAddress(address) + ": " + reason, "error");
					return;
				}
				postCalibration("/unit/home", { address: address }, function(homeOk) {
					if (!homeOk) {
						showCalibrationStatus("Re-home failed for " + formatHexAddress(address), "error");
						return;
					}
					entry.row.querySelector(".calibration-reality").value = "";
					applyCalibrationNext(pending, index + 1, expectIndex);
				});
			});
		})
		.catch(function() {
			showCalibrationStatus("Read offset failed for " + formatHexAddress(address) + " — aborting", "error");
		});
}

function postCalibration(path, params, callback) {
	var query = Object.keys(params).map(function(k) {
		return encodeURIComponent(k) + "=" + encodeURIComponent(params[k]);
	}).join("&");
	fetch(path + "?" + query, { method: "POST" })
		.then(function(r) { callback(r.ok); })
		.catch(function() { callback(false); });
}

//Queue-native execution feedback (#204). Every maintenance POST answers
//{"seq":N} when the op is queued; the outcome arrives later through
///unit/op-result. Jog/home/identify stay fire-and-forget (the operator
//watches the hardware) — EEPROM-mutating ops await the real outcome here.
//The single result slot serves ONE critical op at a time, so the
//Maintenance controls lock while a poll is in flight.
var maintAwaitDepth = 0;

function setMaintenanceBusy(busy) {
	maintAwaitDepth += busy ? 1 : -1;
	if (maintAwaitDepth < 0) maintAwaitDepth = 0;
	var section = document.getElementById("section-maintenance");
	if (section) section.classList.toggle("maint-busy", maintAwaitDepth > 0);
}

//callback(ok, reason): ok=true means the op EXECUTED successfully (wire ACK
//+ postcondition for address ops), not merely that it queued.
function postCalibrationAwait(path, params, callback) {
	var query = Object.keys(params).map(function(k) {
		return encodeURIComponent(k) + "=" + encodeURIComponent(params[k]);
	}).join("&");
	setMaintenanceBusy(true);
	fetch(path + (query ? "?" + query : ""), { method: "POST" })
		.then(function(r) {
			if (!r.ok) return r.text().then(function(t) { throw new Error(t || ("HTTP " + r.status)); });
			return r.json();
		})
		.then(function(data) { pollOpResult(data.seq, 30, callback); })
		.catch(function(e) {
			setMaintenanceBusy(false);
			callback(false, e && e.message ? e.message : "request failed");
		});
}

//Address ops settle ~3 s + reprobe before their result lands; 30 × 500 ms
//also survives a queued show frame ahead of the op.
function pollOpResult(seq, remaining, callback) {
	fetch("/unit/op-result?seq=" + seq, { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(function(res) {
			if (res.state === "pending" && remaining > 0) {
				setTimeout(function() { pollOpResult(seq, remaining - 1, callback); }, 500);
				return;
			}
			setMaintenanceBusy(false);
			if (res.state === "ok") { callback(true, ""); return; }
			var reason;
			if (res.state === "pending") reason = "still queued — display busy; check unit health in a moment";
			else if (res.state === "expired") reason = "outcome unknown (superseded) — refresh unit health";
			else reason = (res.reason || "failed") + (res.detail ? " (" + res.detail + ")" : "");
			callback(false, reason);
		})
		.catch(function() {
			if (remaining > 0) {
				setTimeout(function() { pollOpResult(seq, remaining - 1, callback); }, 500);
				return;
			}
			setMaintenanceBusy(false);
			callback(false, "op-result poll failed");
		});
}

function showCalibrationStatus(message, kind) {
	var el = document.getElementById("calibrationStatus");
	el.className = "status " + (kind || "");
	el.classList.remove("hidden");
	el.textContent = message;
}

// ===================== first-run wizard =====================

//Shown instead of the tabs when the device is portal-fresh: no stored name,
//no broker, and the wizard wasn't dismissed on this browser before. Never
//blocks — every path lands on Home.
function wizardGoto(step) {
	for (var i = 1; i <= 3; i++) {
		document.getElementById("wizardStep" + i).classList.toggle("hidden", i !== step);
		document.getElementById("step" + i).className = i <= step ? "done" : "";
	}
	if (step === 2) {
		document.getElementById("wizardDeviceName").placeholder =
			document.getElementById("boardName").textContent.toLowerCase();
	}
}

function wizardSaveName() {
	var name = document.getElementById("wizardDeviceName").value;
	if (!name) { wizardGoto(3); return; }
	showStatus("wizardNameStatus", "Saving…", "pending");
	postSettingsFields({ deviceName: name }, function(ok) {
		if (!ok) {
			showStatus("wizardNameStatus", "✘ That name was rejected — letters, digits and dashes only.", "error");
			return;
		}
		showStatus("wizardNameStatus", "✔ Saved — applies on the next reboot.", "success", 4000);
		wizardGoto(3);
	});
}

function wizardSaveMqtt() {
	var host = document.getElementById("wizardMqttHost").value;
	if (!host) { wizardFinish(false); return; }
	showStatus("wizardMqttStatus", "Saving…", "pending");
	postSettingsFields({
		mqttHost: host,
		mqttPort: document.getElementById("wizardMqttPort").value
	}, function(ok) {
		if (!ok) {
			showStatus("wizardMqttStatus", "✘ Save failed — check host and port.", "error");
			return;
		}
		wizardFinish(true);
	});
}

function wizardFinish(savedMqtt) {
	localStorage.setItem("sf-setup-done", "1");
	document.getElementById("tabbar").classList.remove("hidden");
	location.hash = "home";
	showView("home");
	showBanner(savedMqtt
		? "Setup complete. Name and MQTT apply after a reboot — Maintenance → Reboot when ready."
		: "Setup complete. You can name the display or connect MQTT any time under Settings.", 10000);
}
