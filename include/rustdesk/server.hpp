// server.hpp — translation of src/server.rs.
//
// Step 13 (part 10) landed the CHILD_PROCESS reaper (check_zombie), the
// Server registry (connections + services + id counter) and new().
// Step 15 (part 12) adds the entry points: accept_connection(_) (take over a
// hole-punched port), create_tcp_connection_ (the signed-id + box key
// handshake, then Connection::start), create_relay_connection(_) and
// start_server (the --server daemon, or a client syncing the key pair from
// it).
//
// `async fn` entry points block their caller until the connection ends;
// the rendezvous mediator calls them on per-request threads, like upstream's
// tokio::spawn. Errors are logged upstream and swallowed here (silent log).

#pragma once

#include <rustdesk/service.hpp>

#include <hbb_common/tcp.hpp>
#include <hbb_common/util.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rustdesk {

// CHILD_PROCESS parity: upstream holds std::process::Child handles pushed by
// platform code; here plain pids (platform step pushes via track_child_pid).
// pid type is intptr_t (pid_t/DWORD both fit; Windows reaper deferred).
std::mutex& child_lock();
std::vector<intptr_t>& child_processes();
void track_child_pid(intptr_t pid);
// Reaper thread parity (100ms waitpid/WNOHANG sweep; detached like upstream).
void check_zombie();

class Server {
public:
    // new() parity: builds the registry with all five services.
    static std::shared_ptr<Server> create();

    void add_connection(const ConnInner& conn, const std::vector<std::string>& noperms);
    void remove_connection(const ConnInner& conn);
    void subscribe(const std::string& name, const ConnInner& conn, bool sub);
    // id_count += 1 under lock (create_tcp_connection_ parity).
    int32_t next_id();

    // Introspection for wiring/tests (no upstream counterpart; the map is
    // otherwise private like upstream's).
    std::shared_ptr<Service> find_service(const std::string& name) const;
    bool has_connection(int32_t id) const;

    // Drop parity: joins every service worker.
    ~Server();

private:
    Server() = default;
    void add_service(std::shared_ptr<Service> service);

    mutable std::shared_mutex mutex_;
    std::unordered_map<int32_t, ConnInner> connections_;
    std::unordered_map<std::string, std::shared_ptr<Service>> services_;
    int32_t id_count_ = 0;
};

// ServerPtr parity (Arc<RwLock<Server>>; locks live inside the methods, so
// callers never hold guards across calls — each op stays atomic as upstream).
using ServerPtr = std::shared_ptr<Server>;
using ServerPtrWeak = std::weak_ptr<Server>;

// `pub async fn accept_connection(server, socket, peer_addr, secure)`:
// drops `socket` (the rendezvous connection), listens on its local address
// with reuse for CONNECT_TIMEOUT, and serves the first peer that connects.
void accept_connection(ServerPtr server, hbb_common::TcpFramedStream socket,
                       const hbb_common::ResolvedAddr& peer_addr, bool secure);

// `pub async fn create_relay_connection(server, relay_server, uuid,
// peer_addr, secure)`: connects to the relay (default port RELAY_PORT), sends
// RequestRelay { uuid } and serves the peer through it.
void create_relay_connection(ServerPtr server, const std::string& relay_server,
                             const std::string& uuid, const hbb_common::ResolvedAddr& peer_addr,
                             bool secure);

// `#[tokio::main] pub async fn start_server(is_server, _tray)`. With
// is_server: the ipc server (postfix "") on a thread, then
// RendezvousMediator::start_all() forever. Otherwise: ask a running server
// for its system info and confirmed key pair, or start one on a thread
// (start_server(true, false)) when none answers.
void start_server(bool is_server, bool tray);

}  // namespace rustdesk
