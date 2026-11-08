---
paths:
  - "firmware/v2/link/**"
---
# firmware/v2/link — the master↔row-board link (#559)

- `wall_link.proto` + `.options`: every message, one envelope per direction. nanopb generates the C at build time via each tree's `custom_nanopb_protos`; `buildtools/ensure_nanopb_deps.py` installs the generator's Python packages where the toolchain has no pip.
- `WallLinkStream.h` collects length-delimited messages from a polled socket (unit facts as JSON in pieces). `WallLinkLogDamp.h`: a link that keeps dropping is logged a few times per period, then counted (both boards).
- `WallLinkNames.h` (#611): the hostname a board carries from its grid place, `split-flap-row<N>` from 1 and `-c<C>` only when boards share a grid row; the master derives it, the row checks it. A board's id `split-flap-<chip id>` stays its identity.
- Deliberately outside `shared/`: no unit compiles it, so an edit must not move the Unit source head.
- Tests: `pio test -e native -f test_wall_link` in Master; `python3 -m pytest tests/` here (needs `grpcio-tools`; `scripts/check.sh` has it) has stock protobuf decode the bytes the native test pins. `fake_master.py` / `fake_row.py` are the bench stand-ins for either end, run against each other by `tests/test_fake_pair.py`.
