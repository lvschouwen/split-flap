---
paths:
  - "flashing/release.py"
  - "flashing/site/**"
---
# Release policy (CalVer `vYYYY.MM.DD`, ≤1/day) — read before tagging

The fleet converges on the git REV (`git describe`/short SHA baked into the binary), never on the tag — a tag is a human changelog + rollback anchor only. There is no version file: a release = an annotated git tag + a GitHub release with notes (`flashing/release.py`).

- At most one release per day — the date is the version. Fix-only days accumulate untagged on `master`; cut a dated release only when the day's diff is worth announcing or anchoring. Never tag inside a feature/fix commit.
- CalVer has no MAJOR signal, so flag breaking changes: title `vYYYY.MM.DD — BREAKING` plus a `## Breaking / Operator action required` section, for any change to the I2C/cluster wire, NVS/EEPROM layout, partition table, OTA contract, config semantics, or minimum bootloader.
- Same-day critical fix after a release: OTA-deploy the fixed REV now and cut the dated release the next day. `vYYYY.MM.DD-hotfix.N` is break-glass only.
- Frozen v1 tags (`v1.x`) stay semver; mixed tags don't version-sort, so always pass `gh release create --latest`. Never parse meaning from the tag, and never retag/rewrite a shipped release.
