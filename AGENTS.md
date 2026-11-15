# AGENTS.md — cppdesk agent instructions

## Builds run in GitHub Actions only (mandatory)

Do not build or test on the development machine: no cmake/ninja/make, no
compiler invocations (not even `-fsyntax-only`), no ctest. CI
(`.github/workflows/ci.yml`) is the build environment. It builds the root
tree, runs ctest, and builds every protobuf-era snapshot standalone.

Workflow for a step:

1. Commit work in progress on a `wip/<topic>` branch and push it. Pushes to
   `wip/**` trigger CI.
2. Read the result: `gh run list -R conceptsforsher/cppdesk --branch wip/<topic>`,
   then `gh run view <run-id> -R conceptsforsher/cppdesk --log-failed` (or
   `--job <id> --log` to check test output and warnings on a green run).
3. Iterate until green. Then land one clean, squashed commit per step on
   `stepbystep` (`feat: add step NN (...)`); CI runs again there.

Every compile check is a CI round trip, so review new code carefully before
pushing.

The RAM-gated `-j` rule below applies only when the user explicitly asks for
a local build.

## Build parallelism — RAM-based `-j` rule (local builds only)

Low-RAM environment. NEVER use plain `-j$(nproc)` or unbounded `--parallel`.
Always gate parallelism on **available** RAM (`MemAvailable`).

| Available RAM | Max jobs |
|---------------|----------|
| < 1.5 GB      | WAIT — do not compile, sleep and re-check in a loop |
| >= 1.5 GB and < 3 GB | `-j1` |
| >= 3 GB and < 4.5 GB | `-j2` |
| >= 4.5 GB     | `-j3` (never higher, even if more cores exist) |

### How to check (Linux)

```bash
# GB with two decimals, from MemAvailable in /proc/meminfo
awk '/MemAvailable/ {printf "%.2f\n", $2/1024/1024}' /proc/meminfo
# or: free -m | awk '/Mem:/ {print $7}'
```

### Required pattern before any `cmake --build` / `make` / `ninja`

```bash
# Wait until at least 1.5 GB is available, then pick -j1/-j2/-j3.
while true; do
  AVAIL_MB=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
  echo "Available RAM: ${AVAIL_MB} MB"
  if [ "$AVAIL_MB" -ge 4608 ]; then JOBS=3; break;
  elif [ "$AVAIL_MB" -ge 3072 ]; then JOBS=2; break;
  elif [ "$AVAIL_MB" -ge 1536 ]; then JOBS=1; break;
  else echo "Less than 1.5GB available, sleeping 60s..."; sleep 60;
  fi
done
echo "Building with -j${JOBS}"
cmake --build build --parallel "$JOBS"
# plain `make`: `make -j"$JOBS"`; plain `ninja`: `ninja -j"$JOBS"`
```

Rules:
- Re-check available RAM right before each build invocation (it changes over time).
- If available RAM drops below 1.5 GB mid-work, stop starting new compiles; wait (`sleep 60` loop) until it recovers.
- Never exceed `-j3`.