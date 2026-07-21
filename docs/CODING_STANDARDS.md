# Split-Flap Coding Standards

**Status:** Active · applies to all code under `firmware/v2/`, `flashing/`, and root tooling.
**Exempt:** `firmware/v1/` (frozen), generated files (`*Assets.h`, `managed_components/`, `sdkconfig.*`).

This document defines *how code in this project must be written*: architecture rules, language
style, concurrency, security, testing, and tooling gates. Project *facts* (repository map, mechanism
inventory, hard rules such as pin assignments and resource owners) live in [`CLAUDE.md`](../CLAUDE.md);
where the two conflict, CLAUDE.md's Hard rules win.

The key words **MUST**, **MUST NOT**, **SHOULD**, and **MAY** are used as in RFC 2119:
MUST is a gate (a reviewer blocks on it), SHOULD requires a stated reason to deviate,
MAY is explicitly allowed.

---

## Contents

1. [Architecture principles](#1-architecture-principles)
2. [C++ style — embedded profile](#2-c-style--embedded-profile)
3. [Concurrency and real-time rules](#3-concurrency-and-real-time-rules)
4. [Memory management](#4-memory-management)
5. [Security requirements](#5-security-requirements)
6. [Testing standards](#6-testing-standards)
7. [Tooling and CI gates](#7-tooling-and-ci-gates)
8. [Python, shell, and web-asset standards](#8-python-shell-and-web-asset-standards)
9. [Documentation and comments](#9-documentation-and-comments)
10. [Review checklists](#10-review-checklists)
11. [Sources](#11-sources)

---

## 1. Architecture principles

### 1.1 Pure policy + thin glue

Decision logic **MUST** live in dependency-free headers (`*Policy.h`, `*Plan.h`, `*Helpers.h`,
`*Digest.h`) that compile on the native host and carry unit tests. Target-only glue (`.cpp` files,
`Nvs*.h`) stays thin: it moves bytes, calls the policy, and applies the result. If you find yourself
writing an `if`-ladder about *what to do* inside a `.cpp`, extract it into a testable header first.

The bench (real hardware) is the E2E tier for glue — a mechanism is not "done" until bench-proven.

### 1.2 Single owner per resource

Every mutable resource — a bus, a filesystem, a flash partition, a state struct — has exactly one
owning task or translation unit. All other code goes through that owner's API or a queue.
Existing owners are pinned in CLAUDE.md Hard rules (`UnitBus.cpp` for Wire, netTask for the
LittleFS `storage` partition, displayTask for display state). A new mutable resource **MUST** name
its owner in the owning file's header comment.

### 1.3 Command in, snapshot out

Cross-task interaction is *enqueue a command* / *read a mutex-copied snapshot*:

- Producers (web, MQTT, cluster, clock) **MUST NOT** touch display or device state directly —
  they enqueue a `DisplayCommand` with all parameters baked in by the sender.
- Readers get a consistent copy (`DisplaySnapshot`) taken under the mutex — never a live reference.
- Async callbacks (web handlers, MQTT callbacks) **MUST NOT** perform blocking device I/O; they
  stage data and flags, and the owning task's tick does the work.

### 1.4 Copied headers are a deliberate policy

There is no shared-include graph across the v2 projects; pure headers are **copies**
(Master ↔ FollowerEsp01, Master → Rescue trims). The price of that choice is a sync duty:

- Every copied header **MUST** carry a `// copied:` note at the top naming its sibling paths.
- A bug fix in a copied header **MUST** land in every tree that carries it, in the same commit.
- Copies meant to be identical **SHOULD** be byte-identical, so CI can diff them.
- Deliberately trimmed copies (Rescue, FollowerEsp01 subsets) **MUST** state *what* was trimmed
  and why in the header comment.

### 1.5 File and function size

- Hand-written files: 200–400 lines typical, **800 lines maximum**. Over the cap, split by
  mechanism — the `WebEndpoints.cpp` → `Web*.cpp` family split is the model.
- Functions **SHOULD** stay under ~50 lines and do one mechanism; nesting **SHOULD** stay ≤ 4 levels.
- Generated files (`WebAssets.h`, `UnitAssets.h`, `RescueAssets.h`) are exempt.

---

## 2. C++ style — embedded profile

This is a pragmatic subset in the spirit of MISRA C++:2023 and BARR-C, adapted for
Arduino/ESP-IDF targets. We do not claim MISRA compliance; we adopt the rules that pay for
themselves on this codebase.

### 2.1 Types

- **Fixed-width integers** (`uint8_t`, `int16_t`, `uint32_t`, …) **MUST** be used for anything with
  a defined layout or range: wire formats, I2C/EEPROM/NVS fields, counters that wrap, bitmasks.
  Plain `int` **MAY** be used for local arithmetic with no layout meaning.
- Time deltas **MUST** use the unsigned-subtraction idiom, which is rollover-safe:

  ```cpp
  // CORRECT — well-defined across millis() wraparound
  if ((uint32_t)(millis() - startedAtMs) > TIMEOUT_MS) { ... }

  // WRONG — breaks at wraparound
  if (millis() > startedAtMs + TIMEOUT_MS) { ... }
  ```

- Signed/unsigned comparisons **MUST NOT** be mixed silently; cast explicitly at the boundary.

### 2.2 Error handling

Every fallible boundary call — `Wire`/I2C transactions, NVS, LittleFS, `esp_*` APIs,
`esp_http_client`, `Update`, JSON parsing — **MUST** have its return value checked, and the
failure path **MUST** leave state consistent:

- No half-written NVS pairs (write the dependent key only after the prerequisite key succeeded;
  order writes so a crash between them fails safe).
- No held mutex, no stuck "in progress" flag, no leaked buffer on the early-return path.
- Never silently swallow an error: degrade with a log line at minimum, propagate where the caller
  can act on it.

```cpp
// The failure path resets the state machine — never just `return`.
if (!settingsStore.putU32(KEY_EPOCH, epoch)) {
    log("cluster: epoch persist failed");
    membershipDirty = true;   // retry next tick instead of wedging
    return false;
}
```

### 2.3 Bounded everything

- Every retry loop **MUST** have an attempt cap or wall-clock deadline.
- Every parser **MUST** length-check before it indexes. External input decides *content*, never
  *how much work we do*.
- Recursion **MUST NOT** be used (fixed stacks, no headroom).

### 2.4 Const, scope, and globals

- APIs **SHOULD** be `const`-correct; read-only parameters are `const&` or by value.
- File-scope state **MUST** be `static` (internal linkage); cross-TU globals need a named owner
  (§1.2).
- Globals with non-trivial constructors **MUST NOT** depend on cross-TU initialization order.

### 2.5 Mutation

In-place mutation of statically allocated state is **idiomatic and required** here — this
deliberately overrides the global "immutability" rule from the general rules set. Copying churns
small heaps; the safety substitute is single ownership (§1.2) and snapshot copies (§1.3).

### 2.6 Formatting

There is **no `.clang-format`** in this repo. **MUST NOT** blind-reformat; match the surrounding
file's style exactly (indent, brace placement, naming).

---

## 3. Concurrency and real-time rules

### 3.1 Task topology (ESP32-S3 Master)

The topology is fixed: display domain pinned to core 1, network domain (net/mqtt/cluster tasks) on
core 0. New work **SHOULD** join an existing task's tick. A genuinely new task **MUST** justify
itself and its stack size in its header comment (deepest call path, measured high-water mark).

### 3.2 Shared data

- Data crossing tasks or cores **MUST** go through a queue, a mutex-copied snapshot, or an
  explicitly documented atomic knob. Nothing crossing cores is assumed atomic — not even a
  32-bit scalar, unless declared `std::atomic` and documented as such.
- Mutex hold times **MUST** be short and **MUST NOT** span device I/O, flash writes, or network
  calls. Copy out under the lock, work outside it.
- Beware TOCTOU between reading a snapshot and enqueuing a command: commands bake in their
  parameters; the executing task re-validates against live state before acting (the
  `MaintenancePolicy` re-run-pre-burn pattern).

### 3.3 Watchdog discipline

- Every app task subscribes to the 30 s TWDT and **feeds inside** long operations — it **MUST
  NOT** unsubscribe to survive one (`TaskWatchdog.h` is the contract).
- Any loop that can exceed ~1 s (flash writes, chunked streams, bus scans, reflash) **MUST**
  feed per iteration and wall-clock-bound each tick.

### 3.4 ESP8266 follower (superloop)

`FollowerEsp01` is single-core, no RTOS: async handlers **only stage** flags and buffers;
`loop()` is the sole mutator and the sole I2C toucher. Anything else is a data race with the
async TCP callbacks.

### 3.5 ISR rules (Nano unit)

TWI ISR ↔ main-loop shared variables **MUST** be `volatile`; multi-byte values shared with an ISR
**MUST** be read/written under `cli()/sei()` or via a handshake flag. The ISR does minimal work —
stage bytes, set a flag, return.

---

## 4. Memory management

- Steady-state allocation: allocate at init, reuse forever. Repeated alloc/free in hot paths or
  per-request **MUST NOT** be introduced.
- Large or long-lived buffers on the S3 **MUST** go through `LargeAlloc.h` (PSRAM-preferred).
- Arduino `String` is acceptable at the web/JSON boundary; it **MUST NOT** appear in tick/ISR
  paths, and **SHOULD** `reserve()` when the size is known. On the ESP8266/Nano, prefer
  fixed `char` buffers with `snprintf`.
- Stack: large locals (>~256 B) in task functions **SHOULD** be `static` or heap/PSRAM — task
  stacks are sized tightly and measured, not padded.
- On the Nano (2 KB RAM): string literals in `PROGMEM`/`F()`, buffer sizes derived from the
  protocol's declared maxima, and a known worst-case stack margin.

---

## 5. Security requirements

### 5.1 Threat model

Devices live on a **trusted private LAN** behind the operator's router/VPN.
Adversaries defended against:

1. a malicious client *on* the LAN, and
2. a malicious web page in an operator's browser (CSRF, DNS rebinding).

**Not** defended against, by explicit decision (accepted risks — revisit ~yearly, and immediately
if any device becomes WAN-reachable):

| Accepted risk | Rationale |
|---|---|
| No secure boot / flash encryption / anti-rollback eFuses | irreversible eFuse burns; bricking risk outweighs on a hobby fleet |
| HTTP (not HTTPS) on the LAN wire | trusted LAN; embedded TLS cost |
| No login/auth on the local web UI | trusted LAN; CSRF layer covers the browser vector |
| MD5 as the firmware transfer check | integrity only — authenticity comes from the LAN boundary + CSRF/HMAC layers |

Any future WAN-reachable update path **MUST** add signature verification (Secure Boot v2-style
signed images or app-level signature check) *before* it ships. MD5 is never an authenticity claim.

### 5.2 Input validation

Every wire input **MUST** be validated at the boundary before use — HTTP params and bodies, MQTT
payloads, cluster JSON, I2C replies, mDNS TXT records. External data is hostile even when it comes
from "our own" peers (a peer may be compromised or a different firmware rev):

- Length-cap **before** parse; reject oversized bodies outright.
- Range-check numerics; missing/malformed JSON keys degrade, never crash.
- Strings that reach I2C, EEPROM, filenames, or shell **MUST** be charset-filtered
  (the `readUnitVersion` reject-quotes pattern).
- Checksummed replies (unit odometer, health) **MUST** fail closed on checksum mismatch.

### 5.3 Browser-origin defense (CSRF/XSS)

- Every mutating HTTP route **MUST** call the CSRF gate: non-LAN-`Origin` POSTs are 403'd
  (`clusterCsrfRejectPost` / `followerCsrfRejectPost`); upload routes that write flash gate
  **inline at `index == 0`** in `onUpload` (`webUploadCsrfRejected`).
- Adding a route means adding it to the CSRF audit surface — in **both** header copies where the
  follower carries the endpoint.
- The web UI **MUST** render wire-derived strings as text nodes only — never `innerHTML`.

### 5.4 Cluster wire authentication

- Leader-wire requests carry the per-member HMAC (`ts` + `mac`, HMAC-SHA256 over the canonical
  string, ±30 s window + monotonic per-member mark). New leader-wire endpoints **MUST** join the
  HMAC layer *and* the source-IP binding — in both trees.
- The canonical string **MUST** be unambiguous (delimiters that cannot appear in the fields, or
  length-prefixing) so two different messages can never canonicalize identically.
- MAC comparison **SHOULD** be constant-time.
- Keys: minted from the hardware RNG, stored in NVS only, rotated on leader reboot, and
  **MUST NOT** be logged, serialized into any JSON response, or committed anywhere.

### 5.5 SSRF and outbound requests

Any user-configurable host/URL the firmware will connect to **MUST** pass the LAN-target check
(`clusterHostIsLanTarget`), and every embedded HTTP client **MUST** set `disable_auto_redirect`.

### 5.6 Firmware update safety

- Uploads: mandatory `?md5=`, chunk offset/length bounds-checked against the partition, stalls
  bounded by the OTA stall watchdog, concurrent uploads rejected.
- The A/B contract stands: images boot `PENDING_VERIFY` and are confirmed pre-inrush at end of
  `setup()`; hold the first flash sector back until the verdict where the pattern applies
  (`FactorySlot.cpp`).
- Server-to-server relay routes stay closed to browsers; `/firmware/*` on followers is never a
  browser surface.

### 5.7 Secrets and surface hygiene

- WiFi credentials and HMAC keys live in NVS only — never in code, logs, `/settings` JSON, issues
  (public repo!), or committed files. `WiFi.persistent(false)` everywhere.
- No new listening surface (endpoint, port, MQTT topic, mDNS TXT field) without a stated need in
  the owning file's header.
- Debug/test hooks (e.g. `RESCUE_CRASH_TEST`) **MUST** be compile-time-gated OFF by default —
  never a runtime flag.
- **Fail closed:** parse failure, auth failure, out-of-window timestamp → reject. Never
  "best-effort apply" a partially valid request.

---

## 6. Testing standards

- **TDD for pure logic:** a policy header gets its native test *first* (`pio test -e native` in the
  owning project). Red → green → refactor.
- **Coverage intent:** ~80 % of host-testable pure logic. Hardware glue is exempt — its tier is
  the bench drill (OTA cycle, reflash, failover, rescue), and a mechanism is not done until
  bench-proven.
- **Regression pinning:** every bug fix lands with a test that failed before the fix — in every
  tree that carries the copied header.
- **Wire twins:** protocol sides are pinned by paired fakes (`fake_follower.py` ↔ `fake_leader.py`)
  run under pytest so the two ends cannot drift. New wire fields join the twins in the same PR.
- **ArduinoFake quirks** (documented in CLAUDE.md): `map()` must be wired in each test's `setUp()`;
  `EEPROM` etc. re-wire via `ArduinoFake(EEPROM)`.
- CI green (all builds + native suites + pytest + drift gates) is a merge precondition.
  A red leg is never hand-waved.

---

## 7. Tooling and CI gates

- **One version of any dependency, fleet-wide — no exceptions.** A library shared by more than one
  project (e.g. `ESPAsyncWebServer`, `AsyncTCP`) **MUST** be pinned to the **same, latest vetted
  version in every project that uses it**. Two versions of one library is how a security or bug fix
  lands in one image and silently misses another — exactly the Rescue-on-3.7.0 vs Master-on-3.11.2
  case that shipped the OTA path on a pre-fix multipart parser. When you bump a shared library, bump
  it **everywhere in the same PR**, and prefer exact pins (`@3.11.2`) over caret ranges (`@^3.11.2`)
  for shared libraries so every build machine resolves the identical version. A project that
  genuinely cannot take the latest (platform incompatibility) **MUST** state why in its
  `platformio.ini` and get an issue to close the gap — it is a tracked exception, never a silent one.
- `pio run` **MUST** be clean in every touched project dir before commit; new compiler warnings in
  touched files are review comments to resolve or explicitly waive.
- **Static analysis:** `pio check` (cppcheck) is the standing expectation for new/changed code.
  Treat new findings in touched files like review comments: fix or waive with a reason.
- **Drift gates:** the unit-bundle rev gate and any copied-header identity check run in CI.
  Adding a copied header means adding it to the check.
- The Unit-firmware bundle flow is ordered and **MUST** be respected: commit code → rebuild Unit
  clean → `make_manifest.py stage` → rebuild Master + FollowerEsp01 → **separate** artifact commit
  (never amend the bundle in, never squash-merge a stage-commit PR).
- Build scripts (`build_assets.py`, `use_custom_bootloader.py`, `make_manifest.py`) **MUST** fail
  loudly (non-zero exit) on any error — a silent fallback in a build script is a fleet incident
  later.

---

## 8. Python, shell, and web-asset standards

### Python (`flashing/`, `tests/`, build scripts)

- Stdlib-preferred; a new dependency needs a reason.
- Explicit error handling; non-zero exit on failure; no bare `except:`.
- No secrets in argv or committed files. `python -m pytest` green.

### Shell (`ota-flash.sh` and friends)

- `set -euo pipefail`, quote every expansion, `trap` cleanup for staged files.
- No destructive operation (erase, overwrite, mass fan-out) without an explicit flag or prompt.
- Exit codes tell the truth: a failed flash **MUST NOT** exit 0.

### Web assets (`data/style.css`, `data/script.js`)

- Wire strings become text nodes only (§5.3).
- UI changes are mockup-gated (design-confirm rule) — no unreviewed visual changes.
- Assets are PROGMEM-baked by `build_assets.py`; `WebAssets.h` is included by `WebContent.cpp`
  **only** (a second include duplicates every blob).

---

## 9. Documentation and comments

- **Comments state constraints only** — invariants, ownership, ordering, units — things the code
  cannot show. No history, no narration, no "why my change is correct" (git and the PR hold those).
- Per-mechanism detail lives in the **header comment of the file that owns it**; CLAUDE.md holds
  one line per mechanism pointing there. CLAUDE.md is current-state only, never a changelog.
- Feature design docs go under `docs/superpowers/specs/`, dated.
- Issues are the history store: issue first, `effort:`/`gain:` labels, no private data (public repo).

---

## 10. Review checklists

### Every C++ change

- [ ] Boundary return values checked; failure paths leave consistent state (§2.2)
- [ ] Loops bounded, parsers length-checked before indexing (§2.3)
- [ ] Time comparisons rollover-safe (§2.1)
- [ ] No new cross-task data without queue/snapshot/atomic + doc (§3.2)
- [ ] Long operations feed the watchdog and are wall-clock-bounded (§3.3)
- [ ] No heap churn in hot paths; large buffers via LargeAlloc/PSRAM (§4)
- [ ] Copied headers updated in every tree, byte-identical where meant to be (§1.4)
- [ ] File under 800 lines or split proposed (§1.5)
- [ ] Pure logic extracted to a testable header, test written first (§1.1, §6)

### Every new/changed endpoint or wire message

- [ ] Input length-capped, range-checked, charset-filtered as applicable (§5.2)
- [ ] CSRF gate applied (inline at `index==0` for uploads), in both trees (§5.3)
- [ ] Leader-wire: HMAC + source-IP binding, both trees (§5.4)
- [ ] No secret can appear in the response or logs (§5.7)
- [ ] Fails closed on any validation error (§5.7)
- [ ] Wire twins (`fake_follower`/`fake_leader`) extended in the same PR (§6)

### Risk-tiered review triggers (always get a dedicated review pass)

OTA / boot / flash-write / concurrency / credentials / cluster-wire changes.

---

## 11. Sources

Industry references this document adapts (not compliance claims):

- ESP-IDF Security Guide — secure boot, flash encryption, OTA signing (basis for §5.1's accepted-risk table): [docs.espressif.com — Security Overview](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/security.html)
- NCC Group, *ESP32 Security by Design*: [nccgroup.com](https://www.nccgroup.com/research/hardware-security-by-design-esp32-guidance/)
- OWASP IoT Top 10 (basis for §5.2–§5.7): [owasp.org — Internet of Things](https://owasp.org/www-project-internet-of-things/)
- MISRA C++:2023 / BARR-C spirit (basis for §2–§3): [misra.org.uk](https://misra.org.uk/), [barrgroup.com — Embedded C Coding Standard](https://barrgroup.com/embedded-systems/books/embedded-c-coding-standard)
- C++ Core Guidelines: [isocpp.github.io/CppCoreGuidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
- PlatformIO — Static Code Analysis (`pio check`) and Unit Testing: [docs.platformio.org](https://docs.platformio.org/en/latest/advanced/static-code-analysis/index.html)
