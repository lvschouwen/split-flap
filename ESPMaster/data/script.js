//Used for local development use
const localDevelopment = false;

//Various variables
var unitCount = 0;
var timezoneOffset = 0;

//Mirrors ESPMaster.ino's char letters[] byte-for-byte. Index 0 is blank.
//ä/ö/ü are stored as $ & # (wire encoding); user-facing inputs go through
//translateLetterToIndex() which normalizes Unicode umlauts to those ASCII
//glyphs before lookup, so users can type either form.
const CALIBRATION_LETTERS = [' ','A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','$','&','#','0','1','2','3','4','5','6','7','8','9',':','.','-','?','!'];
const CALIBRATION_STEPS_PER_FLAP = 2038 / 45;
var calibrationUnits = [];  //[{address, versionStatus}]

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
		const tzOffset = timezoneOffset * 60000;

		switch(deviceMode) {
			case "text":
				//Convert characters which don't translate directly, replaces ä, ö, ü with unused unicode characters #, $, &
				var inputTextValue = document.getElementById('inputText').value;
				inputTextValue = inputTextValue.replace(/ä/gi, '$');
				inputTextValue = inputTextValue.replace(/ö/gi, '&');
				inputTextValue = inputTextValue.replace(/ü/gi, '#');
				document.getElementById('inputText').value = inputTextValue;

				//Set the hidden date time to UNIX
				var currentScheduledDateTimeText = document.getElementById('inputScheduledDateTime').value;

				//Take into account the timezone offset when we generate the unix timestamp
				var currentScheduledDateTime = new Date(currentScheduledDateTimeText);
				var time = Math.floor((currentScheduledDateTime.getTime() - tzOffset) / 1000);
				document.getElementById('inputHiddenScheduledDateTimeUnix').value = time;

				break;
		}
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
			<br>
			Ensure things like dates provided for schedules are in the future.
		`);
	}
	else if (urlParams.get('is-resetting-units') === "true") {
		showBannerMessage(`
			Display is now resetting/re-calibrating. It should only take a few seconds.
			<br>
			It will display different characters in order to carry this out and then go back to the last thing being displayed.
		`);
	}
	
	//Set date time fields to be a minimum of todays date/time add 1 minute
	var tzOffset = timezoneOffset * 60000;
	document.querySelectorAll('input[type="datetime-local"]').forEach((dateTimeElement) => {
		var currentDateTime = convertDateToString((new Date(Date.now() - tzOffset + 60000)));
		dateTimeElement.value = dateTimeElement.min = currentDateTime;
	});

	if (localDevelopment) {
		setSpeed("80");
		setSavedMode("text");
		setAlignment("left");
		setVersion("Development")
		setUnitCount(10, 3);
		setLastReceivedMessage(new Date().toLocaleString());
		showHideResetWifiSettingsAction(false);
		showHideOtaUpdateAction(false);
		setCalibrationUnits([
			{address: 1, versionStatus: 0},
			{address: 2, versionStatus: 0},
			{address: 3, versionStatus: 1},
		]);
		showScheduledMessages([
			{
				"scheduledDateTimeUnix": 1690134480,
				"message": "Test Message 1",
				"showIndefinitely": false
			},
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
				setVersion(responseObject.version);
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
				for (var i = 0; i < addresses.length; i++) {
					var addr = addresses[i];
					var unitIndex = addr - 1;
					calUnits.push({
						address: addr,
						versionStatus: versionStatuses[unitIndex]
					});
				}
				setCalibrationUnits(calUnits);

				if (responseObject.scheduledMessages) {
					showScheduledMessages(responseObject.scheduledMessages);
				}

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

//Send message to delete a message
function deleteScheduledMessage(id, message) {
	var confirmDeletion = confirm(`Delete Message '${message}'?`);
	if (!confirmDeletion) {
		return false;
	}

	var xhr = new XMLHttpRequest();
	xhr.onreadystatechange = function () {
		//Reload the page
		if (this.readyState == 4 && this.status == 202) {
			window.location.reload();
		}
	};

	xhr.open("DELETE", `/scheduled-message/remove?id=${id}`, true);
	xhr.send();
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

//Shows "<detected> / <max>" in the units label. The JS global
//`unitCount` (used by the input line-count calculation below) tracks
//the compile-time max so breaking text into lines stays consistent
//regardless of how many units the master actually saw on the bus.
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

//Used for scheduling messages
function showHideScheduledMessageInput() {
	var scheduleOptionsElement = document.getElementById("divScheduleOptions");
	var checkboxScheduled = document.getElementById("inputCheckboxScheduleEnabled");

	if (checkboxScheduled.checked) {
		scheduleOptionsElement.classList.remove("hidden")
	}
	else {
		scheduleOptionsElement.classList.add("hidden")
	}
}

function showHideResetWifiSettingsAction(isWifiApMode) {
	if (!isWifiApMode) {
		var linkActionResetWifi = document.getElementById("linkActionResetWifi");
		linkActionResetWifi.classList.add("hidden");
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
	xhr.open("GET", "/reflash-units");
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

//Formats and displays all scheduled messages in a "nice" format
function showScheduledMessages(scheduledMessages) {
	var elementMessageCount = document.getElementById("spanScheduledMessageCount");
	elementMessageCount.innerText = scheduledMessages.length;

	//Closest to being shown first
	scheduledMessages = scheduledMessages.sort((a, b) => a.scheduledDateTimeUnix - b.scheduledDateTimeUnix);

	var container = document.getElementById("containerScheduledMessages");

	scheduledMessages.forEach(function(scheduledMessage) {
		//Create a container for a message
		var messageElement = document.createElement("div");
		messageElement.className = "message";

		//Create a element to show the time
		var timeElement = document.createElement("div");
		timeElement.className = "time";
		timeElement.innerText = new Date((scheduledMessage.scheduledDateTimeUnix * 1000) + (timezoneOffset * 60000)).toString().slice(0, -34);

		//Create a element to show the text. Build via DOM + textContent so user
		//message text can't inject HTML (previously interpolated into innerHTML).
		var textElement = document.createElement("div");
		textElement.className = "text";

		var messageLabel = document.createElement("b");
		messageLabel.textContent = "Message:";
		var displayMessage = scheduledMessage.message.trim() == "" ? "<Blank>" : scheduledMessage.message;
		textElement.appendChild(messageLabel);
		textElement.appendChild(document.createTextNode(" " + displayMessage));
		textElement.appendChild(document.createElement("br"));

		var indefiniteLabel = document.createElement("b");
		indefiniteLabel.textContent = "Shown Indefinitely:";
		textElement.appendChild(indefiniteLabel);
		textElement.appendChild(document.createTextNode(" " + (scheduledMessage.showIndefinitely ? "Yes" : "No")));

		//Create a remove button. Use addEventListener so message text isn't
		//interpolated into an onclick attribute string.
		var actionElement = document.createElement("div");
		var actionButtonElement = document.createElement("span");
		actionElement.className = "action";
		actionButtonElement.className = "remove-button";
		actionButtonElement.innerText = "Remove";
		actionButtonElement.addEventListener('click', function() {
			deleteScheduledMessage(scheduledMessage.scheduledDateTimeUnix, scheduledMessage.message);
		});
		actionElement.appendChild(actionButtonElement);

		//Add all the elements to the message
		messageElement.appendChild(timeElement);
		messageElement.appendChild(textElement);
		messageElement.appendChild(actionElement);

		container.appendChild(messageElement);
	});
}

function showContent() {
	var elementInitialLoading = document.getElementById("initialLoading");
	var elementContent = document.getElementById("loadedContent");

	elementInitialLoading.classList.add("hidden");
	elementContent.classList.remove("hidden");

	initLogPanel();
	initMasterFirmwareUpload();
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
		status.textContent = "Uploading master firmware…";

		var formData = new FormData();
		formData.append("firmware", file);

		var xhr = new XMLHttpRequest();
		xhr.open("POST", "/firmware/master");
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

	details.addEventListener('toggle', function () {
		if (details.open) startPolling();
		else stopPolling();
	});

	//<details open> doesn't fire the toggle event on load, so kick off
	//polling manually if the panel starts expanded.
	if (details.open) startPolling();

	//Also stop polling if the tab is hidden — no point waking the ESP for updates nobody's reading.
	document.addEventListener('visibilitychange', function () {
		if (document.hidden) stopPolling();
		else if (details.open) startPolling();
	});

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

	//Version-outdated or unknown units can still try the opcodes — older
	//firmware will silently drop them. Flag visually so the user isn't
	//surprised when nothing happens.
	if (unit.versionStatus !== 0) {
		var warn = document.createElement("span");
		warn.className = "calibration-warn";
		warn.textContent = unit.versionStatus === 1 ? "(outdated fw)" : "(fw unknown)";
		row.appendChild(warn);
	}

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
	form.append("scheduledDateTimeUnix", "0");

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