//Used for local development use
const localDevelopment = false;

//Various variables
var unitCount = 0;
var timezoneOffset = 0;

//Must match SFP_ALPHABET in shared/SplitFlapProtocol.h byte-for-byte (index 0
//is blank). build_assets.py verifies this at build time and fails on drift (#149).
//ä/ö/ü are stored as $ & # (wire encoding); user-facing inputs go through
//translateLetterToIndex() which normalizes Unicode umlauts to those ASCII
//glyphs before lookup, so users can type either form.
const CALIBRATION_LETTERS = [' ','A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','$','&','#','0','1','2','3','4','5','6','7','8','9',':','.','-','?','!'];

//Curated POSIX TZ strings for the timezone dropdown (issue #48). Kept
//intentionally short — the ESP-01 serves this page from PROGMEM and the
//flash budget is tight. Add zones sparingly. Strings sourced from:
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
var calibrationUnits = [];  //[{address, versionStatus, version}]

//Used for submission!
const form = document.getElementById('form');
form.onsubmit = function () {
	//Show loading icon
	var containerSubmit = document.getElementById('containerSubmit');
	
	const loadingIconContainer = document.createElement("div");
	loadingIconContainer.className = "lds-facebook";

	for(var index = 0; index < 3; index++) {
		loadingIconContainer.appendChild(document.createElement("div"));
	}

	containerSubmit.replaceWith(loadingIconContainer);

	if (localDevelopment) {
		setTimeout(function() {
			location.reload();
		}, 3000);
		
		return false;
	}
	else {
		const deviceMode = document.querySelector('input[name="deviceMode"]:checked').value;

		if (deviceMode === "text") {
			//Convert characters which don't translate directly, replaces ä, ö, ü with unused unicode characters #, $, &
			var inputTextValue = document.getElementById('inputText').value;
			inputTextValue = inputTextValue.replace(/ä/gi, '$');
			inputTextValue = inputTextValue.replace(/ö/gi, '&');
			inputTextValue = inputTextValue.replace(/ü/gi, '#');
			document.getElementById('inputText').value = inputTextValue;
		}
	}
}

//Tab navigation (#128). Three sections on one page; location.hash deep-links
//a tab and survives refresh. Dispatches "sf-tabchange" so interested panels
//(the log poller) can react without coupling to the tab code.
const TAB_NAMES = ["display", "settings", "maintenance"];

function currentTabFromHash() {
	var name = location.hash.replace("#", "");
	return TAB_NAMES.indexOf(name) >= 0 ? name : "display";
}

function showTab(name) {
	TAB_NAMES.forEach(function(tab) {
		var section = document.getElementById("section-" + tab);
		if (section) section.classList.toggle("hidden", tab !== name);
	});
	document.querySelectorAll(".tabbar-button").forEach(function(button) {
		button.classList.toggle("active", button.dataset.tab === name);
	});
	document.dispatchEvent(new CustomEvent("sf-tabchange", { detail: name }));
}

function initTabs() {
	document.querySelectorAll(".tabbar-button").forEach(function(button) {
		button.addEventListener("click", function() {
			location.hash = button.dataset.tab;
		});
	});
	window.addEventListener("hashchange", function() {
		showTab(currentTabFromHash());
	});
	showTab(currentTabFromHash());
}

//Per-card saves (#128): POST the given fields to / with ajax=1, so the
//backend answers "ok" / "ok-reboot" / 400 instead of redirecting. Only the
//posted fields are applied server-side (provided-gating), so each card can
//save independently.
function postSettingsFields(fields, callback) {
	if (localDevelopment) {
		setTimeout(function() { callback(true, "ok-reboot"); }, 500);
		return;
	}
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

function showCardStatus(elementId, message, kind, hideAfterMs) {
	var el = document.getElementById(elementId);
	if (!el) return;
	el.className = "firmware-status " + (kind || "");
	el.classList.remove("hidden");
	el.innerHTML = message;
	if (hideAfterMs) {
		setTimeout(function() { el.classList.add("hidden"); }, hideAfterMs);
	}
}

var REBOOT_NOW_LINK = ' <a href="#" onclick="return postAction(\'/reboot\', \'Reboot the display now?\');">Reboot now</a>';

function saveDeviceCard() {
	showCardStatus("deviceCardStatus", "Saving…", "pending");
	postSettingsFields({
		deviceName: document.getElementById("inputDeviceName").value,
		timezone: document.getElementById("selectTimezone").value
	}, function(ok, result) {
		if (!ok) showCardStatus("deviceCardStatus", "✘ Save failed — check the device name.", "error");
		else if (result === "ok-reboot") showCardStatus("deviceCardStatus", "✔ Saved. The device name applies after a reboot." + REBOOT_NOW_LINK, "success");
		else showCardStatus("deviceCardStatus", "✔ Saved.", "success", 5000);
	});
}

//Detect and Save share the MQTT card's fields and status line — while a
//discovery poll is in flight the save button (and vice versa the detect
//button) is disabled so the async result can't stomp a save in progress.
function setMqttCardBusy(busy) {
	document.getElementById("buttonMqttDetect").disabled = busy;
	document.getElementById("buttonMqttSave").disabled = busy;
}

function saveMqttCard() {
	setMqttCardBusy(true);
	showCardStatus("mqttCardStatus", "Saving…", "pending");
	postSettingsFields({
		mqttHost: document.getElementById("inputMqttHost").value,
		mqttPort: document.getElementById("inputMqttPort").value,
		mqttUser: document.getElementById("inputMqttUser").value,
		mqttPassword: document.getElementById("inputMqttPassword").value
	}, function(ok, result) {
		setMqttCardBusy(false);
		if (!ok) showCardStatus("mqttCardStatus", "✘ Save failed — check host and port.", "error");
		else if (result === "ok-reboot") showCardStatus("mqttCardStatus", "✔ Saved. MQTT settings apply after a reboot." + REBOOT_NOW_LINK, "success");
		else showCardStatus("mqttCardStatus", "✔ Saved (no changes).", "success", 5000);
	});
}

//Live-apply for the Presentation card (#128): alignment radios post on
//click, the speed slider on release ("change" fires once per deliberate
//adjustment — never while dragging, so no EEPROM churn).
function initLiveApply() {
	document.querySelectorAll('input[name="alignment"]').forEach(function(radio) {
		radio.addEventListener("change", function() {
			postSettingsFields({ alignment: radio.value }, function(ok) {
				showCardStatus("presentationStatus", ok ? "✔ Alignment saved." : "✘ Alignment save failed.", ok ? "success" : "error", 4000);
			});
		});
	});
	document.getElementById("rangeFlapSpeed").addEventListener("change", function(event) {
		postSettingsFields({ flapSpeed: event.target.value }, function(ok) {
			showCardStatus("presentationStatus", ok ? "✔ Speed saved." : "✘ Speed save failed.", ok ? "success" : "error", 4000);
		});
	});
}

//MQTT broker auto-detect (#129). POST arms the discovery on the master (the
//blocking mDNS queries run in its loop()), then poll GET until done. The
//result only prefills the host/port fields — nothing persists until Save.
function detectMqttBroker() {
	var suggestions = document.getElementById("mqttSuggestions");
	setMqttCardBusy(true);
	suggestions.classList.add("hidden");
	showCardStatus("mqttCardStatus", "Searching the LAN for a broker…", "pending");

	if (localDevelopment) {
		setTimeout(function() {
			handleDiscoverResult({ status: "done", candidates: [
				{ host: "192.168.1.10", name: "homeassistant", port: 1883, source: "home-assistant" },
				{ host: "192.168.1.20", name: "mosquitto", port: 1883, source: "mqtt" }
			]});
			setMqttCardBusy(false);
		}, 1000);
		return;
	}

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
							showCardStatus("mqttCardStatus", "✘ Discovery timed out.", "error", 5000);
							setMqttCardBusy(false);
						}
						else setTimeout(poll, 500);
					})
					.catch(function() {
						showCardStatus("mqttCardStatus", "✘ Discovery failed.", "error", 5000);
						setMqttCardBusy(false);
					});
			})();
		})
		.catch(function() {
			showCardStatus("mqttCardStatus", "✘ Discovery failed.", "error", 5000);
			setMqttCardBusy(false);
		});
}

//candidate.name/host come off the mDNS wire — escape before they touch the
//innerHTML-based status line (the suggestion chips use textContent, safe).
function escapeHtml(value) {
	return String(value).replace(/[&<>"']/g, function(c) {
		return "&#" + c.charCodeAt(0) + ";";
	});
}

function applyBrokerSuggestion(candidate) {
	document.getElementById("inputMqttHost").value = candidate.host;
	document.getElementById("inputMqttPort").value = candidate.port;
	showCardStatus("mqttCardStatus", "Prefilled " + escapeHtml(candidate.name) + " — add credentials if needed, then Save MQTT.", "success");
}

function handleDiscoverResult(result) {
	var candidates = result.candidates || [];
	var suggestions = document.getElementById("mqttSuggestions");
	suggestions.innerHTML = "";
	suggestions.classList.add("hidden");

	if (candidates.length === 0) {
		showCardStatus("mqttCardStatus", "No broker found on the LAN. Enter the host manually.", "error", 7000);
		return;
	}
	applyBrokerSuggestion(candidates[0]);
	if (candidates.length > 1) {
		candidates.forEach(function(candidate) {
			var chip = document.createElement("button");
			chip.type = "button";
			chip.className = "mqtt-suggestion";
			chip.textContent = candidate.name + " (" + candidate.host + ":" + candidate.port + ")";
			chip.addEventListener("click", function() { applyBrokerSuggestion(candidate); });
			suggestions.appendChild(chip);
		});
		suggestions.classList.remove("hidden");
	}
}

// Retrieve current Split-Flap settings when the page loads/refreshes
window.addEventListener('load', loadPage);

// Request and retrieve settings from ESP-01s filesystem
function loadPage() {
	//Show messages from the server if need be
	const urlParams = new URLSearchParams(location.search);
	if (urlParams.get('invalid-submission') === "true") {
		showBannerMessage(`
			Something went wrong during submission. Feel free to try again, ensure that you have entered valid information.
		`);
	}
	else if (urlParams.get('is-resetting-units') === "true") {
		showBannerMessage(`
			Display is now resetting/re-calibrating. It should only take a few seconds.
			<br>
			It will display different characters in order to carry this out and then go back to the last thing being displayed.
		`);
	}
	//device-name-saved / mqtt-saved banner params are gone (#128): those
	//fields save via per-card fetch() posts with inline status now, so the
	//server's non-ajax redirect can no longer carry them from any UI action.

	if (localDevelopment) {
		setSpeed("80");
		setSavedMode("text");
		setAlignment("left");
		populateTimezoneOptions();
		setTimezone("");
		setVersion("Development")
		setDeviceName("", "split-flap-9a3c1f");
		setMqttSettings("", "", "", false, false);
		setUnitCount(10, 3);
		setLastReceivedMessage(new Date().toLocaleString());
		showHideResetWifiSettingsAction(false);
		showHideOtaUpdateAction(false);
		setCalibrationUnits([
			{address: 1, versionStatus: 0, version: "3ed3938"},
			{address: 2, versionStatus: 0, version: "3ed3938"},
			{address: 3, versionStatus: 1, version: "fb91753"},
		]);

		setTimeout(function() {
			showContent();
		}, 1000);
	}
	else {
		var xhrRequest = new XMLHttpRequest();
		xhrRequest.onreadystatechange = function () {
			if (this.readyState == 4 && this.status == 200) {
				var responseObject = JSON.parse(this.responseText);
				
				timezoneOffset = responseObject.timezoneOffset;

				setSpeed(responseObject.flapSpeed);
				setSavedMode(responseObject.deviceMode);
				setAlignment(responseObject.alignment);
				populateTimezoneOptions();
				setTimezone(responseObject.timezonePosix || "");
				setVersion(responseObject.version);
				setDeviceName(responseObject.deviceName || "", responseObject.effectiveDeviceName || "");
				setMqttSettings(responseObject.mqttHost || "", responseObject.mqttPort || "",
					responseObject.mqttUser || "", responseObject.mqttPasswordSet === true,
					responseObject.mqttConnected === true);
				setUnitCount(responseObject.unitCount, responseObject.detectedUnitCount);
				setLastReceivedMessage(responseObject.lastTimeReceivedMessageDateTime);
				showHideResetWifiSettingsAction(responseObject.wifiSettingsResettable);
				showHideOtaUpdateAction(responseObject.otaEnabled);

				//Calibration panel is keyed off detectedUnitAddresses — a sketch-
				//running unit that didn't reply to CMD_GET_VERSION predates #28
				//and also predates the calibration opcodes, so we filter it out.
				var calUnits = [];
				var addresses = responseObject.detectedUnitAddresses || [];
				var versionStatuses = responseObject.detectedUnitVersionStatus || [];
				var versions = responseObject.detectedUnitVersions || [];
				for (var i = 0; i < addresses.length; i++) {
					var addr = addresses[i];
					var unitIndex = addr - 1;
					calUnits.push({
						address: addr,
						versionStatus: versionStatuses[unitIndex],
						version: versions[unitIndex] || ""
					});
				}
				setCalibrationUnits(calUnits);

				showContent();
			}
		};

		xhrRequest.open("GET", "/settings", true);
		xhrRequest.send();
	}
}

// Shows a message up top of the page should the server request one to be shown
function showBannerMessage(message, hideAfterDuration) {
	var bannerMessageElement = document.getElementById('bannerMessage'); 
	bannerMessageElement.innerHTML = message;

	bannerMessageElement.classList.remove("hidden");

	if (hideAfterDuration) {
		setTimeout(function() {
			bannerMessageElement.classList.add("hidden");
		}, 7500);
	}
}

//Ongoing show how many characters are being used
function updateCharacterCount() {
	var inputText = document.getElementById('inputText').value;
	var length = inputText.replaceAll("\\n", "").length;

	var labelCharacterCount = document.getElementById("labelCharacterCount");
	var labelLineCount = document.getElementById("labelLineCount");

	labelCharacterCount.innerHTML = length;
	labelLineCount.innerHTML = Math.ceil(length / unitCount) + inputText.split("\\n").length - 1;
}

//Easy add a newline
function addNewline() {
	var inputTextElement = document.getElementById('inputText'); 
	var textWithNewline = inputTextElement.value + "\\n";
	inputTextElement.value = textWithNewline;

	updateCharacterCount();
}

//Updates slider value while sliding
function updateSpeedSlider() {
	var sliderValue = document.getElementById("rangeFlapSpeed").value;
	document.getElementById("rangeFlapSpeedValue").innerHTML = sliderValue + " %";
}

//Sets mode by checking corresponding radio button/tab
function setSavedMode(mode) {
	switch (mode) {
		case "text":
			document.getElementById("modeText").checked = true;
			break;
		case "clock":
			document.getElementById("modeClock").checked = true;
			break;
	}

	setDeviceModeTab(mode);
}

//Shows/hides the tab associated with the device mode
function setDeviceModeTab(mode) {
	document.querySelectorAll('.tab').forEach(function(tab) {
		if (!tab.classList.contains("hidden")) {
			tab.classList.add("hidden");
		}
	});

	var tabName = `tab-${mode}`;
	var tab = document.getElementById(tabName);
	if (tab !== null) {
		tab.classList.remove("hidden");
	}
}

//Sets flap speed by setting the ranges
function setSpeed(speed) {
	document.getElementById("rangeFlapSpeedValue").innerHTML = speed + " %";
	document.getElementById("rangeFlapSpeed").value = speed;
}

//Builds the timezone <select> options once from TIMEZONE_OPTIONS. Idempotent
//so loadPage() can safely call it on every refresh.
function populateTimezoneOptions() {
	var select = document.getElementById("selectTimezone");
	if (!select || select.children.length > 0) return;
	TIMEZONE_OPTIONS.forEach(function(tz) {
		var opt = document.createElement("option");
		opt.value = tz.value;
		opt.textContent = tz.label;
		select.appendChild(opt);
	});
}

//Selects the POSIX TZ currently persisted on the master. If the stored
//value isn't in our dropdown (e.g. set via a compile-time `timezonePosix`
//fallback, or ported from an older firmware), we surface it as a disabled
//"Custom" option so the user sees what's live without losing it on submit.
function setTimezone(tz) {
	var select = document.getElementById("selectTimezone");
	if (!select) return;
	var match = Array.prototype.find.call(select.options, function(opt) { return opt.value === tz; });
	if (match) {
		select.value = tz;
		return;
	}
	var custom = document.createElement("option");
	custom.value = tz;
	custom.textContent = "Custom: " + tz;
	select.insertBefore(custom, select.firstChild);
	select.value = tz;
}

//Sets alignment by checking corresponding radio button
function setAlignment(alignment) {
	switch (alignment) {
		case "left":
			document.getElementById("radioLeft").checked = true;
			break;
		case "center":
			document.getElementById("radioCenter").checked = true;
			break;
		case "right":
			document.getElementById("radioRight").checked = true;
			break;
	}
}

//Sets the version on the UI just for awareness
function setVersion(version) {
	document.getElementById("labelVersion").innerHTML = version;
}

//Per-device identity (#125). Header shows what the device is actually
//using right now; the form field holds the raw stored value ("" = unset)
//with the effective name as placeholder so "unset" is self-explanatory.
function setDeviceName(storedName, effectiveName) {
	document.getElementById("labelDeviceName").textContent = effectiveName || "N/A";

	var input = document.getElementById("inputDeviceName");
	input.value = storedName;
	input.placeholder = effectiveName;
}

//MQTT broker settings (#57). The password is write-only: the server only
//says whether one is stored, and an empty password field means "keep it".
function setMqttSettings(host, port, user, passwordSet, connected) {
	document.getElementById("inputMqttHost").value = host;
	document.getElementById("inputMqttPort").value = port;
	document.getElementById("inputMqttUser").value = user;

	var passwordInput = document.getElementById("inputMqttPassword");
	passwordInput.value = "";
	passwordInput.placeholder = passwordSet ? "(unchanged)" : "Password";

	var status = document.getElementById("labelMqttStatus");
	if (!host) {
		status.textContent = "— off";
	} else {
		status.textContent = connected ? "— connected" : "— not connected";
	}
}

//Shows "<detected> / <width>" in the units label. The JS global
//`unitCount` (used by the input line-count calculation below) tracks the
//display width the master derived from its bus probe (#123) — highest
//responding unit + 1 — so line-break math matches the firmware's layout.
function setUnitCount(total, detected) {
	var label = String(total);
	if (detected !== undefined && detected !== null) {
		label = detected + " / " + total;
	}
	document.getElementById("labelUnits").textContent = label;
	unitCount = total;
}

function convertDateToString(dateTime) {
	const year = dateTime.getFullYear();
	const month = String(dateTime.getMonth() + 1).padStart(2, '0');
  	const day = String(dateTime.getDate()).padStart(2, '0');
	  
	const hours = String(dateTime.getHours()).padStart(2, '0');
	const minutes = String(dateTime.getMinutes()).padStart(2, '0');

	return `${year}-${month}-${day}T${hours}:${minutes}`;
}

//Sets the last received post message to the server
function setLastReceivedMessage(time) {
	const timeMessage = time == "" ? "N/A" : time;
	document.getElementById("labelLastMessageReceived").innerHTML = timeMessage;
}

function showHideResetWifiSettingsAction(isWifiApMode) {
	if (!isWifiApMode) {
		//Hide the whole WiFi card (#128) — a card with only a hidden link
		//would render as an empty box on the Settings tab.
		document.getElementById("cardWifi").classList.add("hidden");
	}
}

function showHideOtaUpdateAction(isOtaEnabled) {
	if (!isOtaEnabled) {
		var linkActionOtaUpdate = document.getElementById("linkActionOtaUpdate");
		linkActionOtaUpdate.classList.add("hidden");
	}
}

//Aborts the running showMessage wait loop, homes every detected unit, and
//clears inputText so the master's event loop doesn't re-issue the previous
//message. Used when a unit gets physically stuck mid-rotation (issue #35).
function stopDisplay() {
	if (!confirm("Stop the display? All detected units will re-home to blank and the current message will be cleared.")) {
		return false;
	}
	var xhr = new XMLHttpRequest();
	xhr.open("POST", "/stop");
	xhr.onreadystatechange = function() {
		if (xhr.readyState !== 4) return;
		if (xhr.status === 200) {
			showBannerMessage(xhr.responseText, 5000);
		} else {
			showBannerMessage("Stop request failed: HTTP " + xhr.status, 5000);
		}
	};
	xhr.send();
	return false;
}

//Pushes every detected sketch-running unit into its twiboot bootloader and
//asks the master to re-flash them from the PROGMEM bundle. Blocks while the
//master works (I2C flashing is serial); progress lines land in /log.
function reflashAllUnits() {
	if (!confirm("Force every detected unit into its bootloader and re-flash from the bundled unit firmware?\n\nLetters will freeze for a few seconds while each unit reboots + gets rewritten. Watch the Log panel for per-unit progress.")) {
		return false;
	}
	var xhr = new XMLHttpRequest();
	xhr.open("POST", "/reflash-units");
	xhr.onreadystatechange = function() {
		if (xhr.readyState !== 4) return;
		if (xhr.status === 200) {
			showBannerMessage(xhr.responseText, 5000);
		} else {
			showBannerMessage("Reflash request failed: HTTP " + xhr.status, 5000);
		}
	};
	xhr.send();
	return false;
}

//POST a state-changing endpoint (#145). These actions used to be GET links,
//so a drive-by <img src="/reset-wifi"> on the LAN could fire them with no
//click. POST forces a scripted same-origin request. Confirms first, then
//shows the server's plain-text response in the banner. Returns false so the
//host <a> never navigates.
function postAction(url, confirmMsg) {
	if (confirmMsg && !confirm(confirmMsg)) {
		return false;
	}
	var xhr = new XMLHttpRequest();
	xhr.open("POST", url);
	xhr.onreadystatechange = function() {
		if (xhr.readyState !== 4) return;
		if (xhr.status >= 200 && xhr.status < 300) {
			showBannerMessage(xhr.responseText || "Done", 8000);
		} else {
			showBannerMessage("Request failed: HTTP " + xhr.status, 5000);
		}
	};
	xhr.send();
	return false;
}


function showContent() {
	var elementInitialLoading = document.getElementById("initialLoading");
	var elementContent = document.getElementById("loadedContent");

	elementInitialLoading.classList.add("hidden");
	elementContent.classList.remove("hidden");

	initTabs();
	initLiveApply();
	initLogPanel();
	initMasterFirmwareUpload();
	initUnitHealth();
}

//Unit Health card (#45). Reads the master's cached per-unit diagnostics JSON
//(GET /units/health) and renders a table; Refresh arms a fresh I2C re-poll
//(POST /units/health/refresh, drained in loop()) then re-fetches. The master
//never touches the Wire bus from the async handler, so the GET is always the
//last cached snapshot.
var unitHealthPollTimer = null;

function initUnitHealth() {
	//Load the cached snapshot whenever the Maintenance tab becomes active, so
	//the table is populated without forcing an I2C re-poll on every visit.
	document.addEventListener('sf-tabchange', function (event) {
		if (event.detail === "maintenance") loadUnitHealth();
	});
	if (currentTabFromHash() === "maintenance") loadUnitHealth();
	if (localDevelopment) {
		renderUnitHealth({
			width: 3, faulty: 1, units: [
				{ i: 0, a: 1, st: 1, v: 1, fw: 0, up: 3720, br: 0, wd: 0, bc: 0, mc: 1, fl: 0, hs: 720 },
				{ i: 1, a: 2, st: 1, v: 1, fw: 1, up: 60, br: 2, wd: 1, bc: 4, mc: 4, fl: 2, hs: 2160 },
				{ i: 2, a: 3, st: 2, v: 0 }
			]
		});
	}
}

function refreshUnitHealth() {
	setUnitHealthSummary("Polling units…", "pending");
	fetch("/units/health/refresh", { method: "POST" })
		.then(function () {
			//Give loop() a beat to drain the flag + poll every unit over I2C
			//(~2 ms + round-trip per unit) before reading the fresh cache.
			if (unitHealthPollTimer !== null) clearTimeout(unitHealthPollTimer);
			unitHealthPollTimer = setTimeout(loadUnitHealth, 1200);
		})
		.catch(function () { setUnitHealthSummary("Refresh request failed.", "error"); });
}

function loadUnitHealth() {
	fetch("/units/health", { cache: "no-store" })
		.then(function (r) { return r.json(); })
		.then(renderUnitHealth)
		.catch(function () { setUnitHealthSummary("Could not load unit health.", "error"); });
}

function setUnitHealthSummary(text, kind) {
	var el = document.getElementById("unitHealthSummary");
	if (!el) return;
	el.textContent = text;
	el.className = "firmware-status " + (kind || "");
}

//MCUSR reset-cause bits on the ATmega328P (Unit.ino byte 1). Decoded here in
//JS so the device payload stays a single small integer.
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

var UNIT_STATE_LABELS = { 0: "silent", 1: "sketch", 2: "bootloader" };
var UNIT_FW_LABELS = { 0: "ok", 1: "OUTDATED", 2: "unknown" };

function unitRowIsFaulty(u) {
	//Mirror UnitHealth.h's unitStatusIsFaulty(): home-failed / hall-never /
	//any lifetime brownout or watchdog. Only meaningful for a read unit.
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

function renderUnitHealth(data) {
	var table = document.getElementById("unitHealthTable");
	var body = document.getElementById("unitHealthBody");
	if (!table || !body) return;
	removeAllChildren(body);

	var units = (data && data.units) || [];
	var responding = 0;
	for (var k = 0; k < units.length; k++) {
		if (units[k].st !== 0) responding++;
	}
	var faulty = (data && typeof data.faulty === "number") ? data.faulty : 0;
	var width = (data && typeof data.width === "number") ? data.width : units.length;

	if (units.length === 0) {
		table.classList.add("hidden");
		setUnitHealthSummary("No units detected.", faulty > 0 ? "error" : "");
		return;
	}
	table.classList.remove("hidden");

	for (var idx = 0; idx < units.length; idx++) {
		var u = units[idx];
		var row = document.createElement("tr");
		var bad = unitRowIsFaulty(u);
		if (bad) row.className = "uh-bad-row";
		appendCell(row, u.i);
		appendCell(row, "0x" + ("0" + u.a.toString(16)).slice(-2));
		appendCell(row, UNIT_STATE_LABELS[u.st] || u.st, u.st === 1 ? "" : "uh-bad");
		if (u.v) {
			appendCell(row, UNIT_FW_LABELS[u.fw] || u.fw, u.fw === 0 ? "" : "uh-warn");
			appendCell(row, formatUptime(u.up));
			appendCell(row, u.br, u.br > 0 ? "uh-bad" : "");
			appendCell(row, u.wd, u.wd > 0 ? "uh-bad" : "");
			appendCell(row, u.bc, u.bc > 0 ? "uh-warn" : "");
			appendCell(row, homeCellText(u), (u.fl & 0x06) ? "uh-bad" : "");
			appendCell(row, decodeMcusr(u.mc));
		} else {
			//Unread unit (bootloader / silent / pre-#47 firmware): no diagnostics.
			for (var c = 0; c < 7; c++) appendCell(row, "—");
		}
		body.appendChild(row);
	}

	var summary = responding + "/" + width + " responding";
	summary += faulty > 0 ? " · " + faulty + " faulty" : " · all healthy";
	setUnitHealthSummary(summary, faulty > 0 ? "error" : "success");
}

//Derive "&v=<rev>" for /firmware/master from a build-stamped filename
//(firmware-<rev>[-dirty]-<size>.bin, see build_assets.py). The rev feeds
//the boot-time silent-revert check (intendedVersion, #52); a renamed file
//just skips the param — same behaviour as before #160.
//KEEP IN SYNC: the same regex is inlined in ServiceBootModes.ino's
//MINIMAL_UPLOAD_FORM (the recovery/quiet-OTA pages can't load this file).
function firmwareVersionParam(fileName) {
	var match = fileName.match(/^firmware-([0-9a-f]{7,40}(?:-dirty)?)(?:-[0-9]+m[0-9]*m?)?\.bin$/i);
	return match ? "&v=" + encodeURIComponent(match[1]) : "";
}

//Master firmware OTA: streams a .bin upload to /firmware/master, which
//flashes the ESP itself via the Update class and reboots.
function initMasterFirmwareUpload() {
	var form = document.getElementById("masterFirmwareForm");
	if (!form) return;

	form.addEventListener("submit", function (event) {
		event.preventDefault();

		var fileInput = document.getElementById("inputMasterFirmwareFile");
		var submitButton = document.getElementById("buttonMasterFirmwareSubmit");
		var status = document.getElementById("masterFirmwareStatus");

		var file = fileInput.files[0];
		if (!file) return;

		var confirmFlash = confirm(
			"Flash " + file.name + " (" + file.size + " bytes) to the master?\n\n" +
			"The ESP reboots into the new firmware on success. On failure, " +
			"the current firmware continues to run."
		);
		if (!confirmFlash) return;

		submitButton.disabled = true;
		fileInput.disabled = true;
		status.className = "firmware-status pending";
		status.classList.remove("hidden");
		status.textContent = "Computing MD5…";

		//?md5= is mandatory on /firmware/master (#144): hash the image
		//client-side (SparkMD5, md5.js) so the handler can verify the
		//upload arrived intact before committing it (#160).
		var reader = new FileReader();
		reader.onerror = function () {
			submitButton.disabled = false;
			fileInput.disabled = false;
			status.className = "firmware-status error";
			status.textContent = "✘ Could not read the selected file.";
		};
		reader.onload = function () {
			var md5 = SparkMD5.ArrayBuffer.hash(reader.result);
			status.textContent = "Uploading master firmware…";

			var formData = new FormData();
			formData.append("firmware", file);

			var xhr = new XMLHttpRequest();
			xhr.open("POST", "/firmware/master?md5=" + md5 + firmwareVersionParam(file.name));
			xhr.onreadystatechange = function () {
				if (xhr.readyState !== 4) return;

				submitButton.disabled = false;
				fileInput.disabled = false;

				if (xhr.status === 200) {
					status.className = "firmware-status success";
					status.textContent = "✔ " + xhr.responseText + " The master will be offline ~15 s while rebooting.";
				} else if (xhr.status === 0) {
					status.className = "firmware-status error";
					status.textContent = "✘ Upload failed — lost connection to master.";
				} else {
					status.className = "firmware-status error";
					status.textContent = "✘ HTTP " + xhr.status + ": " + xhr.responseText;
				}
			};
			xhr.send(formData);
		};
		reader.readAsArrayBuffer(file);
	});
}

//Log panel: polls GET /log every 2s while the <details> is open.
function initLogPanel() {
	var details = document.getElementById("logDetails");
	var pre = document.getElementById("logContent");
	if (!details || !pre) return;

	var pollHandle = null;

	function fetchLog() {
		fetch('/log', { cache: 'no-store' })
			.then(function (r) { return r.ok ? r.text() : ''; })
			.then(function (text) {
				//Preserve scroll-lock-to-bottom UX: if the user was already at the
				//bottom, stay pinned there as new content arrives.
				var atBottom = (pre.scrollTop + pre.clientHeight) >= (pre.scrollHeight - 8);
				pre.textContent = text;
				if (atBottom) pre.scrollTop = pre.scrollHeight;
			})
			.catch(function () { /* network hiccups shouldn't blow up the UI */ });
	}

	function startPolling() {
		if (pollHandle !== null) return;
		fetchLog();
		pollHandle = setInterval(fetchLog, 2000);
	}

	function stopPolling() {
		if (pollHandle === null) return;
		clearInterval(pollHandle);
		pollHandle = null;
	}

	//Poll only while the log is actually visible: <details> open, browser
	//tab in the foreground AND the Maintenance tab active (#128).
	var onMaintenanceTab = currentTabFromHash() === "maintenance";

	function syncPolling() {
		if (details.open && !document.hidden && onMaintenanceTab) startPolling();
		else stopPolling();
	}

	details.addEventListener('toggle', syncPolling);

	document.addEventListener('sf-tabchange', function (event) {
		onMaintenanceTab = event.detail === "maintenance";
		syncPolling();
	});

	document.addEventListener('visibilitychange', syncPolling);

	//<details open> doesn't fire the toggle event on load, so sync once
	//manually in case the page loads straight onto #maintenance.
	syncPolling();

	if (localDevelopment) {
		pre.textContent = "Starting Split-Flap...\nScanning I2C bus for units...\n- unit responding at 0x01\n- unit responding at 0x02\nI2C scan complete. Detected 2/10 expected units.\n";
	}
}

//Calibration panel (issue #32). Builds a per-unit row with Expect/Reality
//inputs plus an Advanced <details> with raw jog & offset controls.
function removeAllChildren(element) {
	while (element.firstChild) element.removeChild(element.firstChild);
}

function setCalibrationUnits(units) {
	calibrationUnits = units || [];
	var card = document.getElementById("calibrationCard");
	var container = document.getElementById("containerCalibrationUnits");
	var advancedContainer = document.getElementById("containerCalibrationAdvanced");
	if (!card || !container || !advancedContainer) return;

	removeAllChildren(container);
	removeAllChildren(advancedContainer);

	if (calibrationUnits.length === 0) {
		card.classList.add("hidden");
		return;
	}
	card.classList.remove("hidden");

	//Populate the shared <datalist> (one copy, referenced by every Reality
	//input) and the test-letter <select>. Skip blank (index 0) for the
	//datalist — "expect blank" is ambiguous to eyeball. Default to "A".
	var datalist = document.getElementById("calibrationLetters");
	var select = document.getElementById("selectCalibrationLetter");
	if (datalist && datalist.children.length === 0) {
		for (var i = 0; i < CALIBRATION_LETTERS.length; i++) {
			var opt = document.createElement("option");
			opt.value = CALIBRATION_LETTERS[i];
			datalist.appendChild(opt);
		}
	}
	if (select && select.children.length === 0) {
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

	//Version badge. Green = unit is on the bundled firmware (#120). Amber =
	//outdated/unknown: those units can still try the opcodes — older firmware
	//will silently drop them — so flag visually so the user isn't surprised
	//when nothing happens.
	var badge = document.createElement("span");
	if (unit.versionStatus === 0) {
		badge.className = "calibration-ok";
		badge.textContent = unit.version ? "(fw " + unit.version + ")" : "(fw ok)";
	} else {
		badge.className = "calibration-warn";
		badge.textContent = unit.versionStatus === 1 ? "(outdated fw)" : "(fw unknown)";
	}
	row.appendChild(badge);

	if (advanced) {
		buildAdvancedControls(row, unit);
	} else {
		buildExpectRealityControls(row);
	}
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
	offsetInput.value = "";
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
	var btn = document.createElement("input");
	btn.type = "button";
	btn.value = label;
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
	//Normalize to the ASCII wire encoding used by ESPMaster.ino's letters[].
	//Users may type either Ä or $; both resolve to index 27.
	if (ch === 'ä' || ch === 'Ä') ch = '$';
	else if (ch === 'ö' || ch === 'Ö') ch = '&';
	else if (ch === 'ü' || ch === 'Ü') ch = '#';
	else ch = ch.toUpperCase();
	return CALIBRATION_LETTERS.indexOf(ch);
}

//"Send to all" submits the main form with the chosen letter repeated
//unitCount times. Reuses the existing showMessage path — simpler than a
//dedicated endpoint and keeps device state consistent with what the user
//sees elsewhere.
function sendCalibrationLetter() {
	var letter = getSelectedCalibrationLetter();
	showCalibrationStatus("Sending '" + letter + "' to all detected units…", "pending");

	//Build a padded string of `letter` repeated unitCount times — the
	//master's showMessage() iterates by unit index; shorter messages leave
	//tail units unchanged.
	var padded = "";
	for (var i = 0; i < unitCount; i++) padded += letter;

	var form = new FormData();
	form.append("alignment", document.querySelector('input[name="alignment"]:checked').value);
	form.append("flapSpeed", document.getElementById("rangeFlapSpeed").value);
	form.append("deviceMode", "text");
	form.append("inputText", padded);

	var xhr = new XMLHttpRequest();
	xhr.open("POST", "/");
	xhr.onreadystatechange = function() {
		if (xhr.readyState !== 4) return;
		if (xhr.status >= 200 && xhr.status < 400) {
			showCalibrationStatus("Sent '" + letter + "'. Fill Reality for each unit and click Apply All.", "success");
			document.querySelectorAll("#containerCalibrationUnits .calibration-expect").forEach(function(el) {
				el.textContent = "Expect: " + letter;
			});
		} else {
			showCalibrationStatus("Failed to send test letter: HTTP " + xhr.status, "error");
		}
	};
	xhr.send(form);
}

function homeAllCalibrationUnits() {
	showCalibrationStatus("Re-homing all detected units…", "pending");
	var remaining = calibrationUnits.length;
	if (remaining === 0) {
		showCalibrationStatus("No units detected.", "error");
		return;
	}
	var failures = 0;
	calibrationUnits.forEach(function(unit) {
		postCalibration("/unit/home", { address: unit.address }, function(ok) {
			if (!ok) failures++;
			remaining--;
			if (remaining === 0) {
				if (failures === 0) {
					showCalibrationStatus("Re-homed " + calibrationUnits.length + " unit(s).", "success");
				} else {
					showCalibrationStatus("Re-homed with " + failures + " failure(s).", "error");
				}
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
	var xhr = new XMLHttpRequest();
	xhr.open("GET", "/unit/offset?address=" + address);
	xhr.onreadystatechange = function() {
		if (xhr.readyState !== 4) return;
		if (xhr.status === 200) {
			try {
				var data = JSON.parse(xhr.responseText);
				row.querySelector(".calibration-offset").value = data.offset;
				showCalibrationStatus("Read offset " + data.offset + " from " + formatHexAddress(address), "success");
			} catch (e) {
				showCalibrationStatus("Unparseable offset response from " + formatHexAddress(address), "error");
			}
		} else {
			showCalibrationStatus("Read offset failed for " + formatHexAddress(address) + ": HTTP " + xhr.status, "error");
		}
	};
	xhr.send();
}

function saveCalibrationOffset(row) {
	var address = parseInt(row.dataset.address, 10);
	var input = row.querySelector(".calibration-offset");
	var value = parseInt(input.value, 10);
	if (isNaN(value)) {
		showCalibrationStatus("Offset for " + formatHexAddress(address) + " is not a number", "error");
		return;
	}
	postCalibration("/unit/offset", { address: address, value: value }, function(ok) {
		showCalibrationStatus(ok
			? "Saved offset " + value + " to " + formatHexAddress(address)
			: "Save offset failed for " + formatHexAddress(address), ok ? "success" : "error");
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
	var xhr = new XMLHttpRequest();
	xhr.open("GET", "/unit/offset?address=" + address);
	xhr.onreadystatechange = function() {
		if (xhr.readyState !== 4) return;
		if (xhr.status !== 200) {
			showCalibrationStatus("Read offset failed for " + formatHexAddress(address) + " — aborting", "error");
			return;
		}
		var currentOffset;
		try {
			currentOffset = JSON.parse(xhr.responseText).offset;
		} catch (e) {
			showCalibrationStatus("Unparseable offset for " + formatHexAddress(address), "error");
			return;
		}
		//new_offset = current − (reality − expect) × steps_per_flap.
		//Rationale: if the drum shows reality = expect + 1 flap, it
		//over-rotated by one flap, so stop one flap earlier.
		var delta = entry.realityIndex - expectIndex;
		var correction = Math.round(delta * CALIBRATION_STEPS_PER_FLAP);
		var newOffset = currentOffset - correction;

		postCalibration("/unit/offset", { address: address, value: newOffset }, function(ok) {
			if (!ok) {
				showCalibrationStatus("Save offset failed for " + formatHexAddress(address), "error");
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
	};
	xhr.send();
}

function postCalibration(path, params, callback) {
	var query = Object.keys(params).map(function(k) {
		return encodeURIComponent(k) + "=" + encodeURIComponent(params[k]);
	}).join("&");
	var xhr = new XMLHttpRequest();
	xhr.open("POST", path + "?" + query);
	xhr.onreadystatechange = function() {
		if (xhr.readyState !== 4) return;
		callback(xhr.status >= 200 && xhr.status < 400);
	};
	xhr.send();
}

function showCalibrationStatus(message, kind) {
	var el = document.getElementById("calibrationStatus");
	if (!el) return;
	el.className = "firmware-status " + (kind || "");
	el.classList.remove("hidden");
	el.textContent = message;
}