# Project Context

## Purpose
This repository is **XS-GEM5** (Xiangshan GEM5): a specialized fork of the
gem5 simulator enhanced for the Xiangshan RISC-V processor. It focuses on
full-system (FS) simulation workflows and Xiangshan-specific checkpoint and
verification formats, with microarchitecture models calibrated against
Xiangshan V3 (Kunminghu).

## Tech Stack
- **Languages**: C++ (core simulator), Python 3 (configs/build tooling/tests), Bash/Shell (scripts).
- **Build system**: SCons (`SConstruct`), outputs under `build/<ISA>/`.
- **Testing**: GoogleTest (unit tests), Python-based system tests (`tests/main.py`).
- **Formatting/linting (pre-commit)**:
  - C++: `clang-format` via `.clang-format`
  - Python: `black` configured in `pyproject.toml`
  - Misc: trailing whitespace, EOF fixer, JSON/YAML checks, LF line endings
- **Docs**: MkDocs Material (`mkdocs.yml`) and ReadTheDocs configuration (`.readthedocs.yaml`).
- **Dev environments**: Nix flake (`nix/flake.nix`) and direnv (`.envrc`).
- **Primary dev OS**: Ubuntu (commonly 20.04/22.04; dependency list is documented in `README.md`).

## Project Conventions

### Code Style
- **C++ formatting**: follow `.clang-format` (BasedOnStyle: Mozilla, `IndentWidth: 4`,
  `ColumnLimit: 119`, spaces not tabs).
- **Python formatting**: `black` with `line-length = 79` (`pyproject.toml`).
- **Line endings**: LF is enforced by pre-commit (`mixed-line-ending --fix=lf`).
- **Encoding**: UTF-8 (no BOM) for all text/code files.
- **Naming & style guidance**: when in doubt, follow upstream gem5 coding style guidelines;
  keep changes minimal and consistent with surrounding code.

### Architecture Patterns
- **FS-first workflow**: this fork is designed around full-system simulation for Xiangshan;
  typical inputs are Xiangshan-specific baremetal images or checkpoints.
- **Config in Python, core in C++**: hardware/platform configurations live under `configs/`,
  while simulation components live under `src/`.
- **Build targets**: build artifacts are organized by ISA and variant,
  e.g. `build/RISCV/gem5.opt`.
- **Third-party code**: external dependencies are vendored under `ext/` and are excluded
  from pre-commit checks.

### Testing Strategy
- **Unit tests (GoogleTest)**: build and run via SCons, e.g. `scons build/NULL/unittests.opt`.
- **System-level tests**: run from `tests/` with `./main.py run`; use tags to scope
  (`--isa`, `--variant`, `--length`) and `rerun` to re-run only failed suites.
- **Expectation**: run at least targeted unit/system tests for the modified area before
  sharing patches.

### Git Workflow
- **Hosting**: GitHub-based workflow (PRs/issues); upstream gem5 Gerrit flow in `CONTRIBUTING.md`
  may not reflect this fork’s day-to-day process.
- **Branches (repo-specific)**: `xs-dev` is periodically synced; `backport` is used for
  backporting functional correctness/basic usage fixes.
- **Commit message convention (upstream gem5)**: keep a short, imperative summary line
  prefixed by relevant component keywords (see `CONTRIBUTING.md`).
- **Incremental commits**: commit early and often while developing (small, reviewable units);
  before opening/merging a PR, use `git rebase -i` (squash/fixup/reword) to present a clean
  history with an appropriate number of commits.
- **Staging discipline**: avoid staging unrelated files; prefer `git add -p` or explicit
  paths (do not use blanket staging).
- **No direct deletions**: a pre-commit hook blocks `git rm`/`D` outside `.recycle_bin/`.
  Move files to `.recycle_bin/` with `git mv` to “delete” them.
- **Pre-commit**: hooks are configured in `.pre-commit-config.yaml` (recommended:
  `pre-commit install`).

### Build & Run
- **Build (RISC-V, common)**: `scons build/RISCV/gem5.opt --gold-linker -j$(nproc)` (see `README.md`).
- **Run (single binary / raw-cpt)**: `./build/RISCV/gem5.opt configs/example/xiangshan.py --raw-cpt --generic-rv-cpt=<bin>`
  (see `README.md`).
- **Run (checkpoint / batch scripts)**: prefer `util/xs_scripts/kmh_6wide.sh <checkpoint>` and
  `util/xs_scripts/parallel_sim.sh ...` (see `README.md` and `docs/Gem5_Docs/top/run_checkpoint.md`).
- **Trace-driven simulation**: see `src/cpu/o3/trace/README.md`, `src/cpu/o3/trace/TRACE_USAGE.md`,
  and `docs/tools/trace/trace_tools.md` (scripts under `util/xs_scripts/trace/`).

### Local Dev Notes
- **Primary dev OS**: Ubuntu.
- **Trace regressions**: use external NFS traces (do not commit trace data); prefer running via
  `util/xs_scripts/trace/run_trace_champsim.sh` with per-run `OUTDIR` so `stats.txt` can be diffed.

#### Trace Regression Matrix (Project)
- **Trace roots (NFS)**:
  - ChampSim: `/nfs/home/share/glr/champsim_traces`
  - CBP2025: `/nfs/home/share/glr/cbp_traces`
- **Selection rule**: pick **1-2 representative traces per category** (e.g. `srv`, `compute_int`, `compute_fp`, `crypto` for ChampSim; `web`, `int`, `fp`, `infra`, `media`, `compress` for CBP2025). Do **not** use checkpoints for trace regression.
- **Tiered run lengths** (KISS, catch issues early):
  - Tier 0 (smoke): `XS_MAX_INSTS=10000` for each selected trace.
  - Tier 1 (diff-friendly): `XS_MAX_INSTS=50000` for each selected trace; compare stats (recommended pre-PR).
  - Tier 2 (deeper): `XS_MAX_INSTS=200000` for a smaller subset (e.g. `web`, `compute_int`) to catch longer-window issues.
  - Tier 3 (optional): `XS_MAX_INSTS=1000000` for 1-2 “most representative” traces before merge.
- **Comparison method**: diff `stats.txt` while ignoring host-only variability:
  - `diff -u <(rg -v "^(hostSeconds|hostTickRate|hostMemory|hostInstRate|hostOpRate)\\b" baseline/stats.txt) <(rg -v "^(hostSeconds|hostTickRate|hostMemory|hostInstRate|hostOpRate)\\b" new/stats.txt)`
- **Deprecated flags**: do not include `--trace-disable-bp-validation` or `--trace-disable-wrongpath` in regression matrices (these are being deprecated).

## Domain Context
- **Xiangshan RVGCpt**: Xiangshan full-system checkpoint format used for faster detailed
  simulation runs.
- **Difftest**: an online execution result checking mechanism used in Xiangshan workflows.
- **Calibration targets**: Xiangshan V3 (Kunminghu) microarchitecture and SPEC CPU 2006
  checkpoint-based performance studies.

## Important Constraints
- **Full-system only**: syscall emulation (SE) is not supported in this fork; it cannot
  “run an ELF directly” like SE-based flows.
- **Checkpoint compatibility**: Xiangshan checkpoints are not compatible with vanilla gem5
  SE or m5 checkpoints.
- **Dev environment constraints**: Nix devshell pins Python 3.10 due to pybind constraints
  (`nix/flake.nix`); follow project docs if you change Python/tooling versions.
- **Local rules**: do not bypass hooks/tests (e.g., avoid `--no-verify`), do not use `sudo`,
  and avoid broad, unrelated refactors.

## External Dependencies
- **DRAMSim3**: optional memory model under `ext/dramsim3/` (see `init.sh` and
  `ext/dramsim3/README`).
- **System dependencies (typical)**: SCons, C/C++ toolchain, Boost, zlib, zstd, protobuf,
  sqlite3, gperftools/tcmalloc (see `README.md` / `nix/flake.nix`).
- **Workload/checkpoint toolchain (external)**: NEMU and Xiangshan docs are commonly used
  to produce FS images and checkpoints referenced by this repo’s workflows.
