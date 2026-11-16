// server.cpp — see server.hpp.
#include <rustdesk/server.hpp>

#include <rustdesk/audio_service.hpp>
#include <rustdesk/clipboard_service.hpp>
#include <rustdesk/common.hpp>
#include <rustdesk/connection.hpp>
#include <rustdesk/input_service.hpp>
#include <rustdesk/ipc.hpp>
#include <rustdesk/rendezvous_mediator.hpp>
#include <rustdesk/video_service.hpp>

#include <hbb_common/config.hpp>
#include <hbb_common/crypto.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#endif

namespace rustdesk {

std::mutex& child_lock() {
    static std::mutex m;
    return m;
}

std::vector<intptr_t>& child_processes() {
    static std::vector<intptr_t> pids;
    return pids;
}

void track_child_pid(intptr_t pid) {
    std::unique_lock l(child_lock());
    child_processes().push_back(pid);
}

void check_zombie() {
    // Reaper parity: sweep every 100ms, drop reaped children.
    std::thread([] {
        while (true) {
            {
                std::unique_lock l(child_lock());
                auto& pids = child_processes();
                size_t i = 0;
                while (i < pids.size()) {
#if defined(__unix__) || defined(__APPLE__)
                    // try_wait() parity: remove only positively-reaped pids.
                    const pid_t ret =
                        ::waitpid(static_cast<pid_t>(pids[i]), nullptr, WNOHANG);
                    if (ret > 0) {
                        pids.erase(pids.begin() + static_cast<ptrdiff_t>(i));
                    } else {
                        ++i;
                    }
#else
                    // Windows reaper deferred to the platform step.
                    ++i;
#endif
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}

std::shared_ptr<Server> Server::create() {
    auto server = std::shared_ptr<Server>(new Server());
    server->add_service(std::make_shared<GenericService>(audio_service::create()));
    server->add_service(std::make_shared<GenericService>(video_service::create()));
    server->add_service(std::make_shared<GenericService>(clipboard_service::create()));
    server->add_service(std::make_shared<input_service::MouseCursorService>(
        input_service::new_cursor()));
    server->add_service(std::make_shared<GenericService>(input_service::new_pos()));
    return server;
}

void Server::add_service(std::shared_ptr<Service> service) {
    std::unique_lock l(mutex_);
    const std::string name = service->name();
    services_.emplace(name, std::move(service));
}

void Server::add_connection(const ConnInner& conn, const std::vector<std::string>& noperms) {
    std::unique_lock l(mutex_);
    for (const auto& [name, service] : services_) {
        bool skip = false;
        for (const auto& np : noperms) {
            if (np == name) {
                skip = true;
                break;
            }
        }
        if (!skip) {
            service->on_subscribe(conn);
        }
    }
    connections_.emplace(conn.get_id(), conn);
}

void Server::remove_connection(const ConnInner& conn) {
    std::unique_lock l(mutex_);
    for (const auto& [name, service] : services_) {
        (void)name;
        service->on_unsubscribe(conn.get_id());
    }
    connections_.erase(conn.get_id());
}

void Server::subscribe(const std::string& name, const ConnInner& conn, bool sub) {
    std::unique_lock l(mutex_);
    const auto it = services_.find(name);
    if (it == services_.end()) {
        return;
    }
    const std::shared_ptr<Service>& service = it->second;
    if (service->is_subed(conn.get_id()) == sub) {
        return;
    }
    if (sub) {
        service->on_subscribe(conn);
    } else {
        service->on_unsubscribe(conn.get_id());
    }
}

int32_t Server::next_id() {
    std::unique_lock l(mutex_);
    id_count_ += 1;
    return id_count_;
}

std::shared_ptr<Service> Server::find_service(const std::string& name) const {
    std::shared_lock l(mutex_);
    const auto it = services_.find(name);
    return it != services_.end() ? it->second : nullptr;
}

bool Server::has_connection(int32_t id) const {
    std::shared_lock l(mutex_);
    return connections_.count(id) > 0;
}

Server::~Server() {
    // Drop parity: join every service worker. (Services map destruction alone
    // would abandon threads; join first while state is reachable.)
    std::unique_lock l(mutex_);
    for (const auto& [name, service] : services_) {
        (void)name;
        service->join();
    }
}

namespace {

using hbb_common::ResolvedAddr;
using hbb_common::TcpFramedStream;
namespace crypto = hbb_common::crypto;

void create_tcp_connection_(ServerPtr server, TcpFramedStream stream, const ResolvedAddr& addr,
                            bool secure) {
    const int32_t id = server->next_id();
    const auto key_pair = hbb_common::get_key_pair();
    const std::vector<uint8_t>& sk = key_pair[0];
    const std::vector<uint8_t>& pk = key_pair[1];
    if (secure && pk.size() == crypto::kSignPublicKeyBytes &&
        sk.size() == crypto::kSignSecretKeyBytes) {
        const std::string my_id = hbb_common::get_id();
        const std::vector<uint8_t> signed_id =
            crypto::sign_sign(reinterpret_cast<const uint8_t*>(my_id.data()), my_id.size(), sk);
        const auto [our_pk_b, our_sk_b] = crypto::box_gen_keypair();
        hbb::Message msg_out;
        hbb::SignedId* si = msg_out.mutable_signed_id();
        si->set_id(signed_id.data(), signed_id.size());
        si->set_pk(our_pk_b.data(), our_pk_b.size());
        // timeout(CONNECT_TIMEOUT, stream.send(&msg_out)).await??
        stream.set_timeout_ms(static_cast<int>(hbb_common::kConnectTimeoutMs));
        const bool sent = stream.send(msg_out);
        stream.set_timeout_ms(0);
        if (!sent) {
            throw std::runtime_error("Failed to send signed id");
        }
        // match timeout(CONNECT_TIMEOUT, stream.next()).await?
        const auto bytes = stream.next_timeout(hbb_common::kConnectTimeoutMs);
        if (!bytes) {
            if (stream.peer_closed()) {
                throw std::runtime_error("Failed to receive public key");  // None (or res? Err)
            }
            throw std::runtime_error("deadline has elapsed");  // Elapsed
        }
        hbb::Message msg_in;
        if (msg_in.ParseFromArray(bytes->data(), static_cast<int>(bytes->size()))) {
            if (msg_in.union_case() == hbb::Message::kPublicKey) {
                const hbb::PublicKey& their = msg_in.public_key();
                if (their.asymmetric_value().size() == crypto::kBoxPublicKeyBytes) {
                    const crypto::Nonce24 nonce{};  // box_::Nonce([0u8; NONCEBYTES])
                    const std::vector<uint8_t> their_pk_b(their.asymmetric_value().begin(),
                                                          their.asymmetric_value().end());
                    std::vector<uint8_t> symmetric_key;
                    try {
                        symmetric_key = crypto::box_open(
                            reinterpret_cast<const uint8_t*>(their.symmetric_value().data()),
                            their.symmetric_value().size(), nonce, their_pk_b, our_sk_b);
                    } catch (const std::exception&) {
                        throw std::runtime_error("Handshake failed: box decryption failure");
                    }
                    if (symmetric_key.size() != crypto::kSecretBoxKeyBytes) {
                        throw std::runtime_error(
                            "Handshake failed: invalid secret key length from peer");
                    }
                    stream.set_key(symmetric_key);
                } else if (!their.asymmetric_value().empty()) {
                    throw std::runtime_error(
                        "Handshake failed: invalid public sign key length from peer");
                }
            } else {
                // log::error!("Handshake failed: invalid message type");
            }
        } else {
            throw std::runtime_error("Handshake failed: invalid message format");
        }
    }
    Connection::start(addr, std::move(stream), id, ServerPtrWeak(server));
}

void accept_connection_(ServerPtr server, TcpFramedStream socket, bool secure) {
    const auto local_addr = socket.local_addr();  // socket.get_ref().local_addr()?
    if (!local_addr) {
        throw std::runtime_error("local_addr failed");
    }
    {
        // drop(socket). Even after the drop, binding below may fail without
        // reuse_addr: the old socket sits in TIME_WAIT for a while (upstream
        // note about "Only one usage of each socket address" on Windows).
        TcpFramedStream dropped = std::move(socket);
    }
    auto listener = hbb_common::TcpListener::bind(*local_addr, true);  // new_listener(.., true)?
    if (!listener) {
        throw std::runtime_error("could not resolve to any address");
    }
    // log::info!("Server listening on: {}", &listener.local_addr()?);
    // timeout(CONNECT_TIMEOUT, listener.accept()).await?: Elapsed propagates
    // and an accept error is skipped; both end here without a connection.
    if (auto stream = listener->accept(hbb_common::kConnectTimeoutMs)) {
        const auto addr = stream->peer_addr();
        if (!addr) {
            return;
        }
        create_tcp_connection_(std::move(server), std::move(*stream), *addr, secure);
    }
}

void create_relay_connection_(ServerPtr server, const std::string& relay_server,
                              const std::string& uuid, const ResolvedAddr& peer_addr,
                              bool secure) {
    auto stream = TcpFramedStream::connect(
        hbb_common::to_socket_addr(check_port(relay_server, hbb_common::kRelayPort)),
        hbb_common::get_any_listen_addr(), hbb_common::kConnectTimeoutMs);
    if (!stream) {
        throw std::runtime_error("Failed to connect to relay server");
    }
    hbb::RendezvousMessage msg_out;
    msg_out.mutable_request_relay()->set_uuid(uuid);
    if (!stream->send(msg_out)) {
        throw std::runtime_error("Failed to send relay request");
    }
    create_tcp_connection_(std::move(server), std::move(*stream), peer_addr, secure);
}

void sleep_secs(double secs) {
    std::this_thread::sleep_for(std::chrono::duration<double>(secs));
}

}  // namespace

void accept_connection(ServerPtr server, TcpFramedStream socket, const ResolvedAddr& peer_addr,
                       bool secure) {
    try {
        accept_connection_(std::move(server), std::move(socket), secure);
    } catch (const std::exception&) {
        // log::error!("Failed to accept connection from {}: {}", peer_addr, err);
        (void)peer_addr;
    }
}

void create_relay_connection(ServerPtr server, const std::string& relay_server,
                             const std::string& uuid, const ResolvedAddr& peer_addr,
                             bool secure) {
    try {
        create_relay_connection_(std::move(server), relay_server, uuid, peer_addr, secure);
    } catch (const std::exception&) {
        // log::error!("Failed to create relay connection for {} with uuid {}: {}",
        //             peer_addr, uuid, err);
    }
}

void start_server(bool is_server, bool /*tray*/) {
#if defined(__linux__)
    // log::info!("DISPLAY={:?}", std::env::var("DISPLAY"));
    // log::info!("XAUTHORITY={:?}", std::env::var("XAUTHORITY"));
#endif
    if (is_server) {
        std::thread([] {
            try {
                ipc::start("");
            } catch (const std::exception&) {
                // log::error!("Failed to start ipc: {}", err);
                // std::process::exit(-1): no destructors run (Rust has no
                // static destructors to run either).
                std::fflush(nullptr);
                std::_Exit(-1);
            }
        }).detach();
        // tray is buggy, and not work on win10 2004, also cause crash, disable
        // it (upstream keeps the Windows tray spawner commented out here).
        RendezvousMediator::start_all();
        return;
    }
    std::optional<ipc::Connection> conn;
    try {
        conn.emplace(ipc::connect(1000, ""));
    } catch (const std::exception&) {
        // log::info!("server not started (will try to start): {}", err);
        std::thread([] { start_server(true, false); }).detach();
        return;
    }
    HBB_ALLOW_ERR(conn->send(ipc::Data::SystemInfo{std::nullopt}));
    try {
        if (const auto data = conn->next_timeout(1000)) {
            // log::info!("server info: {:?}", data);
        }
    } catch (const std::exception&) {
    }
    // sync key pair
    int n = 0;
    while (true) {
        if (hbb_common::get_key_confirmed()) {
            // check ipc::get_id(), key_confirmed may change, so give some
            // chance to correct
            n += 1;
            if (n > 3) {
                break;
            }
            sleep_secs(1.);
        } else {
            HBB_ALLOW_ERR(conn->send(ipc::Data::ConfirmedKey{std::nullopt}));
            std::optional<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> pair;
            try {
                if (const auto data = conn->next_timeout(1000)) {
                    if (const auto* ck = data->get_if<ipc::Data::ConfirmedKey>()) {
                        pair = ck->value;
                    }
                }
            } catch (const std::exception&) {
            }
            if (pair) {
                hbb_common::set_key_pair({pair->first, pair->second});
                hbb_common::set_key_confirmed(true);
                // log::info!("key pair synced");
                break;
            }
            sleep_secs(1.);
        }
    }
}

}  // namespace rustdesk
