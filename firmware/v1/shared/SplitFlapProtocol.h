#pragma once
/* ###########################################################################
 * SplitFlapProtocol.h — single source of truth for the master<->unit contract
 * ###########################################################################
 *
 * Included by BOTH firmwares (they are separate PlatformIO projects — each
 * adds `-I ../shared` to its build so this resolves as a flat include):
 *   - firmware/v1/ESPMaster  (ESP8266 master / I2C bus master)
 *   - firmware/v1/Unit       (Arduino Nano per-flap / I2C slave)
 *
 * Before #149 the opcodes, address base and 45-char alphabet were duplicated
 * across the two sketches AND data/script.js, kept in sync by comment only —
 * drift silently sent wrong characters or mis-decoded opcodes and nothing in
 * CI caught it. Everything on the wire now derives from the macros below:
 *   - master  : `letters[]`      = SFP_ALPHABET, FLAP_AMOUNT  = SFP_FLAP_AMOUNT
 *   - unit    : `LETTER_CHARS[]` = SFP_ALPHABET, AMOUNTFLAPS  = SFP_FLAP_AMOUNT
 *   - web UI  : data/script.js CALIBRATION_LETTERS is verified byte-for-byte
 *               against SFP_ALPHABET at build time by build_assets.py.
 * The native test env asserts the header's own consistency (test_unit_protocol).
 *
 * Pure preprocessor + a static_assert only — no system includes — so it is
 * safe to include anywhere on AVR, ESP8266 and the native (host) test build.
 */

// ---------------------------------------------------------------------------
// Alphabet — the drum's fixed character set. The master sends an INDEX into
// this table (0..SFP_FLAP_AMOUNT-1), never the character itself. Index 0 is
// blank. ä/ö/ü are wire-encoded as $ & # (the web UI translates user input
// before sending). Changing this string changes what every flap must show —
// keep it in lock-step with the physical flap ordering on the drums.
#define SFP_ALPHABET " ABCDEFGHIJKLMNOPQRSTUVWXYZ$&#0123456789:.-?!"

// Number of flaps per unit, derived from the alphabet so the two can never
// disagree. Cast to int to match the historical `#define ... 45` type and to
// avoid size_t/int promotion surprises in the arithmetic both sketches do.
#define SFP_FLAP_AMOUNT ((int)(sizeof(SFP_ALPHABET) - 1))

// ---------------------------------------------------------------------------
// I2C addressing. Address 0x00 is reserved (general call); every unit's
// address is offset by SFP_I2C_ADDRESS_BASE so the master's 0-based unit
// index maps to 0x01..0x10 (DIP "0000" -> 0x01, DIP "1111" -> 0x10).
#define SFP_I2C_ADDRESS_BASE          1

// General-call broadcast address — a write to 0x00 reaches every unit with
// TWGCE enabled. By convention the master only ever broadcasts SFP_CMD_HOME
// (see broadcastHome()); units treat received opcodes the same whether
// addressed individually or via general call, so other opcodes on broadcast
// would produce unintended side effects.
#define SFP_I2C_GENERAL_CALL_ADDRESS  0x00

// ---------------------------------------------------------------------------
// Command opcodes understood by the unit's receiveLetter(). The unit's first
// received byte is a letter index (0..SFP_FLAP_AMOUNT-1) for a normal display
// command, or one of these opcodes (all >= SFP_FLAP_AMOUNT, i.e. >= 45) for a
// command — so opcode values MUST stay above the highest letter index.
//
// Organized into semantic bands (issue #47):
//   0x80..0x8F  queries   — write opcode, then requestFrom for the reply
//   0x90..0x9F  mutations — write opcode (+ args), no read follow-up
//
// 0x80 (ENTER_BOOTLOADER) and 0x81 (GET_VERSION) are FIXED FOREVER across
// protocol bumps — they are the cross-generation recovery path (how a new
// master updates / detects an old unit). Do not renumber them.

// --- 0x8X queries ---
#define SFP_CMD_ENTER_BOOTLOADER   0x80  // trigger watchdog reset into twiboot
#define SFP_CMD_GET_VERSION        0x81  // reply: 8 bytes GIT_REV (null-padded)
#define SFP_CMD_GET_OFFSET         0x82  // reply: 2 bytes int16 LE calOffset
#define SFP_CMD_GET_STATUS         0x83  // reply: 8 bytes health/diag (issue #47)
#define SFP_CMD_GET_LETTER         0x84  // reply: 2 bytes index + ~index (issue #106)

// --- 0x9X mutations ---
// All mutation opcodes defer heavy work (EEPROM write, motor moves, WDT reset)
// to the unit's loop() via pending* flags — doing it in the Wire ISR stalls
// the bus and can drop follow-up transactions.
#define SFP_CMD_HOME               0x90  // no args; unit runs full calibrate(true)
#define SFP_CMD_JOG                0x91  // +1 signed byte (-127..+127)
#define SFP_CMD_REBOOT             0x92  // no args; soft watchdog reset (stays in sketch)
#define SFP_CMD_SET_OFFSET         0x93  // +2 bytes int16 LE; persist to EEPROM
// Provisioning wizard opcodes (issue #56). EEPROM address takes precedence
// over DIP, so burning an EEPROM address makes the DIP switch irrelevant.
#define SFP_CMD_SET_I2C_ADDRESS    0x94  // +1 byte new I2C address (1..126); persist + reboot
#define SFP_CMD_CLEAR_I2C_ADDRESS  0x95  // no args; clear EEPROM magic, reboot -> DIP
#define SFP_CMD_IDENTIFY           0x96  // no args; non-blocking LED_BUILTIN blink (~3 s)

// Opcodes must never collide with a valid letter index. Guard it here so a
// future alphabet growth (or opcode renumber) fails at compile time on both
// firmwares rather than silently mis-decoding on the wire.
static_assert(SFP_CMD_ENTER_BOOTLOADER >= SFP_FLAP_AMOUNT,
              "command opcodes must be >= SFP_FLAP_AMOUNT to not alias letter indices");
