# run_ut.sh Usage Guide

## Mode Comparison

| Feature | Default mode | `--fast` mode |
|---|---|---|
| Build directory cleanup | Full wipe before build | Skipped (incremental) |
| cmake reconfiguration | Always | Only on first run or fingerprint mismatch |
| Coverage report (lcov/genhtml) | Generated | Skipped |
| Typical use case | CI / pre-commit | Local development iteration |

## Usage

```bash
# Default: full clean build + coverage report
bash script/run_ut.sh

# Default with test filter
bash script/run_ut.sh SmemBmTest

# Fast: incremental build, no coverage
bash script/run_ut.sh --fast

# Fast with test filter
bash script/run_ut.sh --fast SmemBmTest

# aarch64 local: DEBUG build (ASAN unstable on aarch64), coverage gate still active
MF_UT_BUILD_TYPE=DEBUG bash script/run_ut.sh --fast
```

## Environment Variables

| Variable | Default | Description |
|---|---|---|
| `MF_UT_BUILD_TYPE` | `ASAN` | UT build type (`CMAKE_BUILD_TYPE`). Override to `DEBUG` on platforms where ASAN is unstable (see [Platform Notes](#platform-notes-aarch64--devcontainer)). |
| `MF_BUILD_JOBS` | 60% of `nproc` | Parallel build job count. |

## `--fast` Fingerprint Mechanism

The `--fast` mode writes a fingerprint string (`${MF_UT_BUILD_TYPE}-UT-OPEN_ABI`,
e.g. `ASAN-UT-OPEN_ABI` by default) to `build/.build_fingerprint` after the first
successful cmake configuration.
On subsequent runs it checks:

1. **Build directory missing** or **fingerprint file missing** → full build
2. **Fingerprint mismatch** (build config changed externally) → full rebuild
3. **Fingerprint matches** → incremental build (skip cmake, skip rm)

The three cases print:

```text
========= first build, full build ============
========= build config changed, full rebuild ============
========= incremental build ============
```

## Recommended Workflow

- During active development, use `bash script/run_ut.sh --fast` for fast
  iteration (recompiles only changed translation units).
- Before committing or pushing, run `bash script/run_ut.sh` to get the full
  clean build and coverage report required by CI.

## Platform Notes (aarch64 / devcontainer)

### aarch64: ASAN is unavailable

On aarch64 (including Ubuntu 22.04), GCC ASAN conflicts with mockcpp's
`JmpCode` function-hooking mechanism:

- `detect_stack_use_after_return=1` → false `stack-use-after-return` reports
  (`ApiHookKey` / `Invocation` paths in mockcpp).
- `detect_stack_use_after_return=0` → SEGV in ASAN stack instrumentation at
  function entry.

The default ASAN build therefore cannot complete on aarch64. For local
iteration use `DEBUG` — it keeps the gcov coverage gate (lines ≥70%,
branches ≥40%); the sanitizer gate is left to CI on x86_64:

```bash
MF_UT_BUILD_TYPE=DEBUG bash script/run_ut.sh --fast
```

### devcontainer: libjemalloc preload

The devcontainer (`vllm-ascend` base image) exports
`LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2`, which conflicts with
ASAN's malloc interceptor. `run_ut.sh` does `unset LD_PRELOAD` before running
the test binary to avoid this — **do not remove that line**. The root-cause
fix (blanking `LD_PRELOAD` in `.devcontainer/devcontainer.json` `containerEnv`)
is a separate follow-up.

### AccLinksTest port 8100

`AccLinksTest` hardcodes `LISTEN_PORT=8100`
(`test/ut/testcase/acc_links/acc_links_test.cpp:37`). If the devcontainer host
(or another process in the same network namespace) occupies port 8100, all 54
cases in this suite fail at `SetUp` with `bind` `EADDRINUSE` (the listener only
sets `SO_REUSEADDR`, which cannot reclaim an active listener). Free the port or
run in a network namespace where it is free.
