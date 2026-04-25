# Master PCB v2 — Bring-up procedure

> First-power sequence + per-test-point readings + JLC PCBA verification. Created 2026-04-25.

## Pre-power inspection

1. Visual: verify barrel-jack polarity silkscreen matches your PSU (center-positive).
2. Visual: confirm the antenna keep-out zone is genuinely copper-free on all 4 layers (use the JLC fab-output PDF).
3. Verify all 4 RJ45 shielded jacks are populated and shield tabs are soldered.
4. Verify the BSS138 / MMBT3906 FAULT LED transistor is populated (P4 fix).
5. Verify D_RAIL (SMAJ51A) is within 10 mm of Q1's source pad.
6. Check that R_SERIES + D_SEC are NOT present (they were deleted in #76 — if your fab board has them, you have a stale gerber).

## Step 1 — 3V3 standalone (no 48 V)

Disconnect 48 V. Apply 3.3 V externally to the SWD pin 1 (VTref).

Expected at test pads:
- 3V3 rail: 3.30 V ± 50 mV
- All 4 EFUSE_EN_n pins: 0 V (POR-gated low through BAT54)
- PROT_FAULT_OR: 3.30 V (idle high via 10 kΩ pull-up)
- TELEM_ALERT_OR: 3.30 V (idle high)
- ESP32-S3 VDD pads: 3.30 V

Healthy outcome:
- PWR_3V3 LED on.
- PWR_48V LED off (no 48 V applied).
- FAULT LED off (PROT_FAULT_OR is high; MMBT3906 is off).
- HEARTBEAT LED off (firmware not running yet).

If any deviation, **do not proceed to step 2**.

## Step 2 — 48 V applied, no firmware

Apply 48 V from the recommended PSU through the barrel jack.

Expected:
- LM74700 conducts; V48_RAIL ≈ 47.5 V (drop across Q1 R_DS(on) is negligible at no-load).
- LMR36015 buck wakes (EN divider crosses 1.2 V at ~15 V V48); 3V3 rail still 3.30 V (now self-powered).
- ESP32-S3 boots (will likely sit in download mode if no firmware flashed).
- All 4 eFuses remain disabled (EFUSE_EN_n still low — firmware controls them).
- D_RAIL idle reverse-biased.

Test-pad readings:
- TP_V48: 47.5 V ± 0.5 V
- TP_3V3: 3.30 V ± 30 mV
- TP_PROT_FAULT_OR: 3.30 V (idle high)
- TP_TELEM_ALERT_OR: 3.30 V

Surge / fault behavior to verify on bench (optional, with a bench-DPS):
- Apply 60 V briefly: D_RAIL clamps at ~57 V tail current; LM74700's OVP turns Q1 off.
- Apply reverse polarity (−48 V): LM74700 detects, Q1 stays off, no current draw.

## Step 3 — Flash firmware via USB-C CDC

Plug USB-C from host. ESP32-S3 enumerates as a CDC + JTAG class device. Flash the master firmware via the standard `pio run -t upload` (PlatformIO) or `idf.py flash` workflow.

Verify in firmware boot log:
- HEARTBEAT LED transitions from off to 2 Hz fast (booting) to 1 Hz steady (running + WiFi up) or 0.5 Hz slow (running + WiFi down/AP mode).

## Step 4 — Per-bus enable + INA237 telemetry

Firmware sequences each bus enable in turn:
1. Set EFUSE_EN_n high.
2. Wait for PGOOD_n to assert (eFuse indicator — exposed as a test pad). **Pass-2 note:** the eFuse part is now candidate `TPS26600PWPR` (was `TPS259827YFFR` — invalid on V48); confirm the PGOOD/FAULT pin number against the TPS26600 datasheet at schematic capture, and update bring-up if the new part exposes only FAULT instead of PGOOD.
3. Read INA237_n V_BUS reading. Should be ~47.5 V (rail) at the eFuse output.
4. Read INA237_n current. Should be < 50 mA with no cable plugged in.
5. If INRUSH_NOT_SETTLED (INA237 still > 200 mA after 50 ms), declare bus-fault and disable the eFuse — log to firmware error queue.

Connect a single CAT6 patch cable to bus 1 with a CAT6-to-bare-wires terminator at the far end (just A/B + GND). Verify SN65HVD75_1 idle differential ≈ 280 mV (failsafe bias).

## Step 5 — System-level: master + first case

With a populated case backplane (1 segment + 1 unit slot, minimum) connected to bus 1:
1. Verify INA237_1 current ramps smoothly (~28 ms rise) on EFUSE_EN_1 assertion — no eFuse hiccup.
2. Verify the unit's status LED enters 2 Hz blink (awaiting address).
3. Master firmware initiates UID-based discovery; unit gets an address and LED transitions to solid (idle).
4. Master sends a test step command; unit moves the stepper a fixed amount.

Pass: unit responds within ~100 ms of master command, no bus errors.

## Common bring-up faults

| Symptom | Likely cause |
|---|---|
| 3V3 not present | C_BOOT cap missing or wrong value — LMR36015 high-side gate driver bricked |
| 3V3 brown-in chatter at low V48 | EN divider wrong; check R_EN_TOP / R_EN_BOT values |
| FAULT LED stuck on at boot | MMBT3906 base resistor missing → BJT default-on |
| eFuse hiccups on enable with bus connected | C_dVdT too small (must be 100 nF, not the pre-2026-04-25 10 nF) |
| INA237 reads scaled wrong (~10× off) | VBUS divider — must be 10 k / 1 k (not pre-2026-04-25 100 k / 10 k) |
| BAT54 conducts with 3V3 healthy | BAT54 polarity reversed (anode must be at /RESET, cathode at EFUSE_EN_n) |
| MAX14830 doesn't enumerate over SPI | VEXT not tied to 3V3, or A0/A1 strap pins wrong (verify SPI-mode handling) |
| ESP32-S3 stuck in download mode after USB-C unplug | IO19/20 strap interaction at boot; verify BOOT button + EN debounce |
