# cppdesk — step-by-step C++ translation of RustDesk

Line-for-line C++20 translation of [rustdesk/rustdesk](https://github.com/rustdesk/rustdesk),
replayed commit-by-commit along upstream `master`, starting from the first
commit. Each upstream commit becomes one step here; frozen, standalone
snapshots live under `examples/NN_<short-hash>_<slug>/`.

Method mirrors the `stepbystep` branch of `progressive-server`
(one upstream commit ≈ one step + one snapshot).

## Status

| Step | Upstream commit | Date | Description | Snapshot |
|------|-----------------|------|-------------|----------|
| 01 | [`35b260e1`](https://github.com/rustdesk/rustdesk/commit/35b260e13a7b135f0a9844c27a05316eceeadbcd) | 2020-09-28 | Initial commit (README profile template, no code) | `examples/01_35b260e1_initial_commit/` |
| 02 | [`53495a72`](https://github.com/rustdesk/rustdesk/commit/53495a72e4c215277c192aa0e52522c28d3dc439) | 2020-09-28 | Update README.md (real tagline, still no code) | `examples/02_53495a72_update_readme/` |
| 03 | [`002fce13`](https://github.com/rustdesk/rustdesk/commit/002fce136c5e32e7c1c4b1cf21e834f4b220c0fa) | 2021-03-17 | funnding (adds .github/FUNDING.yml, still no code) | `examples/03_002fce13_funnding/` |
| 04 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 1: workspace + hbb_common core | `examples/04_d1013487_source_code_part1/` |
| 05 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 2: config + fs layer | `examples/05_d1013487_source_code_part2/` |
| 06 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 3: transport (tcp/udp) | `examples/06_d1013487_source_code_part3/` |
| 07 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 4: protobuf structs | `examples/07_d1013487_source_code_part4/` |
| 08 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 5: app common + Config methods | `examples/08_d1013487_source_code_part5/` |
| 09 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 6: fs file-transfer jobs | `examples/09_d1013487_source_code_part6/` |
| 10 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 7: rendezvous mediator | `examples/10_d1013487_source_code_part7/` |
| 11 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 8: client login layer | `examples/11_d1013487_source_code_part8/` |
| 12 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 9: client connection + crypto | `examples/12_d1013487_source_code_part9/` |
| 13 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 10: server foundation | `examples/13_d1013487_source_code_part10/` |
| 14 | [`d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb) | 2021-03-29 | source code, part 11: ipc (ipc.rs + parity-tokio-ipc) | `examples/14_d1013487_source_code_part11/` |

One upstream commit may span several steps when it is too large for a single
accurate port (here `d1013487`: 175 files). Parts are numbered in the step
subject until the commit is fully translated.

Next in upstream `master` order: `d1013487` parts 12+ (Connection loop +
rest of server.rs, port_forward, platform/ui, entry), then `f43f5df9`.

Full per-step log: [examples/INDEX.md](examples/INDEX.md).

## Building (root or any snapshot)

GitHub Actions is the build environment
([`.github/workflows/ci.yml`](.github/workflows/ci.yml)). Every push to
`stepbystep` or `wip/**`, and every pull request, builds the root tree and
runs ctest, and also builds each protobuf-era snapshot standalone. Agents do
not build on the development machine (see [AGENTS.md](AGENTS.md)).

Dependencies (Ubuntu): `cmake ninja-build g++ libprotobuf-dev protobuf-compiler
libsodium-dev libzstd-dev libssl-dev`. CI runs:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
./build/cppdesk
```

Each `examples/NN_*/` snapshot builds standalone the same way:

```bash
cmake -B build-14 -S examples/14_d1013487_source_code_part11 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-14 --parallel 3
```

## Layout

```
AGENTS.md                   — agent instructions (CI-only builds; RAM-based -j rule)
CMakeLists.txt              — live build (current step state)
cmake/version.hpp.in        — version template (build.rs gen_version() parity)
LICENSE                     — upstream license, verbatim
libs/hbb_common/
  include/hbb_common/       — ported module headers (addr_mangle, bytes_codec, compress, util,
                              toml, json, config, tcp, udp, fs, crypto)
  src/                      — ported module sources
  protos/                   — upstream .proto files (+ C++ scoping fixes, see step 07)
  protos/gen/               — checked-in protoc output (fallback when protoc is missing;
                              CI generates at build time)
libs/parity-tokio-ipc/      — vendored IPC transport (lib.rs + unix.rs; step 14, MIT/Apache-2.0)
src/main.cpp                — live placeholder binary (tracks HEAD step)
src/common.cpp              — app entry-layer impl (NAT/rendezvous/update tasks, step 08)
src/ipc.cpp                 — src/ipc.rs impl (IPC daemon, framing, client helpers, step 14)
src/platform/windows.cc     — upstream Win32 helpers, verbatim (WIN32-only build)
src/tray-icon.ico           — upstream icon, verbatim
include/rustdesk/
  common.hpp                — app entry-layer header (src/common.rs parity, step 08)
  ipc.hpp                   — src/ipc.rs: Data/FS, framed IPC connection, daemon + client
                              helpers (step 14)
tests/                      — CTest parity for upstream #[cfg(test)] modules, plus
                              coverage for modules upstream leaves untested
examples/
  INDEX.md                  — step index (upstream hash, date, subject)
  01_35b260e1_initial_commit/
    CMakeLists.txt          — frozen standalone build for step 01
    README.md               — what upstream changed vs previous step + C++ mapping
    src/main.cpp            — frozen code for step 01
  02_53495a72_update_readme/
    ...                     — same frozen shape for step 02
  03_002fce13_funnding/
    ... + .github/FUNDING.yml — same frozen shape for step 03
  04_d1013487_source_code_part1/
    ... + libs/ tests/ cmake/ LICENSE — full frozen workspace for step 04
  05_d1013487_source_code_part2/
    ... + libs/ tests/ cmake/ LICENSE — full frozen workspace for step 05
  06_d1013487_source_code_part3/
    ... + libs/ tests/ cmake/ LICENSE — full frozen workspace for step 06
  07_d1013487_source_code_part4/
    ... + libs/ tests/ cmake/ LICENSE — full frozen workspace for step 07
  08_d1013487_source_code_part5/
    ... + libs/ src/ include/ tests/ cmake/ LICENSE — full frozen workspace for step 08
  09_d1013487_source_code_part6/
    ... + libs/ src/ include/ tests/ cmake/ LICENSE — full frozen workspace for step 09
  10_d1013487_source_code_part7/
    ... + libs/ src/ include/ tests/ cmake/ LICENSE — full frozen workspace for step 10
  11_d1013487_source_code_part8/
    ... + libs/ src/ include/ tests/ cmake/ LICENSE — full frozen workspace for step 11
  12_d1013487_source_code_part9/
    ... + libs/ src/ include/ tests/ cmake/ LICENSE — full frozen workspace for step 12
  13_d1013487_source_code_part10/
    ... + libs/ src/ include/ tests/ cmake/ LICENSE — full frozen workspace for step 13
  14_d1013487_source_code_part11/
    ... + libs/ src/ include/ tests/ cmake/ LICENSE — full frozen workspace for step 14
```

Rule: the repo root always reflects the latest translated step; `examples/`
copies are frozen and never modified after their step lands.
