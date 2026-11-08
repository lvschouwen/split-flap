---
paths:
  - "firmware/v2/buildtools/**"
  - "firmware/v2/*/platformio.ini"
  - "firmware/v2/*/build_assets.py"
---
# firmware/v2/buildtools — python shared by every tree's build

- `fwbuild.py`: Intel-HEX → image, rev stamping, PROGMEM arrays; imported by each `build_assets.py`, `make_manifest.py` and the twiboot builder. The one `patch_asyncweb.py`.
- `check_framework_libs.py` (#634): pioarduino compiles one shared Arduino IDF lib build per chip and never reads it back, so Master and Rescue keep the same `custom_sdkconfig` and this pre-script recompiles libs that lack it.
- Deliberately outside `shared/`: a build-script edit must not move the Unit source head. Tests: `python3 -m pytest tests/` here.
- **Pins (#648):** every platform, library and pip package is an exact version (no `^`/unpinned). To bump, read the new version from `pio pkg list -e <env>` / `pip index versions <pkg>`, edit the pin in the `platformio.ini` or `scripts/check.sh`, and let `scripts/check.sh` be the gate.
- The pioarduino platform needs PlatformIO Core ≥ 6.2.0. A first v2 build on a clean machine is slow (the hybrid compile downloads IDF). `managed_components/`, `sdkconfig.*`, `.dummy/` in v2 project dirs are generated (gitignored; `Bootloader/sdkconfig.defaults` is a source file kept by a negation).
