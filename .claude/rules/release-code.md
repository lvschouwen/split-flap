---
paths:
  - "firmware/v2/release/**"
---
# firmware/v2/release — where a board looks for a release and whose signature it trusts (#583)

- `ReleaseSource.h`: site name + the release key's public half (a fork changes both). `ReleaseManifest.h`: what a manifest says and what a board refuses.
- `ReleaseFetch.h`: the order of look and install over hooks — signature before the manifest is read, size and SHA-256 before anything is made the next to run. `ReleaseTarget.h`: those hooks on the S3 (HTTPS, the signature check).
- `RescueSourceHead.h` (#613): the marker the rescue image embeds and the master scans the factory slot for — the rescue source head, `fwbuild.py`'s rev of the last commit touching Rescue/release/shared/buildtools, stamped into every `BuildVersion.h`.
- Compiled into Master and Rescue; deliberately outside `shared/` (no unit compiles it). Tests: Master's `test_release_manifest` / `test_release_fetch`. Design: `docs/superpowers/specs/2026-10-10-release-update-design.md`.
