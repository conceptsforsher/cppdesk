# Step 14 — "source code", part 11 (RustDesk `d1013487`)

Source: [`rustdesk/rustdesk@d1013487`](https://github.com/rustdesk/rustdesk/commit/d1013487e2f3862e2f801ef1a704bb61fdff8bbb)
(AuthorDate 2021-03-29; parent `002fce13`.)

This is **part 11 of upstream commit d1013487**: the **IPC layer**. It covers
`src/ipc.rs` (464 lines) and its transport, the vendored
`libs/parity-tokio-ipc` (lib.rs + unix.rs). The desktop variants of
`common.rs`'s `get_rendezvous_server` / `get_nat_type` now ask the IPC daemon
as upstream does. The server `Connection` event loop, which talks to the
connection manager over this channel, comes next.

## What changed vs step 13

| Upstream item | C++ translation |
|---|---|
| `libs/parity-tokio-ipc` (vendored parity-tokio-ipc 0.7.2: lib.rs 154L + unix.rs 163L) | `libs/parity-tokio-ipc/` static library (`parity_tokio_ipc` namespace): `Endpoint` / `Incoming` / `Connection` / `SecurityAttributes` / `dummy_endpoint` over blocking `AF_UNIX` sockets (CLOEXEC like Rust std, `MSG_NOSIGNAL` like the Rust runtime's ignored SIGPIPE). `timeout(ms, Endpoint::connect)` becomes a connect bounded by `SO_SNDTIMEO`. MIT/Apache licenses copied verbatim. `win.rs` (named pipes) is deferred to the Windows platform step |
| `enum FS`, `enum Data` (`#[serde(tag = "t", content = "c")]`) | `ipc::FS`, `ipc::Data`: the variants are nested structs (`ipc::Data::Login`, `ipc::FS::ReadDir`, …) held in a `std::variant`. `to_json` is byte-identical to `serde_json::to_vec`. `from_json` applies serde's rules: unknown keys skipped; duplicate, missing or mistyped fields rejected; `"c"` may precede `"t"`; a missing `"c"` is `None` for `Option` payloads; integer range checks (`i32`/`i64`/`u64`/`u8`) |
| `serde_json` | `libs/hbb_common/{include/hbb_common/json.hpp,src/json.cpp}`: a minimal stand-in, the same approach as `toml.cpp` for confy. `Value` keeps serde_json's PosInt/NegInt/Float split (so `-0` and `1.0` are floats). The compact emitter uses serde_json escaping (lowercase `\u00XX`, raw UTF-8). The parser is strict, with recursion limit 128, and `is_valid_utf8` gives `std::str::from_utf8` parity |
| `Connection { inner: Framed<Conn, BytesCodec> }` | `ipc::Connection`: `send`, `next` (`Ok(None)` for undecodable frames becomes `nullopt`; EOF throws "reset by the peer"), `next_timeout` (throws `TimeoutError`, i.e. tokio `Elapsed`), `next_timeout2` (`nullopt` on expiry), and a persistent read buffer so a frame cut by a timeout completes later. Deadlines use `poll()` |
| `start`, `new_listener`, `handle` | A blocking accept loop with one detached thread per client (`tokio::spawn` parity). `handle()` is dispatched verbatim: `SystemInfo`, `Close` (`process::exit(0)` becomes `fflush` + `_Exit(0)`: no static destructors, like Rust), `OnlineStatus` (max ONLINE latency or 0, key confirmed), `ConfirmedKey(None)`, `Config` get/set (`id` / `password` / `salt` / `rendezvous_server`), `Options` get/set, `NatType`. All other variants get no reply |
| `get_pid_file`, `check_pid`, `write_pid` (psutil) | Process-name check via `/proc/<pid>/comm` (macOS `proc_name`), then the "double check with connect"; a stale socket is otherwise removed. The pid file is created like `File::create`, then `chmod 0777` |
| client helpers (`get_config[_async]`, `set_config`, `set_password`, `get_id`, `get_password`, `get_rendezvous_server`, `get_options[_]`, `get_option`, `set_option`, `set_options`, `get_nat_type[_]`) | Ported 1:1. `ResultType` becomes exceptions, and the `unwrap_or` fallbacks to local Config are kept where upstream has them. `"rendezvous_server"` travels as `SocketAddr` text, which needed the new hbb_common `to_string(ResolvedAddr)` (Display) and `parse_socket_addr` (FromStr, numeric only) |
| `common.rs` desktop `get_rendezvous_server(ms)` / `get_nat_type(ms)` | These now delegate to `ipc::` (the Android/iOS variants still read Config directly). `test_nat_type_` uses upstream's 100 ms lookup; the port passed 0, which was harmless only while the timeout was ignored |

## Transport bugs found and fixed in-step

Found while porting `Framed<Conn, BytesCodec>` for the IPC connection: the
TCP `FramedStream` shares the same framing semantics, and its C++ port broke
them. Each fix is pinned by a loopback test in `tests/test_tcp.cpp`, which
until now only checked that a listener binds.

1. **Lost and corrupted frames.** `recv_frame()` decoded every `recv()` from
   a fresh 4 KiB buffer. Several frames arriving in one `recv()` lost all but
   the first, and any frame larger than one read was corrupted, because the
   next `recv()` overwrote the buffered partial payload. It now keeps a
   persistent read buffer (tokio_util `Framed` parity). The tests cover a
   300 KiB frame, three frames in one write, a split header, and a partial
   frame across a timeout.
2. **`next_timeout()` left its timeout on blocking streams.** The restore
   step skipped saved value 0, so every later `next()` on such a stream timed
   out. It now restores exactly, receive side only.
3. **Connect and accept timeouts leaked into the stream.** The connect
   timeout stayed on the connected socket, and Linux copies the listener's
   accept timeout into the accepted socket, so later blocking I/O timed out.
   Both are now cleared (`timeout(ms, connect)` bounds the connect only).
4. **EOF and timeout both returned `nullopt`.** The new `peer_closed()`
   tells them apart. Upstream's `stream.next()` yields `None` only at EOF,
   and the `Connection` loop needs exactly that distinction.

## Upstream quirks kept verbatim (pinned by tests)

- **`incoming()` chmods before it binds.** parity-tokio-ipc's `incoming()`
  applies the security attributes *before* `bind()`. A `set_mode()` endpoint
  on a fresh path therefore fails with ENOENT. The crate's own `smoke_test`
  panics on exactly this as shipped.
- **The socket file outlives the listener.** That smoke test's last
  assertion expects the opposite, but the vendored crate has no `Drop`
  unlink. rustdesk only uses `allow_everyone_create()` (no chmod), and
  `check_pid` cleans stale sockets.
- **Some messages get no reply.** `Config((unknown, Some(_)))` and
  `ConfirmedKey(Some(_))` fall through to `_ => {}` silently.

## Tests

All new tests are hermetic: the config dir is a temp `HOME`, and
`custom-rendezvous-server = 127.0.0.1:1` keeps the daemon's `test_nat_type`
on loopback.

- **`test_json`.** serde_json number classes, escapes, strict grammar,
  recursion limit, UTF-8 rules.
- **`test_parity_tokio_ipc`.** The `smoke_test` echo flow with two clients,
  `SecurityAttributes`, the two quirks above, connect errors, EOF/shutdown,
  and EPIPE without SIGPIPE.
- **`test_ipc`:**
  - every `Data`/`FS` shape checked byte-exactly against serde_json output,
    plus a round trip;
  - serde decode rules;
  - framing over a socketpair (coalesced frames, 300 KB payload, partial
    frame across a timeout, undecodable frames, EOF/reset);
  - `handle()` through a live in-process server, including no-reply
    variants, multiple clients, and `check_pid` against a live listener;
  - `check_pid` / `write_pid` on stale state (missing or garbage pid file,
    pid 1, our own pid with and without a listener);
  - `Data::Close` making a re-exec'd child daemon exit with status 0;
  - the client helpers against a cross-process child daemon at the real
    postfix `""`. Daemon values win over local ones, `key_confirmed` resets
    on an id change, salt and password are adopted, options round-trip, and
    everything falls back to local values once the daemon is gone. This case
    is skipped if a live daemon already owns `/tmp/RustDesk/ipc`.
- **`test_common`, `test_client`, `test_mediator`** now also compile
  `src/ipc.cpp`, because `common.cpp` delegates to it.

## Build and test (GitHub Actions)

CI (`.github/workflows/ci.yml`) builds the root tree with ctest and builds
every protobuf-era snapshot. This snapshot is added to that matrix. The same
commands work locally:

```console
$ cmake -B build -S examples/14_d1013487_source_code_part11 -G Ninja -DCMAKE_BUILD_TYPE=Release
$ cmake --build build --parallel 3
$ ./build/cppdesk
cppdesk 1.1.2 step 14 (d1013487 part 11): + ipc — connection/ui/platform pending.
```

Next in `d1013487` parts: the `Connection` event loop plus the rest of
`server.rs` (`start_server`, `create_tcp_connection`,
`accept_connection`, relay), `port_forward.rs`, `platform/*`, the service
capture/playback bodies, `ui`, and the entry point (`main.rs` + `cli.rs`).
