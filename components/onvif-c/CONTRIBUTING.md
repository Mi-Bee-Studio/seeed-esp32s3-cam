# Contributing

## TDD workflow (required)

1. **Red** — write or extend the host test first (`tests/test_core.c` for
   core builders/goldens, `tests/test_service.c` / `test_events.c` /
   `test_discovery.c` for the port layer; stubs live in `tests/host_stubs/`).
2. **Green** — make the library change. Response-byte changes additionally
   need a justification stronger than cosmetics: NVR integrators may match
   raw substrings (byte stability guarantee), so golden diffs are behavior
   changes and get called out in the CHANGELOG.
3. **Gates** — all four must pass locally and in CI:
   - `tests/run.sh` — host suites (needs only a C compiler + pthreads)
   - `tests/coverage.sh` — line coverage of `core/` + `esp_idf/` stays
     ≥80%; new logic ships with tests or waits
   - `tools/check_style.sh` — clang-format clean; `tools/format.sh` fixes
     (pin `clang-format==22.1.8` to match CI exactly)
   - `tools/check-repo-hygiene.sh`

## Setup

- `tools/setup-hooks.sh` — installs the pre-commit hook (hygiene + style +
  host tests) once per clone.
- Firmware integration is validated in the four MiBee Cam repos
  (`family_check.sh` pins the vendored tree across boards): after changing
  this repo, re-vendor into all four and update their trees in the same
  change — never let the copies drift.
- `main` is protected: PR-only merges, CI required.
