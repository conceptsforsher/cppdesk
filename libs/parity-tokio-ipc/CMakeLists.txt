# libs/parity-tokio-ipc/CMakeLists.txt — translation of libs/parity-tokio-ipc/Cargo.toml
# (rustdesk's vendored fork of parity-tokio-ipc 0.7.2, MIT/Apache-2.0; licenses
# copied verbatim alongside).
#
# Dependency mapping:
#   tokio (uds, io-util, ...) / futures / libc -> blocking POSIX sockets
#   rand                                       -> <random> (dummy_endpoint)
#   log                                        -> unused by the unix backend
#   mio-named-pipes / miow / winapi (win.rs)   -> deferred with the Windows
#                                                 platform step (named pipes)

add_library(parity_tokio_ipc STATIC src/unix.cpp)
target_include_directories(parity_tokio_ipc PUBLIC include)
target_compile_features(parity_tokio_ipc PUBLIC cxx_std_20)
