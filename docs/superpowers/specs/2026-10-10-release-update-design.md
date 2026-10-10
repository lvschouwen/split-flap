# Update from a signed release, and a web flasher (#583)

Status: **approved by the owner 2026-10-10; section 10 measured the same day and passed.** Nothing else is built.

## 1. Purpose

A wall finds a newer release by itself and installs it at the press of a button. A new board is flashed from a browser. It is built for the owner's wall first and must work for anyone who runs this firmware, including from a fork.

## 2. Decisions taken (owner, 2026-10-10)

- Releases are published under `split-flap.vanschouwen.nl`, served by GitHub Pages. The wall fetches from that name and from nowhere else.
- The master looks for a release once a day. A setting turns that off. It only looks: installing always needs a press of the button.
- One press updates the boards: the master, the row image (handed to the rows the way it is today) and the rescue slot. Unit firmware stays the separate **Update units** job.
- The rescue image can fetch the latest master image.
- A release is signed on the build machine. The wall verifies what it downloads. An image uploaded over the LAN (`POST /firmware/master`, `/firmware/row`, `/firmware/rescue`, `ota-flash.sh`) is not checked for a signature: the LAN stays trusted.
- A normal start never needs the internet. The A/B slots, rollback and the boot guard are unchanged.
- The ESP-01 row board never talks to the internet.
- Hardware secure boot and flash encryption are out of scope. The signature protects against a changed download or a taken-over host, not against someone holding the board.

## 3. What a release publishes

A `gh-pages` branch of this repository, served as `split-flap.vanschouwen.nl`:

| Path | What |
|---|---|
| `/` | The web flasher page (section 8) |
| `/releases/latest.json` | The manifest of the newest release |
| `/releases/latest.json.sig` | Its signature |
| `/releases/<tag>/firmware-<rev>-master.bin` | Master image (what `POST /firmware/master` takes) |
| `/releases/<tag>/follower-<rev>-gz.bin` | Row image, packed (what `POST /firmware/row` takes) |
| `/releases/<tag>/rescue-<rev>.bin` | Rescue image (what `POST /firmware/rescue` takes) |
| `/releases/<tag>/factory-<rev>-master.bin` | Whole-flash image of a master for USB: bootloader, partition table, application, rescue |
| `/releases/<tag>/follower-<rev>.bin` | Row image, plain, for USB |
| `/releases/<tag>/flasher.json` | What the flasher page reads for this release |
| `/test/...` | The same layout for a trial release (section 5) |

The newest three releases are kept; older directories are removed by the release script.

### The manifest

```json
{
  "format": 1,
  "tag": "v2026.10.10",
  "notes": "https://github.com/lvschouwen/split-flap/releases/tag/v2026.10.10",
  "commitTime": 1791619200,
  "master": {"rev": "9509ccc", "path": "v2026.10.10/firmware-9509ccc-master.bin", "size": 1612384, "sha256": "…"},
  "row":    {"rev": "9509ccc", "path": "v2026.10.10/follower-9509ccc-gz.bin", "size": 326333, "sha256": "…", "md5": "…"},
  "rescue": {"rev": "47640a1", "path": "v2026.10.10/rescue-47640a1.bin", "size": 997696, "sha256": "…"}
}
```

- `commitTime` is the committer time, in seconds, of the commit the release is tagged at.
- `path` is relative to `/releases/` (or `/test/`). A board refuses a path that is not relative or that leaves that directory.
- `md5` on the row image is what the row board checks today; it stays.
- At most 2 KB. A board refuses a larger one and a `format` it does not know.

### The signature

ECDSA over P-256 with SHA-256, over the exact bytes of `latest.json`. `latest.json.sig` is the signature in DER, base64. The framework's crypto library has this scheme and not Ed25519.

One signature covers everything: an image is trusted because its SHA-256 is in a manifest whose signature checked out.

### Keys

- The private key is one file on the build machine, outside the repository. It needs a copy kept somewhere safe.
- The public key and the host name are two constants in `firmware/v2/release/ReleaseSource.h`. A fork changes both.
- One key. A lost or changed key is recovered by one LAN upload of an image built with the new public key, which needs no signature.

## 4. What counts as newer

Every image gets the committer time of the commit it was built from baked in at build time, next to its rev.

A release is offered when the manifest's `commitTime` is later than the running image's. Equal or earlier is "up to date".

- A build ahead of the release is never offered the release.
- An old manifest played back to a wall offers nothing.
- Nothing reads meaning from a tag.

The rescue slot and the stored row image are compared by rev against the manifest, as today's Firmware screen compares them.

## 5. Cutting a release

`flashing/release.py`, run on the build machine, replaces the manual tag-and-notes steps:

1. Takes the commit the staged images were built from (`--rev`, `HEAD` by default) and refuses unless: the tree is clean; that commit is on the pushed `master`; CI is green on it; the master and row images staged in `~/bench-bins` carry its rev, and the Master build directory holds that same build (the whole-flash image is merged from its bootloader and partition table); no tag exists for today. The rescue image is the newest staged one built from a commit that the release contains.
2. Builds the whole-flash master image and writes `flasher.json`.
3. Writes the manifest, signs it, and checks the signature with the public key from `ReleaseSource.h` — a key pair that does not match stops here.
4. Commits to `gh-pages` (new directory, `latest.json` and its signature last, old directories pruned) and pushes.
5. Creates the annotated tag at that commit and the GitHub release with the notes file it was given. The images are attached to the GitHub release as well.

`--channel test` does steps 2 to 4 under `/test/`, with no tag and no GitHub release. A wall is pointed at the test channel by a setting (section 6), so a release can be tried on the wall before it is cut.

The release policy in the root `CLAUDE.md` (CalVer, at most one a day, BREAKING in the title) is unchanged; the script enforces the mechanical parts.

## 6. On the master

### Shared between master and rescue: `firmware/v2/release/`

A directory outside `shared/`, like `link/` and `buildtools/`: no unit compiles it, so an edit there must not move the unit source head.

- `ReleaseSource.h` — host name, public key.
- `ReleaseManifest.h` — pure: reads a manifest into a struct, refuses what section 3 says to refuse, answers "newer than this commit time" and "this image differs from that rev". Natively tested.
- `ReleaseFetch.h` — the fetch-and-verify steps over hooks (get bytes, check signature, feed a writer, hash), so the order "signature first, then hash, then activate" is tested natively with stand-in hooks.

### Looking

On the worker task: two minutes after a start once the clock is set, then every 24 hours; after a failed look, one hour later. The request carries nothing about the wall.

The result is kept in memory: not looked yet / up to date / newer release (tag, notes link, the three revs) / failed (why). A setting `releaseCheck` (on by default) turns the daily look off; `check-release` still looks when asked. A setting `releaseChannel` (`stable` by default, or `test`) picks the path.

### Installing

Action `update-from-release`: a job with a number, read at `GET /api/v2/op/<id>`. It looks again first, so it never installs from a result that is a day old. Then, in this order:

1. **Rescue**, when its rev differs: downloaded into the factory slot through the writer `POST /firmware/rescue` uses, which holds the first sector back until the image checked out.
2. **Row image**, when its rev differs: downloaded into the store `POST /firmware/row` uses. An image stored by this job is **held**: it is offered to the rows only once the master itself runs the release's master rev. A master that fell back to its old image keeps the rows where they are.
3. **Master**: downloaded into the other slot through the writer `POST /firmware/master` uses, hashed while it is written. The slot is made the next to start only when the SHA-256 matches. Then the ordinary restart.

Each download is checked against its size and SHA-256 from the manifest. Anything that does not match is thrown away and ends the job as failed, with the reason. A failure at any step before the restart leaves the running image running.

After the restart the existing rules apply: the new image confirms itself or the board falls back; three crash starts hand over to rescue.

The job is refused while a unit update runs, and while it runs the display producers stand down exactly as for an upload. It adds no flash writer: it feeds the three that exist.

### API

- `POST /api/v2/action`: `check-release`, `update-from-release`.
- `GET /api/v2/firmware` gains `release`: state, tag, notes link, revs, when it last looked, and the held row image if there is one.
- `PUT /api/v2/settings/wall`: `releaseCheck`, `releaseChannel`.
- The history gets entries for a release found, an update started, and how it ended.

No Home Assistant entity in this version.

## 7. On the page

- Wall screen: one line, "New release v2026.10.10 found", linking to the Firmware screen. Shown only for a newer release.
- Firmware screen: the release, a link to its notes, what would change (master, row image, rescue; and whether the units will read outdated afterwards), an **Update** button with a confirm, and progress by step. After the restart the page reloads itself, as after an upload.
- Wall settings: the daily look on/off, and the channel.

## 8. In the rescue image

A "Get the latest release" button on its page: the same look and verify, then the master image only, written to `app0` the way its upload is. Offered when rescue joined the owner's WiFi; in its own access-point mode there is no internet and the page says so.

Rescue has no clock of its own to rely on, and "newer" needs none: it shows what the release is and what the slots hold, and the person decides.

## 9. Web flasher

A static page at `/` using ESP Web Tools (a pinned version; Chrome and Edge only, since it needs Web Serial). Two buttons:

- **Master (ESP32-S3, 16 MB)** — the whole-flash image at offset 0.
- **Row board (ESP-01, 1 MB)** — the plain row image at offset 0.

After flashing, each board is set up through its own WiFi portal as today. The page also carries the short "what next" text: join WiFi, open the master, pair the rows.

## 10. Measured before code

On the installed master, with the clock running and the row linked:

1. **Memory for an HTTPS download.** Fetch a file of master-image size from GitHub Pages, hashing it, five times. Record the lowest free internal memory and whether anything restarted or dropped the row. If internal memory does not carry it, try the TLS buffers in PSRAM and measure again.
2. **The signature check** exists in the framework build both S3 images use, and how long one check takes.
3. **Image size** of master and rescue with the client and the check linked in.
4. **GitHub Pages** serves the images with a length and without a redirect to another host.

If 1 fails with PSRAM as well, the design goes back to the owner before anything else is built.

### Results (2026-10-10, master on a trial build of 9509ccc)

| | Result |
|---|---|
| HTTPS download, TLS memory internal (the framework default) | **Fails.** The TLS library cannot allocate (`MBEDTLS_ERR_SSL_ALLOC_FAILED`) with 58 KB free and a largest block of 31.7 KB. |
| HTTPS download, TLS memory in PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`) | **5 of 5.** 1 612 384 bytes each, SHA-256 right every time, 8.6 to 20 s, handshake about 0.5 s. Internal memory never under 52 KB free during a download; the lowest ever seen on that start stayed 40 KB, where it is without the download. No restart, the row stayed linked. |
| Stack | 3.5 KB used by connect, download and hash. |
| Signature check | Present. One check of a P-256 signature takes 0.28 s; a message with one changed bit is refused. |
| Image size | +75 KB on the master (1 612 384 to 1 687 472 bytes) with the client, the check and the trial code; 4 MB slot. |
| GitHub Pages | 200 with `content-length`, `application/octet-stream`, no redirect. |

So the master is built with `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`. It moves every TLS-library allocation to PSRAM, including what joining WiFi uses; the trial build joined and ran normally with it. The rescue image needs the same setting and is measured when it is built (order of work, step 4). Not measured: a download while it is also written to flash.

## 11. Tests

- Native: `ReleaseManifest.h` (well-formed, oversized, unknown format, path leaving the directory, newer/equal/older), `ReleaseFetch.h` order and failure paths with stand-in hooks, the held row image rule.
- Python: `release.py` against a throwaway repository and key — refusals of step 1, the signature it writes checks out with the standard library's counterpart, pruning keeps three.
- A gate that the public key in `ReleaseSource.h` is a valid P-256 key and that the page, the script and the boards agree on the paths of section 3.
- On the wall, through the test channel: an update end to end; a manifest with one changed byte; a manifest signed with another key; an image with one changed byte; a download cut off halfway; the row image held across a master that falls back.

## 12. Order of work

1. Section 10's measurements.
2. `release.py`, the `gh-pages` layout, the DNS record (the owner adds it), a first test-channel release.
3. Master: look, install, API, page.
4. Rescue.
5. Flasher page.
6. The first release cut with the script.
