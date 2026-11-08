# Vendored eCat3

Upstream: https://github.com/Ptr314/ecat3 (GPL-3.0), commit `4c32b5faabea07cfd91f9dd76269ab456303b52e` (2026-11-06),
with the `src/libs/dsk_tools` submodule (https://github.com/Ptr314/dsk_tools) at `08f9f120627fe0efcf4e4dc70cb625949188a7ac`.

Copied: `src/` (without `pics/`, `translations/`, `wasm/`, which only the Qt and browser builds use),
`deploy/`, `tests/` (without `results/`), `LICENSE`, `VERSION`, `README.md`, `CLAUDE.md`.

Every frank-ecat change to these files is guarded by `ECAT_EMBEDDED` and listed in
[PATCHES.md](PATCHES.md), so an upstream update is a copy plus re-applying that list.

Host check: the stock console build of this tree runs the upstream test suite
(`tests/run_tests.py`): see the top-level README.
