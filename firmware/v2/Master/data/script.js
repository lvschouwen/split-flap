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
//Cluster wall (#277): non-null while the SSE stream carries grid rows —
//the leader's mirror then renders the WHOLE wall, one strip per row.
var wallWidths = null;
var wallSelfRow = 0;
//Who built the wall (#294): "sse" = this board leads (rows ride /events),
//"digest" = this board follows and mirrors the leader's ping-piggybacked
//digest. Each source owns its own collapse rule in applySettings.
var wallSource = null;
//Per-row health strips for rows that are NOT this board's own (#294) —
//null for the self row (the physical healthStrip covers it).
var wallRowStrips = [];
var mirrorRowTiles = [];

//The device stores umlauts as $ & # on the wire; show the real glyphs.
function wireToGlyph(ch) {
	if (ch === '$') return 'Ä';
	if (ch === '&') return 'Ö';
	if (ch === '#') return 'Ü';
	return ch;
}

//Two-leaf tile (#246): the .top half folds over the hinge carrying
//the old glyph while the full-height .char swaps underneath it.
function buildTile() {
	var t = document.createElement("div");
	t.className = "tile";
	var top = document.createElement("span");
	top.className = "top";
	//Full-tile-height inner span, clipped by the half-height leaf, so the
	//leaf shows exactly the top half of a properly centered glyph.
	var tg = document.createElement("span");
	tg.className = "tg";
	top.appendChild(tg);
	var ch = document.createElement("span");
	ch.className = "char";
	t.appendChild(top);
	t.appendChild(ch);
	t._glyph = "";
	return t;
}

//The health strip tracks THIS board's physical units (never remote wall
//rows), so it rebuilds from unitCount alone.
function buildStrip(n) {
	var strip = document.getElementById("healthStrip");
	while (strip.firstChild) strip.removeChild(strip.firstChild);
	for (var i = 0; i < n; i++) strip.appendChild(document.createElement("span"));
}

function clearMirror() {
	var mirror = document.getElementById("mirror");
	//The wall build reparents the health strip under the own row — put it
	//back before clearing or the rebuild would delete it.
	var note = document.getElementById("healthNote");
	note.parentNode.insertBefore(document.getElementById("healthStrip"), note);
	//Discarded tiles must not keep riffling against detached DOM nodes —
	//kill their timers before the rebuild drops the references.
	mirrorTiles.forEach(function(tile) {
		(tile._timers || []).forEach(clearTimeout);
		if (tile._riffle) clearInterval(tile._riffle);
	});
	while (mirror.firstChild) mirror.removeChild(mirror.firstChild);
	mirrorTiles = [];
	mirrorRowTiles = [];
	wallRowStrips = [];
	mirrorShown = "";
}

function buildMirror(width) {
	clearMirror();
	wallWidths = null;
	wallSource = null;
	document.getElementById("mirror").classList.remove("stale");
	var mirror = document.getElementById("mirror");
	mirror.classList.remove("wall");
	mirror.style.removeProperty("--wallcols");
	document.getElementById("healthStrip").style.removeProperty("width");
	for (var i = 0; i < width; i++) {
		var t = buildTile();
		mirror.appendChild(t);
		mirrorTiles.push(t);
	}
	buildStrip(width);
}

//Cluster wall (#277): one strip per grid row, every row sharing the tile
//size of the widest row (--wallcols), left-aligned at col 0. The health
//strip moves under the own row — it shows this board's units only.
function buildWall(widths, selfRow) {
	clearMirror();
	wallWidths = widths.slice();
	wallSelfRow = selfRow;
	var mirror = document.getElementById("mirror");
	mirror.classList.add("wall");
	var maxW = Math.max.apply(null, widths);
	mirror.style.setProperty("--wallcols", maxW);
	wallRowStrips = [];
	for (var r = 0; r < widths.length; r++) {
		var rowEl = document.createElement("div");
		rowEl.className = "mrow";
		rowEl.style.width = (widths[r] / maxW * 100) + "%";
		var tiles = [];
		for (var i = 0; i < widths[r]; i++) {
			var t = buildTile();
			rowEl.appendChild(t);
			tiles.push(t);
			mirrorTiles.push(t);
		}
		mirror.appendChild(rowEl);
		mirrorRowTiles.push(tiles);
		if (r === selfRow) {
			var strip = document.getElementById("healthStrip");
			strip.style.width = (widths[r] / maxW * 100) + "%";
			mirror.appendChild(strip);
			wallRowStrips.push(null);
		} else {
			//Remote rows get their own strip (#294), fed by the 5 s
			///cluster/status poll (leader) or the digest (follower) —
			//hidden until that row's board reports health.
			var rs = document.createElement("div");
			rs.className = "health rowhealth hidden";
			rs.style.width = (widths[r] / maxW * 100) + "%";
			for (var c = 0; c < widths[r]; c++) rs.appendChild(document.createElement("span"));
			mirror.appendChild(rs);
			wallRowStrips.push(rs);
		}
	}
	buildStrip(unitCount || 0);
	refreshLiveStatus();
}

//Row strips from cluster member health (#294): bit i of a member's hex
//faultMask = its unit at position col+i is faulty (amber, like the local
//strip's flagged state); an unjoined member's whole span goes red. Rows
//where nobody reported health keep their strip hidden — absence is
//"unknown", never "all good". Coincident mirror members merge worst-wins.
function updateWallHealth(members) {
	if (!wallWidths || wallRowStrips.length === 0) return;
	var byRow = {};
	(members || []).forEach(function(m) {
		//No self-skip here: on a FOLLOWER pane the digest's self member is
		//the LEADER's row, which needs its strip. The local pane's own row
		//is skipped naturally — its wallRowStrips slot is null (the
		//physical healthStrip owns it).
		(byRow[m.row] = byRow[m.row] || []).push(m);
	});
	for (var r = 0; r < wallRowStrips.length; r++) {
		var strip = wallRowStrips[r];
		if (!strip) continue;
		var rowMembers = (byRow[r] || []).filter(function(m) {
			return typeof m.faultMask === "string" || !m.joined;
		});
		strip.classList.toggle("hidden", rowMembers.length === 0);
		if (rowMembers.length === 0) continue;
		var cls = [], titles = [];
		rowMembers.forEach(function(m) {
			var mask = parseInt(m.faultMask || "0", 16) || 0;
			for (var i = 0; i < m.width; i++) {
				var cell = m.col + i;
				if (cell >= strip.children.length) break;
				if (!m.joined) {
					cls[cell] = "bad";
					titles[cell] = "board unreachable";
				} else if ((mask >>> i) & 1) {
					if (cls[cell] !== "bad") cls[cell] = "warn";
					titles[cell] = "unit flagged faulty";
				} else if (cls[cell] === undefined) {
					cls[cell] = "";
				}
				if (m.joined && m.wear && !titles[cell]) titles[cell] = "wear flagged on this row";
			}
		});
		for (var c = 0; c < strip.children.length; c++) {
			strip.children[c].className = cls[c] || "";
			strip.children[c].title = titles[c] || "";
		}
	}
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

//Riffle (#251): a tile travels THROUGH the drum order to its target the
//way the hardware does (forward-only, wrap-around) instead of folding
//once. Intermediate steps swap glyphs behind a restarted (partial) fold —
//rapid flutter — and the final step gets the full #246 two-leaf fold.
//Step rate follows the configured flap speed; travel is time-capped so a
//worst-case 44-flap run stays snappy. Honors prefers-reduced-motion.
var RIFFLE_MAX_TRAVEL_MS = 2500;

function riffleStepMs() {
	var speed = (window.lastSettings && Number(window.lastSettings.flapSpeed)) || 80;
	return Math.max(30, 95 - Math.round(speed / 2));  // speed 80 -> 55 ms
}

function riffleTileTo(tile, wireChar, staggerIndex) {
	var glyph = wireToGlyph(wireChar);
	if (glyph === " ") glyph = "\u00a0";
	if (tile._glyph === glyph) return;
	tile._glyph = glyph;
	//Two renders <1 s apart must not interleave: kill this tile's pending
	//flip/riffle before scheduling the new one.
	(tile._timers || []).forEach(clearTimeout);
	if (tile._riffle) { clearInterval(tile._riffle); tile._riffle = null; }
	var topG = tile.firstChild.firstChild, ch = tile.lastChild;

	var n = CALIBRATION_LETTERS.length;
	var from = CALIBRATION_LETTERS.indexOf(tile._wire || " ");
	var to = CALIBRATION_LETTERS.indexOf(wireChar);
	tile._wire = wireChar;
	var dist = (from >= 0 && to >= 0) ? (to - from + n) % n : 0;
	var reduced = window.matchMedia &&
		window.matchMedia("(prefers-reduced-motion: reduce)").matches;

	function finalFold() {
		//The .top leaf folds carrying the OLD glyph; the full-height .char
		//swaps mid-fold, and the leaf takes the new glyph as it snaps back
		//(animation end, 240 ms). #246.
		tile.classList.remove("flip");
		void tile.offsetWidth;
		tile.classList.add("flip");
		tile._timers.push(setTimeout(function() { ch.textContent = glyph; }, 110));
		tile._timers.push(setTimeout(function() { topG.textContent = glyph; }, 240));
	}

	tile._timers = [setTimeout(function() {
		if (dist <= 1 || reduced) { finalFold(); return; }
		var stepMs = Math.max(25, Math.min(riffleStepMs(),
			Math.floor(RIFFLE_MAX_TRAVEL_MS / dist)));
		var at = from;
		tile._riffle = setInterval(function() {
			at = (at + 1) % n;
			if (at === to) {
				clearInterval(tile._riffle);
				tile._riffle = null;
				finalFold();
				return;
			}
			var g = wireToGlyph(CALIBRATION_LETTERS[at]);
			if (g === " ") g = "\u00a0";
			//Restarted fold = one partial flap per step: drum flutter.
			tile.classList.remove("flip");
			void tile.offsetWidth;
			tile.classList.add("flip");
			ch.textContent = g;
			topG.textContent = g;
		}, stepMs);
	}, staggerIndex * 45)];
}

function renderMirror(text) {
	if (mirrorTiles.length === 0) return;
	if (wallWidths) {
		//Follower digest wall (#294): the board's own SSE text overlays just
		//the own row — the leader's digest rows own the rest. Riffle's
		//per-tile glyph dedup absorbs the digest re-paint of the same text.
		if (wallSource !== "digest" || wallSelfRow < 0) return;
		var row = mirrorRowTiles[wallSelfRow];
		if (!row) return;
		var overlay = padForMirror(text, row.length, currentAlignment);
		for (var i = 0; i < row.length; i++) riffleTileTo(row[i], overlay[i], i);
		return;
	}
	var frame = padForMirror(text, mirrorTiles.length, currentAlignment);
	if (frame === mirrorShown) return;
	mirrorShown = frame;
	mirrorTiles.forEach(function(tile, i) {
		riffleTileTo(tile, frame[i], i);
	});
}

//Wall rows arrive pre-positioned (padded, sliced server-side) — render
//verbatim, no alignment math. Stagger restarts per row so the rows
//animate in parallel like the physical wall.
function renderWall(rows) {
	if (!wallWidths) return;
	var frame = rows.join("\n");
	if (frame === mirrorShown) return;
	if (wallSource !== "digest") mirrorShown = frame;
	for (var r = 0; r < mirrorRowTiles.length; r++) {
		var text = String(rows[r] || "").toUpperCase();
		for (var i = 0; i < mirrorRowTiles[r].length; i++) {
			riffleTileTo(mirrorRowTiles[r][i], i < text.length ? text[i] : " ", i);
		}
	}
}

//Follower pane of glass (#294): mirror the leader's ping-piggybacked
//digest — wall rows, per-row health, member pills — from THIS board's page.
//Rides the 5 s /settings poll; riffle's per-tile dedup absorbs repaints.
function wallWidthsFromMembers(members) {
	//Digest content is only as trustworthy as the LAN — clamp the geometry
	//(8 rows / 255 units mirror the firmware's own table limits) so a
	//hostile value can never build a runaway wall and hang this tab.
	var widths = [];
	members.forEach(function(m) {
		var row = Number(m.row), extent = Number(m.col) + Number(m.width);
		if (!(row >= 0 && row < 8) || !(extent >= 1 && extent <= 255)) return;
		if (!(widths[row] >= extent)) widths[row] = extent;
	});
	for (var r = 0; r < widths.length; r++) widths[r] = widths[r] || 0;
	return widths;
}

function pollClusterDigest(s) {
	fetch("/cluster/digest", { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(function(d) {
			var digest = d.digest || {};
			var st = digest.status || {};
			var members = st.members || [];
			if (members.length === 0 || !digest.rows) return;
			window.lastDigestLeaderHost = (digest.leader || {}).host || s.clusterLeaderHost || "";
			var widths = wallWidthsFromMembers(members);
			if (widths.length === 0) return;
			var selfRow = Number(s.clusterRow);
			if (!wallWidths || wallSource !== "digest" ||
				wallWidths.join() !== widths.join() || wallSelfRow !== selfRow) {
				buildWall(widths, selfRow);
				wallSource = "digest";
				refreshLiveStatus();
			}
			renderWall(digest.rows);
			updateWallHealth(members);
			renderFollowerPills(members);
			//Stale digest (spec): a silent leader freezes this mirror — grey
			//it and say how old the picture is instead of looking live.
			var stale = Number(d.ageMs) > 30000;
			document.getElementById("mirror").classList.toggle("stale", stale);
			if (stale) {
				setBoardStatus("● CLUSTER · last seen " +
					Math.round(Number(d.ageMs) / 1000) + "s ago", true);
			}
		})
		.catch(function() {});
}

function setBoardStatus(text, offline) {
	var el = document.getElementById("boardStatus");
	el.textContent = text;
	el.style.color = offline ? "var(--warn)" : "";
}

function refreshLiveStatus() {
	setBoardStatus("● LIVE · " + (wallWidths ? "CLUSTER · " : "") +
		(currentMode === "clock" ? "CLOCK" : "TEXT"), false);
}

// ===================== settings poll =====================

function applySettings(s) {
	unitCount = s.unitCount || 0;
	currentAlignment = s.alignment || "left";
	currentMode = s.deviceMode || "text";

	//Collapse fallback (#277): if the SSE stream died and missed the
	//uncluster transition, the poll is the authority — tear the wall down.
	//Each wall source has its own collapse rule (#294): the SSE wall dies
	//with leadership, the digest wall with the follower membership.
	var followerClustered = !!s.clusterState && s.clusterState !== "standalone";
	if (wallWidths && wallSource !== "digest" && !s.clusterLeading) buildMirror(unitCount);
	if (wallWidths && wallSource === "digest" && !followerClustered) buildMirror(unitCount);
	//Follower pane of glass (#294): while clustered, mirror the leader's
	//digest — the whole wall on THIS board's page, 5 s cadence.
	if (followerClustered && !s.clusterLeading) pollClusterDigest(s);
	if (!wallWidths) {
		if (mirrorTiles.length !== unitCount) buildMirror(unitCount);
		renderMirror(s.lastWrittenText || "");
	} else if (document.getElementById("healthStrip").children.length !== unitCount) {
		//SSE built the wall before the first poll delivered unitCount.
		buildStrip(unitCount);
	}

	//#289 dummy mode: reflect the stored override (never while the user is
	//editing the field).
	var overrideInput = document.getElementById("inputUnitCountOverride");
	var overrideValue = s.unitCountOverride || 0;
	if (overrideInput && document.activeElement !== overrideInput) {
		overrideInput.value = overrideValue;
	}
	var overridePill = document.getElementById("labelWidthOverride");
	if (overridePill) {
		overridePill.textContent = overrideValue > 0 ? "pinned: " + overrideValue : "auto";
		overridePill.className = "pill " + (overrideValue > 0 ? "ok" : "off");
	}

	//#330 headless mode: reflect the stored deviceRole and the unit-less
	//suggestion. Detection only nudges (banner) — the user picks the role.
	var role = s.deviceRole || "display";
	setSegValue("segDeviceRole", role);
	var rolePill = document.getElementById("labelDeviceRole");
	if (rolePill) {
		rolePill.textContent = role === "display" ? "display" : role.replace("headless-", "");
		rolePill.className = "pill " + (role === "display" ? "off" : "ok");
	}
	var roleBanner = document.getElementById("deviceRoleSuggestion");
	if (roleBanner) roleBanner.classList.toggle("hidden", !s.headlessSuggested);

	document.getElementById("boardName").textContent = (s.effectiveDeviceName || "split-flap").toUpperCase();
	refreshLiveStatus();
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
		setTimezone(s.timezonePosix || "");
		setDeviceName(s.deviceName || "", s.effectiveDeviceName || "");
		setMqttFields(s.mqttHost || "", s.mqttPort || "", s.mqttUser || "", s.mqttPasswordSet === true);
		document.getElementById("labelVersion").textContent = s.version || "—";
		if (!s.wifiSettingsResettable) document.getElementById("cardWifi").classList.add("hidden");
		setCalibrationUnitsFromSettings(s);
	}
	setMqttPill(s.mqttHost || "", s.mqttConnected === true);
	updateClusterBanner(s);
	updateClusterFollowerCard(s);
	window.lastSettings = s;  // System tab reuses version etc. (#245)
}

//Cluster membership (#272): while this board renders a row of a cluster
//wall, the leader owns text/mode/clock — show a persistent banner with a
//link to the leader and disable the content controls (the backend answers
//409 regardless; maintenance stays live). Everything comes off /settings,
//so the state survives reboots and poll-recovers after leader changes.
function updateClusterBanner(s) {
	var el = document.getElementById("clusterBanner");
	if (!el) return;
	var clustered = !!s.clusterState && s.clusterState !== "standalone";
	//#317: this board is the LEADER (not itself a follower row) — show a
	//distinct "leading a wall" banner and relabel the command controls.
	var leading = !clustered && !!s.clusterLeading;
	el.classList.toggle("hidden", !clustered && !leading);
	if (clustered) {
		//leaderName/leaderHost come off an unauthenticated LAN POST — build
		//the banner with DOM nodes, never markup strings.
		var leader = s.clusterLeaderName || s.clusterLeaderHost || "leader";
		var text = "Clustered — row " + (Number(s.clusterRow) + 1) + " of " + leader;
		if (s.clusterState === "local-fallback") text += " (leader unreachable — showing local clock)";
		else if (s.clusterState === "grace") text += " (waiting for leader)";
		el.textContent = text;
		//Strict hostname[:port] allowlist — anything else gets no link at all.
		var host = String(s.clusterLeaderHost || "");
		if (/^[A-Za-z0-9.\-]+(:\d+)?$/.test(host)) {
			el.appendChild(document.createTextNode(" · "));
			var link = document.createElement("a");
			link.href = "http://" + host + "/";
			link.textContent = "open leader";
			el.appendChild(link);
		}
		//#295: a follower that has written the leader off can take over —
		//it holds the member table from the ping digest.
		if (s.clusterState === "local-fallback") {
			el.appendChild(document.createTextNode(" · "));
			var promote = document.createElement("button");
			promote.type = "button";
			promote.className = "btn";
			promote.id = "buttonClusterPromote";
			promote.textContent = "Promote this board to leader…";
			promote.addEventListener("click", promoteCluster);
			el.appendChild(promote);
		}
	}
	if (leading) buildLeadingBanner(el);
	["inputText", "buttonSend", "selectDuration"].forEach(function(id) {
		var control = document.getElementById(id);
		if (control) control.disabled = clustered;
	});
	document.querySelectorAll("#segMode button").forEach(function(b) {
		b.disabled = clustered;
	});
	applyWallLabels(leading);
	updateMessageInputs(leading);
}

//#317: the leader banner — driven off the /settings poll for clusterLeading,
//with member count + per-member auth taken from the cached /cluster/status.
//Built with DOM nodes (member hosts/names come off unauthenticated LAN wire).
function buildLeadingBanner(el) {
	var st = window.lastClusterStatus || {};
	var members = st.members || [];
	var rows = 0;
	members.forEach(function(m) { rows = Math.max(rows, m.row + 1); });
	el.textContent = "⚑ Leading — driving a " + rows + "-row wall · " +
		members.length + " member" + (members.length === 1 ? "" : "s");
	//Auth summary over the FOLLOWER links only (the leader's own row is not a
	//wire link). "all" = leader signs to every follower.
	var followers = members.filter(function(m) { return !m.self; });
	if (followers.length) {
		var authed = followers.filter(function(m) { return m.hmac; }).length;
		el.appendChild(document.createTextNode(" · "));
		var chip = document.createElement("span");
		if (authed === followers.length) { chip.className = "pill ok"; chip.textContent = "Auth · all links"; }
		else if (authed > 0) { chip.className = "pill bad"; chip.textContent = "Auth · " + authed + "/" + followers.length; }
		else { chip.className = "pill off"; chip.textContent = "Unauthenticated"; }
		el.appendChild(chip);
	}
	el.appendChild(document.createTextNode(" · text, mode and stop apply to the whole wall."));
}

//#317: relabel the command buttons to signal wall-wide reach while leading.
function applyWallLabels(leading) {
	var send = document.getElementById("buttonSend");
	if (send) send.textContent = leading ? "Send to wall" : "Send";
	var stop = document.getElementById("buttonStop");
	if (stop) stop.textContent = leading ? "Stop & blank the wall" : "Stop & blank display";
}

//#318: per-row wall widths from the cached /cluster/status — a row's capacity
//is the max of (col+width) over its members. Returns [w0,w1,…] or null.
function clusterRowWidths() {
	var st = window.lastClusterStatus;
	if (!st || !st.members || !st.members.length) return null;
	var widths = {};
	st.members.forEach(function(m) {
		var w = (m.col || 0) + (m.width || 0);
		if (!(m.row in widths) || w > widths[m.row]) widths[m.row] = w;
	});
	var maxRow = Math.max.apply(null, Object.keys(widths).map(Number));
	var arr = [];
	for (var r = 0; r <= maxRow; r++) arr.push(widths[r] || 0);
	return arr;
}

//#318: one input per wall row (maxlength = that row's width) while leading a
//multi-row wall — replaces the single box + literal-\n marker. The shape
//string gates rebuilds so typed text survives the /cluster/status poll.
var perRowShape = null;
function updateMessageInputs(leading) {
	var perRow = document.getElementById("perRowInputs");
	if (!perRow) return;
	var wrap = document.getElementById("singleInputWrap");
	var meta = document.getElementById("messageMetaRow");
	var widths = leading ? clusterRowWidths() : null;
	var usePerRow = !!(widths && widths.length > 1);
	if (wrap) wrap.classList.toggle("hidden", usePerRow);
	if (meta) meta.classList.toggle("hidden", usePerRow);
	perRow.classList.toggle("hidden", !usePerRow);
	if (!usePerRow) { perRowShape = null; return; }
	var shape = widths.join(",");
	if (shape === perRowShape) return;
	perRowShape = shape;
	removeAllChildren(perRow);
	widths.forEach(function(w, r) {
		var row = document.createElement("div");
		row.className = "row";
		var cell = document.createElement("div");
		cell.className = "grow";
		var lbl = document.createElement("label");
		lbl.className = "small";
		lbl.textContent = "Row " + r + " · " + w + " wide";
		var input = document.createElement("input");
		input.type = "text";
		input.className = "perrow-input";
		input.maxLength = w;
		input.autocomplete = "off";
		input.placeholder = "up to " + w + " characters";
		cell.appendChild(lbl);
		cell.appendChild(input);
		row.appendChild(cell);
		perRow.appendChild(row);
	});
}

//#318: compose the wall text from the per-row inputs — joined with real
//newlines, which the grid composer treats as row breaks (#290).
function perRowCompose() {
	var inputs = document.querySelectorAll("#perRowInputs .perrow-input");
	return Array.prototype.map.call(inputs, function(i) {
		return normalizeUmlauts(i.value);
	}).join("\n");
}

//#295 one-click takeover: the firmware validates (local-fallback + held
//digest) and stages the transformed member table; the old leader demotes
//itself via the sticky-leadership join 409 when it returns.
function promoteCluster() {
	if (!confirm("Take over as the wall\u2019s leader?\n\nThis board starts driving every member; the old leader joins as a plain member when it comes back.")) return;
	var button = document.getElementById("buttonClusterPromote");
	if (button) button.disabled = true;
	fetch("/cluster/promote", { method: "POST" })
		.then(function(r) {
			return r.json().then(function(j) { return { ok: r.ok, message: j.message }; });
		})
		.then(function(res) {
			alert(res.message || (res.ok ? "Promoted." : "Promote failed."));
			if (res.ok) location.reload();
			else if (button) button.disabled = false;
		})
		.catch(function() {
			alert("Promote failed \u2014 board unreachable.");
			if (button) button.disabled = false;
		});
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
	initClusterCard();
	initDisplayEvents();
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

//SSE display push (#251): the mirror flips the moment displayTask executes
//a command instead of waiting on the 5 s poll. The poll backstops the
//single-row mirror's CONTENT and the wall's collapse (clusterLeading),
//but not wall content — a dead stream freezes the remote rows until
//EventSource auto-reconnects and onConnect resends the full wall (#277).
function initDisplayEvents() {
	if (!window.EventSource) return;
	var es = new EventSource("/events");
	es.addEventListener("display", function(event) {
		try {
			var d = JSON.parse(event.data);
			if (d.rows && d.rows.length) {
				var widths = d.rows.map(function(row) { return String(row).length; });
				var selfRow = d.selfRow || 0;
				if (!wallWidths || wallSource !== "sse" ||
					wallWidths.join() !== widths.join() || wallSelfRow !== selfRow) {
					buildWall(widths, selfRow);
					wallSource = "sse";
				}
				renderWall(d.rows);
			} else {
				//Rows only ride the stream while this board LEADS — a
				//follower's own-text events must not collapse its digest
				//wall (#294); renderMirror overlays just the own row there.
				if (wallWidths && wallSource === "sse") {
					//Cluster disabled — collapse to the single-row mirror.
					buildMirror(unitCount || 0);
					refreshLiveStatus();
				}
				renderMirror(d.text || "");
			}
		} catch (e) { /* malformed event — the poll catches up */ }
	});
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
function postSettingsFields(fields, callback, base) {
	var body = new URLSearchParams();
	Object.keys(fields).forEach(function(key) { body.append(key, fields[key]); });
	body.append("ajax", "1");
	fetch((base || "") + "/", { method: "POST", body: body })
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
	//#318: per-row inputs (leading a multi-row wall) compose the wall text;
	//otherwise the single box.
	var perRow = document.getElementById("perRowInputs");
	var text = (perRow && !perRow.classList.contains("hidden"))
		? perRowCompose()
		: normalizeUmlauts(document.getElementById("inputText").value);
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

//#289 dummy mode: pin the display width (0 = auto/probe-derived).
function saveUnitCountOverride() {
	var value = parseInt(document.getElementById("inputUnitCountOverride").value, 10);
	if (isNaN(value) || value < 0 || value > 16) {
		showStatus("unitCountOverrideStatus", "Enter 0 (auto) or 1-16.", "error", 4000);
		return;
	}
	postSettingsFields({ unitCount: value }, function(ok) {
		showStatus("unitCountOverrideStatus",
			ok ? (value > 0 ? "Width pinned to " + value + "." : "Back to auto (probe-derived).")
			   : "Save failed.",
			ok ? "success" : "error", 4000);
	});
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
	//#330 headless mode: deviceRole selector — posts only its own field, the
	//poll loop reflects the device's answer back like the other segments.
	document.querySelectorAll("#segDeviceRole button").forEach(function(b) {
		b.addEventListener("click", function() {
			setSegValue("segDeviceRole", b.dataset.value);
			postSettingsFields({ deviceRole: b.dataset.value }, function(ok) {
				showStatus("deviceRoleStatus", ok ? "✔ Role saved." : "✘ Role save failed.", ok ? "success" : "error", 4000);
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

//Full IANA timezone list (#252): the dropdown is populated lazily from
///tz.json — baked at build time from the vendored posix_tz_db zones.csv,
//~460 zones. Option value = POSIX string (wire format), text = IANA name;
//"UTC" maps to "" (the stored default). Same control as the rest of the
//UI — the old 14-entry curated list was an ESP-01 flash-budget fossil.
var tzListLoaded = false;
//Saving the device card before the list has loaded must not post the
//transient empty select (= silently switch the device to UTC) — until
//then the timezone key is omitted and the server's provided-field gating
//leaves the stored value alone.
var tzFieldReady = false;

function loadTimezoneOptions(done) {
	if (tzListLoaded) { done(); return; }
	fetch("/tz.json")
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(function(table) {
			var select = document.getElementById("selectTimezone");
			Object.keys(table).forEach(function(name) {
				var opt = document.createElement("option");
				opt.value = table[name];
				opt.textContent = name;
				select.appendChild(opt);
			});
			tzListLoaded = true;
			done();
		})
		.catch(function() { done(); });  //list stays empty — custom-only mode
}

//Select the stored POSIX string. Many zones share a POSIX rule, so prefer
//the option whose NAME the user actually picked (remembered locally);
//a stored value not in the list (custom / fetch failed) surfaces as a
//"Custom" option so it isn't lost on save — v1 behavior.
function setTimezone(tzPosix) {
	loadTimezoneOptions(function() {
		var select = document.getElementById("selectTimezone");
		tzFieldReady = true;
		var remembered = localStorage.getItem("sf-tz-name");
		var fallback = -1;
		for (var i = 0; i < select.options.length; i++) {
			if (select.options[i].value !== tzPosix) continue;
			if (select.options[i].textContent === remembered) {
				select.selectedIndex = i;
				return;
			}
			if (fallback < 0) fallback = i;
		}
		if (fallback >= 0) { select.selectedIndex = fallback; return; }
		var custom = document.createElement("option");
		custom.value = tzPosix;
		custom.textContent = "Custom: " + tzPosix;
		select.insertBefore(custom, select.firstChild);
		select.selectedIndex = 0;
	});
}

//The selected option's POSIX value; remembers the picked NAME so a shared
//POSIX rule round-trips to the same zone next load.
function timezoneFieldPosix() {
	var select = document.getElementById("selectTimezone");
	var opt = select.options[select.selectedIndex];
	if (!opt) return "";
	if (opt.textContent.indexOf("Custom: ") !== 0) {
		localStorage.setItem("sf-tz-name", opt.textContent);
	}
	return opt.value;
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
	var fields = { deviceName: document.getElementById("inputDeviceName").value };
	if (tzFieldReady) fields.timezone = timezoneFieldPosix();
	postSettingsFields(fields, function(ok, result) {
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

//Wear cell (#246): relative bar scaled to the display max + the count.
//Flagged units (server-side WearPolicy — data.wear) go red.
function appendWearCell(row, u, wearFlagged, maxOdo) {
	var td = document.createElement("td");
	if (typeof u.odo !== "number") {
		td.textContent = "—";
	} else {
		var bar = document.createElement("span");
		bar.className = "wear-bar" + (wearFlagged ? " hot" : "");
		var fill = document.createElement("i");
		var pct = maxOdo > 0 ? Math.max(4, Math.round((u.odo / maxOdo) * 100)) : 4;
		fill.style.width = pct + "%";
		bar.appendChild(fill);
		td.appendChild(bar);
		td.appendChild(document.createTextNode(
			formatOdometer(u.odo) + (wearFlagged ? " ⚠" : "")));
		if (wearFlagged) td.className = "uh-bad";
	}
	row.appendChild(td);
}

//Drift cell (#263/#264): since-boot drift events + the hall-corrected
//physical-letter check. A mismatch means the drum PHYSICALLY shows a
//different letter than the master intended; the unit self-corrects at its
//next idle window (the pending marker).
function appendDriftCell(row, u) {
	var td = document.createElement("td");
	if (typeof u.de !== "number") {
		td.textContent = "\u2014";
	} else {
		var text = "" + u.de;
		if (u.dp) text += " \u21ba";
		if (u.mm) {
			var shows = (typeof u.phys === "number" && CALIBRATION_LETTERS[u.phys]) || "?";
			text += " \u2717";
			td.title = "drum shows '" + shows + "', not the intended letter" +
				(u.ds ? " \u00b7 last drift " + u.ds + " steps" : "");
			td.className = "uh-bad";
		} else if (u.de > 0) {
			td.className = "uh-warn";
			if (u.ds) td.title = "last drift " + u.ds + " steps";
		}
		td.textContent = text;
	}
	row.appendChild(td);
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
	//Mid-reflash a unit in bootloader mode is expected, not broken (#249):
	//yellow instead of red, and the one being written pulses.
	var rf = (data && data.reflash) || null;
	var rfActive = rf !== null && reflashIsRunning(rf);
	var strip = document.getElementById("healthStrip");
	var silent = [];
	if (strip.children.length === width) {
		for (var i = 0; i < width; i++) {
			var u = units[i];
			var cls = "";
			if (rfActive && u && u.st === 2) {
				cls = (rf.state === "flashing" && rf.cur === u.a) ? "flashing cur" : "flashing";
			} else if (!u || u.st !== 1) { cls = "bad"; silent.push(i + 1); }
			else if (unitRowIsFaulty(u) || u.fw === 1 || u.mm) cls = "warn";
			strip.children[i].className = cls;
			strip.children[i].title = (u && u.mm) ? "off-letter \u2014 drum disagrees with the intended frame" : "";
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
	var maxOdo = 0;
	for (var m = 0; m < units.length; m++) {
		if (typeof units[m].odo === "number" && units[m].odo > maxOdo) maxOdo = units[m].odo;
	}

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
		appendWearCell(row, unit, flagged, maxOdo);
		appendDriftCell(row, unit);
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
	var offLetter = [];
	for (var mmIdx = 0; mmIdx < units.length; mmIdx++) {
		if (units[mmIdx].mm) offLetter.push(units[mmIdx].i + 1);
	}
	if (offLetter.length > 0) summary += " \u00b7 unit " + offLetter.join(", ") + " off-letter";
	setUnitHealthSummary(summary,
		faulty > 0 || responding < width || wear.flagged.length > 0 || offLetter.length > 0 ? "bad" : "ok");

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
		appendButton(row, "Self-test", function() { selfTestUnitUi(u.a); });
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

//On-demand unit self-test (#265): ~15 s diagnostic revolution measuring
//actual steps/rev (nominal 2038), hall window width and revolution time.
//Queue-native like every op, but the outcome rides its own endpoint
//because it carries the measurements. Controls lock while it runs (the
//single result slot serves one self-test at a time).
function selfTestUnitUi(address) {
	showProvisioningStatus("Self-test on unit " + formatHexAddress(address) +
		" \u2014 about 15 s of motion\u2026", "");
	setMaintenanceBusy(true);
	fetch("/unit/self-test?address=" + address, { method: "POST" })
		.then(function(r) {
			if (!r.ok) return r.text().then(function(t) { throw new Error(t || ("HTTP " + r.status)); });
			return r.json();
		})
		.then(function(data) { pollSelfTestResult(address, data.seq, 100); })
		.catch(function(e) {
			setMaintenanceBusy(false);
			showProvisioningStatus("Self-test failed to queue: " +
				(e && e.message ? e.message : "request failed"), "error");
		});
}

function pollSelfTestResult(address, seq, remaining) {
	fetch("/unit/self-test-result?seq=" + seq, { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(function(res) {
			if (res.state === "pending" && remaining > 0) {
				setTimeout(function() { pollSelfTestResult(address, seq, remaining - 1); }, 1000);
				return;
			}
			setMaintenanceBusy(false);
			if (res.state === "ok") {
				var delta = res.steps_per_rev - 2038;
				showProvisioningStatus("Unit " + formatHexAddress(address) + " self-test: " +
					res.steps_per_rev + " steps/rev (" + (delta >= 0 ? "+" : "") + delta +
					" vs nominal), hall window " + res.hall_window + " steps, " +
					(res.rev_time_ms / 1000).toFixed(1) + " s/rev.", "success");
			} else if (res.state === "pending") {
				showProvisioningStatus("Self-test still queued \u2014 display busy; check again in a moment.", "");
			} else if (res.state === "expired") {
				showProvisioningStatus("Self-test outcome superseded \u2014 run it again.", "error");
			} else {
				showProvisioningStatus("Self-test failed: " + (res.reason || "unknown") + ".", "error");
			}
		})
		.catch(function() {
			if (remaining > 0) {
				setTimeout(function() { pollSelfTestResult(address, seq, remaining - 1); }, 1000);
				return;
			}
			setMaintenanceBusy(false);
			showProvisioningStatus("Self-test result poll failed.", "error");
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
			if (trackReflashProgress(rf)) {
				//Same payload carries the mid-job unit facts — keep the
				//board strip live (yellow) while the job runs (#249).
				renderUnitHealth(json);
				return;
			}
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
	setStat("statMaxAlloc", "largest contiguous " + formatBytes(now.maxAlloc || 0));
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

	//Cluster vitals fan out to each row's /settings — slower than the local
	//2 s stats poll so a wall of boards isn't hammered (#318 D).
	var clusterVitalsHandle = null;

	function syncPolling() {
		var want = onSystemTab && !document.hidden;
		if (want && pollHandle === null) {
			fetchStats();
			pollHandle = setInterval(fetchStats, 2000);
			refreshSysClusterVitals();
			clusterVitalsHandle = setInterval(refreshSysClusterVitals, 8000);
		} else if (!want && pollHandle !== null) {
			clearInterval(pollHandle);
			pollHandle = null;
			clearInterval(clusterVitalsHandle);
			clusterVitalsHandle = null;
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
function postCalibrationAwait(path, params, callback, base) {
	var query = Object.keys(params).map(function(k) {
		return encodeURIComponent(k) + "=" + encodeURIComponent(params[k]);
	}).join("&");
	setMaintenanceBusy(true);
	fetch((base || "") + path + (query ? "?" + query : ""), { method: "POST" })
		.then(function(r) {
			if (!r.ok) return r.text().then(function(t) { throw new Error(t || ("HTTP " + r.status)); });
			return r.json();
		})
		.then(function(data) { pollOpResult(data.seq, 30, callback, base); })
		.catch(function(e) {
			setMaintenanceBusy(false);
			callback(false, e && e.message ? e.message : "request failed");
		});
}

//Address ops settle ~3 s + reprobe before their result lands; 30 × 500 ms
//also survives a queued show frame ahead of the op.
function pollOpResult(seq, remaining, callback, base) {
	fetch((base || "") + "/unit/op-result?seq=" + seq, { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(function(res) {
			if (res.state === "pending" && remaining > 0) {
				setTimeout(function() { pollOpResult(seq, remaining - 1, callback, base); }, 500);
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
				setTimeout(function() { pollOpResult(seq, remaining - 1, callback, base); }, 500);
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


// ===================== cluster card (#274) =====================

//Leader-side member editor + discovery browse. clusterMembers is the
//editor's source of truth: mirrored once from /cluster/status, then only
//user edits touch it — the 5 s status poll refreshes state/rev cells only
//(and only while the editor still mirrors the saved table), so it can
//never stomp a row mid-edit.
var clusterMembers = null;
var clusterStatusTimer = null;
var clusterRolloutSeen = false;

function initClusterCard() {
	var tabActive = false;
	document.addEventListener("sf-tabchange", function(event) {
		//Settings needs the editor pills/rollout; Maintenance (#318 C) and
		//System (#318 D) need the live member list to render their cards.
		tabActive = event.detail === "settings" || event.detail === "maintenance" ||
			event.detail === "system";
		if (tabActive) loadClusterStatus();
	});
	//One steady 5 s timer: the settings tab needs pills/rollout, and the
	//home tab's wall needs per-row health (#294) while this board leads.
	clusterStatusTimer = setInterval(function() {
		if (document.hidden) return;
		if (tabActive || (wallWidths && wallSource === "sse")) loadClusterStatus();
	}, 5000);
}

function setClusterPill(text, kind) {
	var pill = document.getElementById("labelClusterStatus");
	pill.className = "pill " + kind;
	pill.textContent = text;
}

//Follower collapse: driven off the same /settings poll as the banner —
//while this board is someone's row the leader editor makes no sense here.
function updateClusterFollowerCard(s) {
	var followerView = document.getElementById("clusterFollowerView");
	if (!followerView) return;
	var clustered = !!s.clusterState && s.clusterState !== "standalone";
	followerView.classList.toggle("hidden", !clustered);
	document.getElementById("clusterLeaderView").classList.toggle("hidden", clustered);
	if (clustered) {
		//leaderName/leaderHost come off an unauthenticated LAN POST — text
		//nodes only (same rule as the banner).
		var leader = s.clusterLeaderName || s.clusterLeaderHost || "the leader";
		document.getElementById("clusterFollowerLine").textContent =
			"This board renders row " + (Number(s.clusterRow) + 1) + " of " + leader +
			" — text, mode and clock come from the leader; maintenance stays local.";
		//Same health the banner reports: grace/fallback must not read green.
		if (s.clusterState === "grace") setClusterPill("waiting for leader", "off");
		else if (s.clusterState === "local-fallback") setClusterPill("leader lost", "bad");
		else setClusterPill("clustered", "ok");
	}
}

function loadClusterStatus() {
	fetch("/cluster/status", { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(updateClusterFromStatus)
		.catch(function() {});
}

//follower-<rev>[-dirty].bin → "&v=<rev>" (parity with ota-flash.sh; the
//follower records it as intendedVersion).
function followerFwVersionParam(fileName) {
	var m = fileName.match(/^follower-([0-9a-f]{7,40}(?:-dirty)?)/i);
	return m ? "&v=" + encodeURIComponent(m[1]) : "";
}

//Upload a follower-*.bin to THIS board's storage (#304 Part B), same-origin
//(no CORS). Client-side MD5 (SparkMD5) + the follower- prefix guard mirror the
//master upload / ota-flash.sh #299. The S3 later streams it to esp01 rows.
function uploadFollowerFirmware() {
	var input = document.getElementById("inputClusterFollowerFw");
	var file = input.files[0];
	if (!file) { showStatus("clusterCardStatus", "✘ Pick a follower-*.bin first.", "error", 5000); return; }
	if (!/^follower-/i.test(file.name)) {
		showStatus("clusterCardStatus", "✘ " + escapeHtml(file.name) + " is not a follower-*.bin (an S3 image would brick an ESP-01).", "error", 8000);
		return;
	}
	var btn = document.getElementById("buttonClusterFollowerFwUpload");
	btn.disabled = true;
	input.disabled = true;
	showStatus("clusterCardStatus", "Computing MD5…", "pending");
	var reader = new FileReader();
	reader.onerror = function() {
		btn.disabled = false;
		input.disabled = false;
		showStatus("clusterCardStatus", "✘ Could not read the file.", "error", 5000);
	};
	reader.onload = function() {
		var md5 = SparkMD5.ArrayBuffer.hash(reader.result);
		showStatus("clusterCardStatus", "Uploading follower image (" + Math.round(file.size / 1024) + " KB)…", "pending");
		var formData = new FormData();
		formData.append("firmware", file);
		var xhr = new XMLHttpRequest();
		xhr.open("POST", "/cluster/follower-firmware?md5=" + md5 + followerFwVersionParam(file.name));
		xhr.onreadystatechange = function() {
			if (xhr.readyState !== 4) return;
			btn.disabled = false;
			input.disabled = false;
			if (xhr.status === 200) {
				showStatus("clusterCardStatus", "✔ " + escapeHtml(xhr.responseText), "success", 8000);
				loadClusterStatus();
			} else if (xhr.status === 0) {
				showStatus("clusterCardStatus", "✘ Upload failed — lost connection.", "error", 6000);
			} else {
				showStatus("clusterCardStatus", "✘ HTTP " + xhr.status + ": " + escapeHtml(xhr.responseText), "error", 8000);
			}
		};
		xhr.send(formData);
	};
	reader.readAsArrayBuffer(file);
}

function clusterStateLabel(m) {
	if (m.updating) return { text: "updating", kind: "off" };
	if (m.updateBlocked) return { text: "update blocked", kind: "bad" };
	if (m.self) return { text: "ok", kind: "ok" };
	if (m.degraded) return { text: "degraded", kind: "bad" };
	if (m.joined) return { text: "ok", kind: "ok" };
	return { text: "joining", kind: "off" };
}

function updateClusterFromStatus(st) {
	//Cache for the leading banner (#317): it runs off the /settings poll but
	//needs member count + per-member auth from here.
	window.lastClusterStatus = st;
	updateClusterBanner(window.lastSettings || {});
	//Maintenance-tab member list (#318 C): shown only while leading.
	renderMaintClusterMembers(st);
	//Wall row strips (#294) — the leader's own SSE wall colors its remote
	//rows from the same member health the pills use.
	if (wallSource === "sse") updateWallHealth(st.members || []);
	var followerVisible = !document.getElementById("clusterFollowerView").classList.contains("hidden");
	if (!followerVisible) {
		if (st.enabled) {
			var rows = 0;
			(st.members || []).forEach(function(m) { rows = Math.max(rows, m.row + 1); });
			setClusterPill("leading · " + rows + " row" + (rows === 1 ? "" : "s"), "ok");
		} else {
			setClusterPill("off", "off");
		}
	}

	if (clusterMembers === null) {
		clusterMembers = (st.members || []).map(function(m) {
			return { host: m.host, row: m.row, col: m.col, width: m.width };
		});
		renderClusterMembers();
	}

	//Live cells refresh only while the editor mirrors the saved table
	//(same hosts, same order) — rows with unsaved edits show a dash.
	var saved = st.members || [];
	var mirrors = clusterMembers.length === saved.length && clusterMembers.every(function(m, i) {
		return m.host === saved[i].host && m.row === saved[i].row &&
			m.col === saved[i].col && m.width === saved[i].width;
	});
	var body = document.getElementById("clusterMemberBody");
	Array.prototype.forEach.call(body.rows, function(tr, i) {
		var pill = tr.querySelector(".cl-state");
		var rev = tr.querySelector(".cl-rev");
		if (!mirrors || !saved[i]) {
			pill.className = "pill off cl-state";
			pill.textContent = "—";
			rev.textContent = "";
			return;
		}
		var label = clusterStateLabel(saved[i]);
		pill.className = "pill " + label.kind + " cl-state";
		pill.textContent = label.text;
		//plat tag (#297) + wire-auth chip (#317): only for non-self members
		//(the leader's own row is not a wire link). hmac = leader signs to it.
		rev.textContent = "";
		if (!saved[i].self) {
			rev.appendChild(document.createTextNode((saved[i].rev || "—") + (saved[i].plat ? " · " + saved[i].plat : "") + "  "));
			var authChip = document.createElement("span");
			authChip.className = "pill " + (saved[i].hmac ? "ok" : "off");
			authChip.style.fontSize = "10px";
			authChip.textContent = saved[i].hmac ? "auth" : "no-auth";
			rev.appendChild(authChip);
		}
	});

	//Fleet rollout (#276) surfacing: progress while it runs, one success
	//line when it finishes, a persistent warning if convergence is dead.
	var rollout = st.rollout || {};
	if (rollout.imageVerifyFailed) {
		showStatus("clusterCardStatus", "⚠ This board’s running image failed its verify pass — automatic follower updates are off until a reboot.", "error");
	} else if (rollout.phase === "uploading" && rollout.total > 0) {
		clusterRolloutSeen = true;
		showStatus("clusterCardStatus", "Updating " + escapeHtml(rollout.host) + " to this board’s firmware — " +
			Math.floor(rollout.sent * 100 / rollout.total) + "% of " + Math.round(rollout.total / 1024) + " KB…", "pending");
	} else if (rollout.phase === "waiting") {
		clusterRolloutSeen = true;
		showStatus("clusterCardStatus", "Flashed " + escapeHtml(rollout.host) + " — waiting for it to reboot and rejoin…", "pending");
	} else if (clusterRolloutSeen) {
		clusterRolloutSeen = false;
		showStatus("clusterCardStatus", "✔ Firmware update round finished.", "success", 8000);
	}

	//ESP-01 firmware store (#304 Part B): the stored image + live push. Push
	//progress is mutually exclusive with the rollout above, so it never fights
	//that status line.
	var fwStored = document.getElementById("clusterFollowerFwStored");
	if (fwStored) {
		var fi = st.followerImage || {};
		fwStored.textContent = fi.present
			? "Stored follower image: " + (fi.rev || "unknown rev") + " — push it from a row’s ⚙ panel."
			: "No follower image stored yet.";
	}
	var fp = st.followerPush || {};
	if (fp.phase === "uploading" && fp.total > 0) {
		showStatus("clusterCardStatus", "Pushing ESP-01 firmware to " + escapeHtml(fp.host) + " — " +
			Math.floor(fp.sent * 100 / fp.total) + "% of " + Math.round(fp.total / 1024) + " KB…", "pending");
	}
}

function renderClusterMembers() {
	var table = document.getElementById("clusterMemberTable");
	var body = document.getElementById("clusterMemberBody");
	while (body.firstChild) body.removeChild(body.firstChild);
	table.classList.toggle("hidden", !clusterMembers || clusterMembers.length === 0);
	if (!clusterMembers) return;

	clusterMembers.forEach(function(member, index) {
		var tr = document.createElement("tr");

		function numberCell(field, min, max) {
			var td = document.createElement("td");
			var input = document.createElement("input");
			input.type = "number";
			input.min = min;
			input.max = max;
			input.value = member[field];
			input.addEventListener("change", function() {
				var value = parseInt(input.value, 10);
				if (!isNaN(value)) member[field] = value;
			});
			td.appendChild(input);
			return td;
		}

		tr.appendChild(numberCell("row", 0, 7));

		//Hosts come off the mDNS wire / user input — text nodes only.
		var hostTd = document.createElement("td");
		hostTd.textContent = member.host === "" ? "(this board)" : member.host;
		tr.appendChild(hostTd);

		tr.appendChild(numberCell("col", 0, 254));
		//#333 warm-standby: width 0 = an off-grid backup member (no columns,
		//non-rendering, promote-eligible) — the leader accepts it.
		tr.appendChild(numberCell("width", 0, 255));

		var stateTd = document.createElement("td");
		var pill = document.createElement("span");
		pill.className = "pill off cl-state";
		pill.textContent = "—";
		stateTd.appendChild(pill);
		tr.appendChild(stateTd);

		var revTd = document.createElement("td");
		revTd.className = "cl-rev meta";
		tr.appendChild(revTd);

		var removeTd = document.createElement("td");
		var manageButton = document.createElement("button");
		manageButton.type = "button";
		manageButton.className = "btn";
		manageButton.textContent = "⚙";
		manageButton.title = "Health + maintenance for this board (#294)";
		manageButton.addEventListener("click", function() {
			openMemberPanel(member.host, member.host === "");
		});
		removeTd.appendChild(manageButton);
		var removeButton = document.createElement("button");
		removeButton.type = "button";
		removeButton.className = "btn";
		removeButton.textContent = "✕";
		removeButton.title = "Remove this member";
		removeButton.addEventListener("click", function() {
			clusterMembers.splice(index, 1);
			renderClusterMembers();
		});
		removeTd.appendChild(removeButton);
		tr.appendChild(removeTd);

		body.appendChild(tr);
	});
}

//Member pills on the follower card (#294): the digest carries the same
//status shape the leader's card reads, so any pane shows the whole wall's
//members — and opens the same management panel.
function renderFollowerPills(members) {
	var box = document.getElementById("clusterFollowerPills");
	if (!box) return;
	removeAllChildren(box);
	members.forEach(function(m) {
		var pill = document.createElement("button");
		pill.type = "button";
		var label = clusterStateLabel(m);
		pill.className = "pill " + label.kind + " member-pill";
		//host comes off the LAN wire — text nodes only.
		pill.textContent = "row " + (Number(m.row) + 1) + " · " +
			(m.host === "" ? "leader" : m.host) + " · " + label.text;
		pill.addEventListener("click", function() {
			//The digest's empty host = the LEADER's own row — reach it via
			//its host, not ours.
			openMemberPanel(m.host === "" ? (window.lastDigestLeaderHost || "") : m.host, false);
		});
		box.appendChild(pill);
	});
}

//Cluster members on the Maintenance tab (#318 C): while this board leads,
//list every row as a pill that opens the SAME management panel used on the
//Settings card — calibrate/reflash/reboot any board without leaving the tab.
function renderMaintClusterMembers(st) {
	var card = document.getElementById("maintClusterCard");
	var list = document.getElementById("maintClusterList");
	if (!card || !list) return;
	var members = (st && st.enabled) ? (st.members || []) : [];
	card.classList.toggle("hidden", members.length === 0);
	removeAllChildren(list);
	if (members.length === 0) {
		//Tidy up any panel left open when the wall goes away.
		var panel = document.getElementById("maintClusterMemberPanel");
		if (panel) { panel.classList.add("hidden"); removeAllChildren(panel); }
		return;
	}
	members.forEach(function(m) {
		var pill = document.createElement("button");
		pill.type = "button";
		var label = clusterStateLabel(m);
		pill.className = "pill " + label.kind + " member-pill";
		//host comes off the LAN wire — text nodes only.
		pill.textContent = "⚙ row " + (Number(m.row) + 1) + " · " +
			(m.host === "" ? "this board" : m.host) + " · " + label.text;
		pill.addEventListener("click", function() {
			openMemberPanel(m.host, m.host === "" || !!m.self, "maintClusterMemberPanel");
		});
		list.appendChild(pill);
	});
}

//Cluster members on the System tab (#318 D): a per-row vitals table fanned
//out to each board's /settings. ESP-01 rows carry heap/rssi/up (#297); an S3
//row's /settings has none, so it shows its rev and dashes. Called on the
//System tab's poll cadence off the cached /cluster/status member list.
function refreshSysClusterVitals() {
	var card = document.getElementById("sysClusterCard");
	var body = document.getElementById("sysClusterBody");
	if (!card || !body) return;
	var st = window.lastClusterStatus;
	var members = (st && st.enabled) ? (st.members || []) : [];
	card.classList.toggle("hidden", members.length === 0);
	if (members.length === 0) { removeAllChildren(body); return; }
	removeAllChildren(body);
	members.forEach(function(m) {
		var tr = document.createElement("tr");
		function cell(text) {
			var td = document.createElement("td");
			td.textContent = text;
			tr.appendChild(td);
			return td;
		}
		cell("row " + (Number(m.row) + 1));
		//host is LAN-wire text — text node only.
		cell(m.host === "" ? "(this board)" : m.host);
		var revC = cell(m.rev || "—");
		var heapC = cell("…");
		var rssiC = cell("…");
		var upC = cell("…");
		body.appendChild(tr);

		var base = (m.self || m.host === "") ? "" : "http://" + m.host;
		if (base && !/^[A-Za-z0-9.\-]+(:\d+)?$/.test(m.host)) {
			heapC.textContent = rssiC.textContent = upC.textContent = "—";
			return;
		}
		fetch(base + "/settings", { cache: "no-store" })
			.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
			.then(function(s) {
				if (s.version) revC.textContent = s.version;
				//Only the ESP-01 dumb row carries these (#297); an S3 member's
				///settings has none, so its vitals stay dashed.
				heapC.textContent = s.heap !== undefined ? Math.round(s.heap / 1024) + " KB" : "—";
				rssiC.textContent = s.rssi !== undefined ? s.rssi + " dBm" : "—";
				upC.textContent = s.up !== undefined ? formatUptime(s.up) : "—";
			})
			.catch(function() {
				heapC.textContent = "unreachable";
				rssiC.textContent = "—";
				upC.textContent = "—";
			});
	});
}

//Per-member management panel (#294 rung 3): the browser fans out STRAIGHT
//to the member (CORS-gated on its side; the leader never proxies). Wire
//strings render as text nodes only. A member on pre-#294 firmware sends no
//CORS header — the fetch fails and the panel degrades to a plain link.
function openMemberPanel(host, isSelf, panelId) {
	//The panel machinery is panel-relative (every sub-fn takes the element),
	//so the same code drives the Settings card and the Maintenance card (#318
	//C) — only the container id differs.
	var panel = document.getElementById(panelId || "clusterMemberPanel");
	if (!panel) return;
	//Hosts originate on the LAN wire (digest / status) — the fetch target
	//gets the same hostname[:port] allowlist as every visible link.
	if (host !== "" && !/^[A-Za-z0-9.\-]+(:\d+)?$/.test(String(host))) return;
	removeAllChildren(panel);
	panel.classList.remove("hidden");
	var base = isSelf || host === "" ? "" : "http://" + host;

	var head = document.createElement("div");
	head.className = "row";
	var title = document.createElement("h3");
	title.textContent = isSelf || host === "" ? "This board" : host;
	head.appendChild(title);
	var grow = document.createElement("span");
	grow.className = "grow";
	head.appendChild(grow);
	if (base && /^[A-Za-z0-9.\-]+(:\d+)?$/.test(host)) {
		var full = document.createElement("a");
		full.href = "http://" + host + "/";
		full.target = "_blank";
		full.rel = "noopener";
		full.textContent = "open full UI";
		head.appendChild(full);
	}
	var close = document.createElement("button");
	close.type = "button";
	close.className = "btn";
	close.textContent = "✕";
	close.addEventListener("click", function() {
		panel.classList.add("hidden");
		removeAllChildren(panel);
	});
	head.appendChild(close);
	panel.appendChild(head);

	var note = document.createElement("p");
	note.className = "note";
	note.textContent = "Loading " + (base ? host : "this board") + "…";
	panel.appendChild(note);

	Promise.all([
		fetch(base + "/settings", { cache: "no-store" }).then(function(r) { if (!r.ok) throw new Error(); return r.json(); }),
		fetch(base + "/units/health", { cache: "no-store" }).then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
	]).then(function(results) {
		note.remove();
		renderMemberPanelBody(panel, base, host, results[0], results[1]);
	}).catch(function() {
		note.textContent = "Unreachable from this browser — the board may be offline or on pre-#294 firmware without cross-pane management. ";
		if (base && /^[A-Za-z0-9.\-]+(:\d+)?$/.test(host)) {
			var link = document.createElement("a");
			link.href = "http://" + host + "/";
			link.textContent = "Open its own page";
			note.appendChild(link);
		}
	});
}

function memberPanelStatus(panel, message, kind) {
	var el = panel.querySelector(".member-panel-status");
	//A poll (self-test / reflash / firmware) can land after the panel was
	//closed or re-rendered for another member — no status node then, no-op.
	if (!el) return;
	el.className = "status member-panel-status " + (kind || "");
	el.classList.remove("hidden");
	el.textContent = message;
}

//Pollers stop when the panel is gone (closed → hidden, or re-rendered so the
//status node is detached). Guards cross-member status bleed on the bench.
function memberPanelLive(panel) {
	return !panel.classList.contains("hidden") &&
		!!panel.querySelector(".member-panel-status");
}

function renderMemberPanelBody(panel, base, host, settings, health) {
	//Identity + firmware line.
	var meta = document.createElement("p");
	meta.className = "note";
	meta.textContent = (settings.effectiveDeviceName || settings.deviceName || host) +
		" · fw " + (settings.version || "?") +
		(settings.plat ? " · " + settings.plat : "") +
		" · " + (health.units || []).filter(function(u) { return u.st === 1; }).length +
		"/" + (health.width || 0) + " units responding" +
		(health.faulty > 0 ? " · " + health.faulty + " flagged" : "");
	panel.appendChild(meta);

	//ESP-01 vitals (#297): the dumb row's /settings carries heap/rssi/up —
	//show them when present (an S3 member's /settings has none of these).
	if (settings.heap !== undefined || settings.rssi !== undefined || settings.up !== undefined) {
		var vitals = document.createElement("p");
		vitals.className = "note";
		var parts = [];
		if (settings.heap !== undefined) parts.push("heap " + Math.round(settings.heap / 1024) + " KB");
		if (settings.rssi !== undefined) parts.push("rssi " + settings.rssi + " dBm");
		if (settings.up !== undefined) parts.push("up " + formatUptime(settings.up));
		vitals.textContent = parts.join(" · ");
		panel.appendChild(vitals);
	}

	//Device name (the one genuinely per-board setting the wall UI owns).
	var nameRow = document.createElement("div");
	nameRow.className = "row";
	var nameInput = document.createElement("input");
	nameInput.type = "text";
	nameInput.maxLength = 32;
	nameInput.value = settings.deviceName || "";
	nameInput.placeholder = "device name";
	nameRow.appendChild(nameInput);
	var nameSave = document.createElement("button");
	nameSave.type = "button";
	nameSave.className = "btn";
	nameSave.textContent = "Save name";
	nameSave.addEventListener("click", function() {
		postSettingsFields({ deviceName: nameInput.value }, function(ok, text) {
			memberPanelStatus(panel, ok ? "✔ Name saved" + (text === "ok-reboot" ? " — takes effect after its next reboot" : "") : "✘ Save failed", ok ? "success" : "error");
		}, base);
	});
	nameRow.appendChild(nameSave);
	panel.appendChild(nameRow);

	//Unit strip — same color language as the local board strip; click a
	//unit for its remote maintenance ops.
	var strip = document.createElement("div");
	strip.className = "health member-strip";
	var actions = document.createElement("div");
	actions.className = "row member-unit-actions hidden";
	(health.units || []).forEach(function(u, i) {
		var cell = document.createElement("span");
		if (!u || u.st !== 1) cell.className = "bad";
		else if (unitRowIsFaulty(u) || u.fw === 1 || u.mm) cell.className = "warn";
		cell.title = "unit " + (i + 1) + (u && u.st === 1 ? "" : " — silent");
		if (u && u.st === 1) {
			cell.style.cursor = "pointer";
			cell.addEventListener("click", function() {
				renderMemberUnitActions(panel, actions, base, u);
			});
		}
		strip.appendChild(cell);
	});
	panel.appendChild(strip);
	panel.appendChild(actions);

	//Board-level ops.
	var ops = document.createElement("div");
	ops.className = "row";
	function opButton(text, handler, danger) {
		var b = document.createElement("button");
		b.type = "button";
		b.className = "btn" + (danger ? " danger" : "");
		b.textContent = text;
		b.addEventListener("click", handler);
		ops.appendChild(b);
	}
	opButton("Re-probe units", function() {
		fetch(base + "/units/health/refresh", { method: "POST" })
			.then(function(r) { memberPanelStatus(panel, r.ok ? "Probing — reopen the panel in a few seconds for fresh facts." : "✘ Probe refused (busy?)", r.ok ? "success" : "error"); })
			.catch(function() { memberPanelStatus(panel, "✘ Probe request failed", "error"); });
	});
	opButton("Reflash units…", function() {
		if (!confirm("Reflash the Nano units of " + (host || "this board") + " from its bundled hex?\n\nUnits go dark ~1 min each (2 at a time); the row is unusable until it finishes.")) return;
		fetch(base + "/reflash-units", { method: "POST" })
			.then(function(r) {
				if (r.status === 409 || r.status === 503) { memberPanelStatus(panel, "✘ A reflash is already running.", "error"); return; }
				if (!r.ok) { memberPanelStatus(panel, "✘ Reflash refused (HTTP " + r.status + ").", "error"); return; }
				memberPanelStatus(panel, "Reflash queued…", "success");
				memberReflashPoll(panel, base);
			})
			.catch(function() { memberPanelStatus(panel, "✘ Reflash request failed.", "error"); });
	}, true);
	opButton("Reboot board…", function() {
		if (!confirm("Reboot " + (host || "this board") + "? Its row goes dark for a few seconds; the leader re-joins it automatically.")) return;
		fetch(base + "/reboot", { method: "POST" })
			.then(function(r) { memberPanelStatus(panel, r.ok ? "Rebooting…" : "✘ Reboot refused", r.ok ? "success" : "error"); })
			.catch(function() { memberPanelStatus(panel, "Rebooting…", "success"); });
	}, true);
	panel.appendChild(ops);

	//ESP-01 firmware (#304 Part B): push the S3-stored follower image to this
	//row. Upload the image on Settings → Cluster first; the server refuses
	//(409) if none is stored. Shown only for esp01 members (S3 members
	//converge via #276 fleet updates, not this path).
	if (settings.plat === "esp01") {
		var fwRow = document.createElement("div");
		fwRow.className = "row";
		var fwLabel = document.createElement("span");
		fwLabel.className = "calibration-label";
		fwLabel.textContent = "Firmware (esp01)";
		fwRow.appendChild(fwLabel);
		var fwBtn = document.createElement("button");
		fwBtn.type = "button";
		fwBtn.className = "btn danger";
		fwBtn.textContent = "Update firmware…";
		fwBtn.addEventListener("click", function() {
			if (!confirm("Push the S3-stored ESP-01 firmware to " + (host || "this board") + "?\n\nUpload it on the Settings → Cluster card first. The row goes dark ~15 s while it reboots; the leader re-joins it. On failure the current firmware keeps running.")) return;
			fetch("/cluster/member/update?host=" + encodeURIComponent(host), { method: "POST" })
				.then(function(r) {
					return r.text().then(function(t) {
						if (!r.ok) { memberPanelStatus(panel, "✘ " + (t || ("HTTP " + r.status)), "error"); return; }
						memberPanelStatus(panel, "Firmware push queued…", "success");
						memberFirmwarePushPoll(panel, host);
					});
				})
				.catch(function() { memberPanelStatus(panel, "✘ Push request failed.", "error"); });
		});
		fwRow.appendChild(fwBtn);
		panel.appendChild(fwRow);
	}

	var status = document.createElement("div");
	status.className = "status member-panel-status hidden";
	panel.appendChild(status);
}

//Firmware push progress: the relay runs on the leader (clusterTask), so poll
//the leader's own /cluster/status followerPush object. The stage reset
//lastResult, so a stale terminal verdict can't show before this push starts.
function memberFirmwarePushPoll(panel, host) {
	if (!memberPanelLive(panel)) return;
	fetch("/cluster/status", { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(function(st) {
			if (!memberPanelLive(panel)) return;
			var fp = st.followerPush || {};
			if (fp.phase === "uploading") {
				var pct = fp.total > 0 ? Math.floor(fp.sent * 100 / fp.total) : 0;
				memberPanelStatus(panel, "Pushing firmware to " + host + " — " + pct + "% of " + Math.round((fp.total || 0) / 1024) + " KB…", "");
				setTimeout(function() { memberFirmwarePushPoll(panel, host); }, 1500);
			} else if (fp.result === "done") {
				memberPanelStatus(panel, "✔ Firmware flashed — the row is rebooting and will rejoin.", "success");
			} else if (fp.result === "failed" || fp.result === "rejected") {
				memberPanelStatus(panel, "✘ Firmware push " + fp.result + ".", "error");
			} else {
				//staged, clusterTask hasn't picked it up yet.
				setTimeout(function() { memberFirmwarePushPoll(panel, host); }, 1500);
			}
		})
		.catch(function() {
			if (memberPanelLive(panel)) setTimeout(function() { memberFirmwarePushPoll(panel, host); }, 1500);
		});
}

//Board-level reflash progress: poll the member's /units/health reflash
//object, reusing the local-board label/running helpers (#205 shape).
function memberReflashPoll(panel, base) {
	if (!memberPanelLive(panel)) return;
	fetch(base + "/units/health", { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(function(json) {
			if (!memberPanelLive(panel)) return;
			var rf = json.reflash;
			if (rf && reflashIsRunning(rf)) {
				memberPanelStatus(panel, reflashProgressLabel(rf), "");
				setTimeout(function() { memberReflashPoll(panel, base); }, 2000);
			} else if (rf) {
				memberPanelStatus(panel, reflashProgressLabel(rf), "success");
			} else {
				memberPanelStatus(panel, "Reflash finished.", "success");
			}
		})
		.catch(function() {
			if (memberPanelLive(panel)) setTimeout(function() { memberReflashPoll(panel, base); }, 2000);
		});
}

function renderMemberUnitActions(panel, actions, base, u) {
	removeAllChildren(actions);
	actions.classList.remove("hidden");
	var label = document.createElement("span");
	label.className = "calibration-label";
	label.textContent = "Unit " + formatHexAddress(u.a) + (u.rev ? " (fw " + u.rev + ")" : "");
	actions.appendChild(label);
	function actButton(text, handler) {
		var b = document.createElement("button");
		b.type = "button";
		b.className = "btn";
		b.textContent = text;
		b.addEventListener("click", handler);
		actions.appendChild(b);
	}
	actButton("Identify", function() {
		postCalibration(base + "/unit/identify", { address: u.a }, function(ok) {
			memberPanelStatus(panel, ok ? "Unit " + formatHexAddress(u.a) + " is blinking its LED." : "✘ Identify failed", ok ? "success" : "error");
		});
	});
	actButton("Home", function() {
		postCalibration(base + "/unit/home", { address: u.a }, function(ok) {
			memberPanelStatus(panel, ok ? "Homing unit " + formatHexAddress(u.a) + "." : "✘ Home failed", ok ? "success" : "error");
		});
	});
	actButton("Self-test", function() {
		memberSelfTest(panel, base, u.a);
	});
	actButton("Reset odo…", function() {
		if (!confirm("Reset the revolution odometer of unit " + formatHexAddress(u.a) + "? Do this only after replacing its drum/motor.")) return;
		postCalibrationAwait("/unit/reset-odometer", { address: u.a }, function(ok, reason) {
			memberPanelStatus(panel, ok ? "✔ Odometer reset." : "✘ " + reason, ok ? "success" : "error");
		}, base);
	});

	//Jog: relative nudge, fire-and-forget (operator watches the flap).
	var jogRow = document.createElement("div");
	jogRow.className = "row";
	var jogLabel = document.createElement("span");
	jogLabel.textContent = "Jog steps: ";
	jogRow.appendChild(jogLabel);
	var jogInput = document.createElement("input");
	jogInput.type = "number";
	jogInput.className = "calibration-offset";
	jogInput.step = "1";
	jogInput.placeholder = "±steps";
	jogRow.appendChild(jogInput);
	var jogBtn = document.createElement("button");
	jogBtn.type = "button";
	jogBtn.className = "btn";
	jogBtn.textContent = "Jog";
	jogBtn.addEventListener("click", function() {
		var n = parseInt(jogInput.value, 10);
		if (isNaN(n) || n === 0) { memberPanelStatus(panel, "✘ Enter a non-zero step count.", "error"); return; }
		postCalibration(base + "/unit/jog", { address: u.a, steps: n }, function(ok) {
			memberPanelStatus(panel, ok ? "Jogged unit " + formatHexAddress(u.a) + " by " + n + " steps." : "✘ Jog failed", ok ? "success" : "error");
		});
	});
	jogRow.appendChild(jogBtn);
	actions.appendChild(jogRow);

	//Offset: read current, edit, write (EEPROM-mutating → await the outcome).
	var offRow = document.createElement("div");
	offRow.className = "row";
	var offLabel = document.createElement("span");
	offLabel.textContent = "Offset: ";
	offRow.appendChild(offLabel);
	var offInput = document.createElement("input");
	offInput.type = "number";
	offInput.className = "calibration-offset";
	offInput.step = "1";
	offInput.placeholder = "?";
	offRow.appendChild(offInput);
	var offGet = document.createElement("button");
	offGet.type = "button";
	offGet.className = "btn";
	offGet.textContent = "Get";
	offGet.addEventListener("click", function() {
		fetch(base + "/unit/offset?address=" + u.a, { cache: "no-store" })
			.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
			.then(function(data) {
				offInput.value = data.offset;
				memberPanelStatus(panel, "Read offset " + data.offset + " from " + formatHexAddress(u.a), "success");
			})
			.catch(function() { memberPanelStatus(panel, "✘ Read offset failed for " + formatHexAddress(u.a), "error"); });
	});
	offRow.appendChild(offGet);
	var offSet = document.createElement("button");
	offSet.type = "button";
	offSet.className = "btn";
	offSet.textContent = "Set";
	offSet.addEventListener("click", function() {
		var value = parseInt(offInput.value, 10);
		if (isNaN(value)) { memberPanelStatus(panel, "✘ Enter an offset value first.", "error"); return; }
		postCalibrationAwait("/unit/offset", { address: u.a, value: value }, function(ok, reason) {
			memberPanelStatus(panel, ok ? "✔ Saved offset " + value + " to " + formatHexAddress(u.a) : "✘ Save offset failed: " + reason, ok ? "success" : "error");
		}, base);
	});
	offRow.appendChild(offSet);
	actions.appendChild(offRow);
}

//Member self-test (#304): mirrors the local selfTestUnitUi flow but reports
//through the panel status line and targets the member via `base`. The single
//result slot serves one self-test at a time.
function memberSelfTest(panel, base, address) {
	memberPanelStatus(panel, "Self-test on unit " + formatHexAddress(address) + " — about 15 s of motion…", "");
	fetch(base + "/unit/self-test?address=" + address, { method: "POST" })
		.then(function(r) {
			if (!r.ok) return r.text().then(function(t) { throw new Error(t || ("HTTP " + r.status)); });
			return r.json();
		})
		.then(function(data) { memberPollSelfTest(panel, base, address, data.seq, 100); })
		.catch(function(e) { memberPanelStatus(panel, "✘ Self-test failed to queue: " + (e && e.message ? e.message : "request failed"), "error"); });
}

function memberPollSelfTest(panel, base, address, seq, remaining) {
	if (!memberPanelLive(panel)) return;
	fetch(base + "/unit/self-test-result?seq=" + seq, { cache: "no-store" })
		.then(function(r) { if (!r.ok) throw new Error(); return r.json(); })
		.then(function(res) {
			if (!memberPanelLive(panel)) return;
			if (res.state === "pending" && remaining > 0) {
				setTimeout(function() { memberPollSelfTest(panel, base, address, seq, remaining - 1); }, 1000);
				return;
			}
			if (res.state === "ok") {
				var delta = res.steps_per_rev - 2038;
				memberPanelStatus(panel, "Unit " + formatHexAddress(address) + " self-test: " +
					res.steps_per_rev + " steps/rev (" + (delta >= 0 ? "+" : "") + delta +
					" vs nominal), hall window " + res.hall_window + " steps, " +
					(res.rev_time_ms / 1000).toFixed(1) + " s/rev.", "success");
			} else if (res.state === "pending") {
				memberPanelStatus(panel, "Self-test still queued — display busy; check again in a moment.", "");
			} else if (res.state === "expired") {
				memberPanelStatus(panel, "Self-test outcome superseded — run it again.", "error");
			} else {
				memberPanelStatus(panel, "✘ Self-test failed: " + (res.reason || "unknown") + ".", "error");
			}
		})
		.catch(function() { memberPanelStatus(panel, "✘ Self-test result poll failed.", "error"); });
}

function clusterNextFreeRow() {
	var used = {};
	clusterMembers.forEach(function(m) { used[m.row] = true; });
	var row = 0;
	while (used[row]) row++;
	return row;
}

function addClusterBoard(host, width) {
	if (clusterMembers === null) clusterMembers = [];
	var duplicate = clusterMembers.some(function(m) { return m.host === host; });
	if (duplicate) {
		showStatus("clusterCardStatus", "✘ " + escapeHtml(host) + " is already in the member table.", "error", 5000);
		return;
	}
	//First member: this board takes row 0 (a wall without the leader's own
	//row is legal but rarely wanted — remove the row if so).
	if (clusterMembers.length === 0) {
		clusterMembers.push({ host: "", row: 0, col: 0, width: unitCount || 16 });
	}
	clusterMembers.push({ host: host, row: clusterNextFreeRow(), col: 0, width: width || 16 });
	renderClusterMembers();
	showStatus("clusterCardStatus", "Added — adjust row/col/width, then Save cluster.", "success", 6000);
}

function addClusterManualHost() {
	var input = document.getElementById("inputClusterManualHost");
	var host = input.value.trim();
	//Same hostname[:port] allowlist as the follower banner link.
	if (!/^[A-Za-z0-9.\-]+(:\d+)?$/.test(host)) {
		showStatus("clusterCardStatus", "✘ Enter a hostname, IP, or host:port.", "error", 5000);
		return;
	}
	input.value = "";
	addClusterBoard(host, 16);
}

function setClusterCardBusy(busy) {
	["buttonClusterScan", "buttonClusterManualAdd", "buttonClusterSave", "buttonClusterDisable"].forEach(function(id) {
		document.getElementById(id).disabled = busy;
	});
}

//Board discovery (#274): POST arms the browse on the master (the blocking
//mDNS query runs in netTask's drain), then poll GET until it answers 200.
function scanClusterBoards() {
	var suggestions = document.getElementById("clusterSuggestions");
	setClusterCardBusy(true);
	suggestions.classList.add("hidden");
	showStatus("clusterCardStatus", "Browsing the LAN for split-flap boards…", "pending");

	function fail() {
		showStatus("clusterCardStatus", "✘ Discovery failed.", "error", 5000);
		setClusterCardBusy(false);
	}
	fetch("/cluster/discover", { method: "POST" })
		.then(function(response) {
			if (!response.ok) throw new Error();
			var deadline = Date.now() + 10000;
			(function poll() {
				fetch("/cluster/discover", { cache: "no-store" })
					.then(function(r) {
						if (r.status === 202) {
							if (Date.now() > deadline) { fail(); return null; }
							setTimeout(poll, 500);
							return null;
						}
						if (!r.ok) throw new Error();
						return r.json();
					})
					.then(function(result) {
						if (result) {
							renderClusterSuggestions(result.boards || []);
							setClusterCardBusy(false);
						}
					})
					.catch(fail);
			})();
		})
		.catch(fail);
}

function renderClusterSuggestions(boards) {
	var suggestions = document.getElementById("clusterSuggestions");
	while (suggestions.firstChild) suggestions.removeChild(suggestions.firstChild);
	if (boards.length === 0) {
		showStatus("clusterCardStatus", "No other split-flap boards found (they advertise once online, v2 firmware). Use the manual host field for other subnets.", "error", 9000);
		return;
	}
	boards.forEach(function(board) {
		var chip = document.createElement("button");
		chip.type = "button";
		chip.textContent = board.name + " (" + board.host + (board.width ? ", " + board.width + " units" : "") + (board.plat ? ", " + board.plat : "") + ")";
		chip.addEventListener("click", function() {
			addClusterBoard(board.host, board.width || 16);
		});
		suggestions.appendChild(chip);
	});
	suggestions.classList.remove("hidden");
	showStatus("clusterCardStatus", "Found " + boards.length + " board(s) — click one to add it.", "success", 6000);
}

function postClusterConfig(spec, doneMessage) {
	var body = new URLSearchParams();
	body.append("members", spec);
	fetch("/cluster/config", { method: "POST", body: body })
		.then(function(r) {
			return r.text().then(function(text) {
				if (!r.ok) {
					showStatus("clusterCardStatus", "✘ " + escapeHtml(text || "Rejected."), "error");
					return;
				}
				showStatus("clusterCardStatus", doneMessage, "success", 6000);
				clusterMembers = null;  // re-mirror the applied table
				setTimeout(loadClusterStatus, 1000);
			});
		})
		.catch(function() { showStatus("clusterCardStatus", "✘ No connection.", "error", 5000); });
}

function saveClusterCard() {
	if (!clusterMembers || clusterMembers.length === 0) {
		showStatus("clusterCardStatus", "✘ Add at least one member first (Scan or a manual host).", "error", 5000);
		return;
	}
	var spec = clusterMembers.map(function(m) {
		return m.host + "|" + m.row + "|" + m.col + "|" + m.width;
	}).join(";");
	showStatus("clusterCardStatus", "Saving…", "pending");
	postClusterConfig(spec, "✔ Cluster config applied — members join within seconds.");
}

function disableCluster() {
	if (!confirm("Disable the cluster? Members revert to standalone (their own clock) after their grace period.")) return;
	showStatus("clusterCardStatus", "Disabling…", "pending");
	postClusterConfig("", "✔ Cluster disabled.");
}

//Follower-side escape hatch. The leader re-joins this board within seconds
//unless its member table drops the row too — the confirm says so.
function leaveCluster() {
	if (!confirm("Leave the cluster? The leader will re-join this board within seconds unless you also remove it from the leader’s member table first.")) return;
	fetch("/cluster/leave", { method: "POST" })
		.then(function() {
			showStatus("clusterCardStatus", "✔ Left the cluster.", "success", 6000);
			loadPage();
		})
		.catch(function() { showStatus("clusterCardStatus", "✘ No connection.", "error", 5000); });
}
