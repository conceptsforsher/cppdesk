cmake_minimum_required(VERSION 3.20)
# Mirrors the matching test executables' sources 1:1 (see ../test_*.cpp).
# New tests for newly ported modules get their own executable + add_test here.

function(cppdesk_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE hbb_common)
    if (name STREQUAL "test_client" OR name STREQUAL "test_mediator")
        add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    else()
        add_test(NAME ${name} COMMAND ${name})
    endif()
endfunction()

# libs/hbb_common/src/bytes_codec.rs #[cfg(test)] parity (test_codec1..6).
cppdesk_test(test_bytes_codec)
# libs/hbb_common/src/lib.rs test_mangle parity.
cppdesk_test(test_addr_mangle)
# compress round-trip (upstream has no compress test; guards the zstd wiring).
cppdesk_test(test_compress)
# toml parser/emitter round-trip (guards confy parity).
cppdesk_test(test_toml)
# Config/Config2/PeerConfig TOML round-trip (upstream test_serialize).
cppdesk_test(test_config)
# TCP transport: framing, listener, raw mode.
cppdesk_test(test_tcp)
# UDP transport: bind, bind_reuse.
cppdesk_test(test_udp)
# protoc-generated structs: wire round-trips (upstream has no proto tests).
cppdesk_test(test_protos)
# Tests of app modules link the rustdesk library (src/lib.rs parity, step
# 15) rather than recompiling app sources per test.
function(cppdesk_app_test name)
    cppdesk_test(${name})
    target_link_libraries(${name} PRIVATE rustdesk)
endfunction()

# src/common.rs pure helpers (key predicates, version/port strings).
cppdesk_app_test(test_common)
# fs.rs file-transfer jobs (read_dir, recursive enum, block round-trip).
cppdesk_test(test_fs)
# crypto seam round-trips (upstream has no crypto unit tests).
cppdesk_test(test_crypto)
# server foundation: service framework, ConnInner queue, cursor cache,
# Server registry, zombie reaper; video_service state and the input_service
# predicates (step 15).
cppdesk_app_test(test_server)
# client.rs login config + handshake helpers (hermetic HOME redirect +
# loopback TCP).
cppdesk_app_test(test_client)
# rendezvous_mediator.rs helpers (prefix, uuid, wire shapes).
cppdesk_app_test(test_mediator)
# serde_json stand-in (hbb_common/json.hpp): serde_json-exact output, strict
# parsing, UTF-8 validation (upstream has no JSON tests; ipc.rs relies on it).
cppdesk_test(test_json)
# tokio::sync::mpsc stand-in (hbb_common/mpsc.hpp): unbounded channel drop
# semantics and the wake descriptor of the poll() loops (step 15).
cppdesk_test(test_mpsc)
# libs/parity-tokio-ipc: lib.rs smoke_test parity (echo over a Unix socket,
# two clients) + the vendored unix.rs quirks (chmod-before-bind, no unlink).
cppdesk_test(test_parity_tokio_ipc)
target_link_libraries(test_parity_tokio_ipc PRIVATE parity_tokio_ipc)
# src/ipc.rs: Data/FS serde shapes, framing, the ipc server's handle()
# (in-process and as a re-exec'd child process for postfix "" and Close),
# check_pid/write_pid, and the client helpers against a live daemon.
# Hermetic: HOME/XDG_CONFIG_HOME point at a temp dir; skips the postfix ""
# cases when a live IPC daemon already owns /tmp/RustDesk/ipc.
cppdesk_app_test(test_ipc)
# std::process / whoami stand-ins and platform/linux.rs part 1: the seat0
# session queries run against a fake `loginctl` first on PATH (hermetic).
cppdesk_app_test(test_platform)
# server.rs + server/connection.rs: a real client (Client::secure_connection,
# LoginConfigHandler) against accept_connection / create_relay_connection over
# loopback, a fake connection manager on the "_cm" ipc socket (in-process,
# and as a re-exec'd `--cm` child for start_ipc's spawn path), port
# forwarding to a local echo server, and the login-failure limits.
cppdesk_app_test(test_connection)
