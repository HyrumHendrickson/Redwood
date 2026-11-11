# AGENTS.md

Instructions for AI coding agents and human contributors working in this repository.

## Project overview

Redwood is a cryptocurrency consensus testbed written in C11 (GNU extensions) with a GTK 3 front end. It simulates networks of nodes running Proof of Work (Sequoia), Proof of Authority (Cedar) or Proof of Stake (Aspen), including configurable cheating behaviours.

| File | Role |
| --- | --- |
| `sha256.c`, `sha256.h` | SHA-256 implementation, including midstate compression for mining |
| `redwood.h` | Public types, limits and the simulation API |
| `sim.c` | Simulation core. No GUI dependencies |
| `cli.c` | Headless runner (`redwood-cli`) |
| `gui.c` | GTK 3 interface (`redwood`) |
| `tests/run_tests.sh` | Regression tests driving `redwood-cli` |

## Commands

```
make                 # build ./redwood (needs GTK 3) and ./redwood-cli
make redwood-cli     # build the headless binary only
make test            # run regression tests
make check           # run regression tests under ASan and UBSan
make clean
```

Run `make test` and `make check` before proposing any change. Both need only a C compiler and a POSIX shell.

## Architecture rules

- `sim.c`, `sha256.c` and `redwood.h` must not include GTK or any GUI header. The core must stay buildable with `make redwood-cli`.
- The simulation is deterministic: the same seed and settings must produce identical output. Use the simulation RNG (`s->rng`) only. Never use `rand()`, `time()`, pointers, or uninitialised memory to influence behaviour.
- Block validity is a pure function of the block and its parent, computed once and cached. Do not add hidden state to validation.
- Supply must equal initial balances plus rewards times height on the best chain. The CLI checks this and the tests enforce it.
- Respect the compile-time limits in `redwood.h` (`MAX_NODES`, `MAX_BLOCKS`, `MAX_TXS`, `WHEEL`, and so on). Any new array indexed by these values needs a bounds check.
- The "signature" scheme is a simulation oracle, not real cryptography. Do not describe it, or present it, as secure.

## Code style

- C11 with GNU extensions (`-std=gnu11`). Code must compile warning-free with `-Wall -Wextra`.
- Four-space indentation, no tabs in C files, Linux brace style (see `.clang-format`).
- Allocate through the `xcalloc` and `xrealloc` wrappers in `sim.c`. Do not call `calloc` or `realloc` directly there.
- Validate all external input (command-line arguments, GUI fields) before use. Prefer `strtol`/`strtoull` with full error checks over `atoi`.
- Prefer `snprintf` with `sizeof` over `sprintf`. Do not use `strcpy`, `strcat` or `gets`.
- Keep functions small and comment the reason for non-obvious logic, not the mechanics.

## Testing expectations

- Every bug fix adds a regression test in `tests/run_tests.sh`.
- New cheat behaviours, coins or CLI options need tests covering the supply invariant and determinism.
- Do not weaken or delete a test to make a change pass. If a test is wrong, explain why in the pull request.

## Security rules

- Never commit secrets, keys, tokens or credentials.
- Do not add network access, file writes outside the working directory, or dynamic code loading without prior discussion in an issue.
- Do not add third-party dependencies without prior discussion. The project currently depends only on libc, libm and (for the GUI) GTK 3.
- Report vulnerabilities as described in `SECURITY.md`, not in public issues.

## Pull request checklist

1. `make test` and `make check` pass.
2. No new compiler warnings.
3. `README.md` and `CHANGELOG.md` are updated for user-visible changes.
4. Commits are small, with imperative-mood subject lines (for example, "Reject negative tick counts").
5. The change does not alter simulation output for existing seeds unless that is its stated purpose.
