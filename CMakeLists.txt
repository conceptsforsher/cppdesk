cmake_minimum_required(VERSION 3.20)
# Version parity: Cargo.toml [package] version = "1.1.2".
project(cppdesk VERSION 1.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# build.rs gen_version() parity: version string available to code as a header.
configure_file(
    cmake/version.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/cppdesk/version.hpp"
    @ONLY
)

# Protobuf: prefer the package config. protobuf >= 22 links Abseil, whose
# transitive DSOs must appear on the link line — a raw find_library() link
# fails with "DSO missing from command line" (absl::log_internal_check_op).
# The interface target below is inherited by libs/hbb_common.
find_package(Protobuf CONFIG QUIET)
add_library(cppdesk_protobuf INTERFACE)
if(TARGET protobuf::libprotobuf)
    target_link_libraries(cppdesk_protobuf INTERFACE protobuf::libprotobuf)
else()
    find_path(PROTOBUF_INCLUDE_DIR NAMES google/protobuf/message.h)
    find_library(PROTOBUF_LIBRARY NAMES protobuf)
    if(NOT PROTOBUF_INCLUDE_DIR OR NOT PROTOBUF_LIBRARY)
        message(FATAL_ERROR "libprotobuf dev files not found (need message.h + libprotobuf)")
    endif()
    target_include_directories(cppdesk_protobuf INTERFACE ${PROTOBUF_INCLUDE_DIR})
    target_link_libraries(cppdesk_protobuf INTERFACE ${PROTOBUF_LIBRARY})
endif()

# Workspace parity: Cargo.toml [workspace] members scrap/hbb_common/enigo.
# Only hbb_common exists yet (part 1 of d1013487); scrap/enigo arrive as
# native C++ modules in later steps, mirroring the old cppdesk libs/ layout.
add_subdirectory(libs/hbb_common)
# Path dependency of the app crate (Cargo.toml: parity-tokio-ipc = { path =
# "libs/parity-tokio-ipc" }), the transport under src/ipc.rs. Step 14.
add_subdirectory(libs/parity-tokio-ipc)

# OpenSSL libcrypto (sha2 crate parity for the login handshake hashes).
# System package, offline-safe lookup like zstd/protobuf.
find_path(OPENSSL_INCLUDE_DIR NAMES openssl/sha.h)
find_library(CRYPTO_LIBRARY NAMES crypto)
if(NOT OPENSSL_INCLUDE_DIR OR NOT CRYPTO_LIBRARY)
    message(FATAL_ERROR "libcrypto dev files not found (need openssl/sha.h + libcrypto)")
endif()

# App crate parity: Cargo.toml builds the crate as a library ([lib], module
# map in src/lib.rs) plus the src/main.rs binary. Same split here: the
# rustdesk static library holds every app module, cppdesk links it, and the
# tests link it instead of recompiling app sources one by one (step 15).
add_library(rustdesk STATIC
    src/common.cpp
    src/rendezvous_mediator.cpp
    src/client.cpp
    src/key_map.cpp
    src/connection.cpp
    src/server.cpp
    src/audio_service.cpp
    src/video_service.cpp
    src/clipboard_service.cpp
    src/input_service.cpp
    src/ipc.cpp
    # std::process / whoami stand-ins (process.hpp, whoami.hpp)
    src/process.cpp
    src/whoami.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # platform/mod.rs: `pub use linux::*`. macOS/Windows arrive with the
    # platform step; until then other hosts fail to link (platform.hpp).
    target_sources(rustdesk PRIVATE src/platform/linux.cpp)
endif()
if(WIN32)
    # build.rs build_windows() parity: compile src/windows.cc into the crate.
    target_sources(rustdesk PRIVATE src/platform/windows.cc)
    # Upstream leaves WtsApi32 commented out — same here:
    # target_link_libraries(rustdesk PRIVATE WtsApi32)
endif()
target_include_directories(
    rustdesk
    PUBLIC
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        include
        # generated protobuf headers come from hbb_common's PUBLIC include
        # (build-time protos_gen, or the checked-in protos/gen fallback)
        ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(rustdesk PUBLIC hbb_common parity_tokio_ipc cppdesk_protobuf ${CRYPTO_LIBRARY})
# NOTE: cppdesk_protobuf is defined above and shared with libs/hbb_common;
# the app calls libprotobuf directly.

add_executable(cppdesk src/main.cpp)
target_link_libraries(cppdesk PRIVATE rustdesk)

include(CTest)
add_subdirectory(tests)
