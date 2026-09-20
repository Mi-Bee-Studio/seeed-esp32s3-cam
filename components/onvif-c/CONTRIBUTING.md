# Contributing

TDD first: any behavior change to response bytes must come with a golden
test update in `tests/test_core.c`, and byte changes need a justification
stronger than cosmetics — NVR integrators may match raw substrings.

- `tests/run.sh` must stay green (zero dependencies beyond a C compiler).
- Firmware integration is validated in the four MiBee Cam repos
  (`family_check.sh` pins the vendored tree across boards).
- `main` is protected: PR-only merges, CI required.
