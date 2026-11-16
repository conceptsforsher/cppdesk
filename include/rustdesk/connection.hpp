// connection.hpp — translation of src/server/connection.rs.
//
// Step 13 landed ConnInner and its message queue. Step 15 (part 12 of
// d1013487) ports the rest: the Connection itself (start + handlers),
// start_ipc (the link to the connection-manager process "_cm"),
// try_activate_screen and the CLICK_TIME / LOGIN_FAILURES statics.
//
// Event loop. Upstream runs each connection as one tokio task whose
// select! waits on five sources: data from the cm (rx_from_cm), messages
// from the services (rx), frames from the peer (stream.next()), the
// read-jobs timer and the test-delay timer. Here Connection::start runs on
// the caller's thread as a poll() loop over the TCP socket and the wake
// descriptors of the two channels (hbb_common/mpsc.hpp, ConnQueue), with the
// timers as poll deadlines. Every pass gives each ready source one turn;
// select! picks one ready branch at random per pass, and both orders give
// every source its turn. start_ipc runs on its own thread with the same
// shape (IPC socket + the rx_to_cm wake pipe).
//
// Channels keep tokio's drop semantics: when start() returns, the service
// queue is closed (later service sends are dropped), tx_to_cm goes away (so
// start_ipc's recv yields None and it exits with "expected"), and
// rx_from_cm goes away (start_ipc's sends fail).
//
// Native seams (see each module's header): input injection (enigo), display
// enumeration (scrap), the system clipboard (copypasta). Each behaves as it
// does upstream on a host without a display server.

#pragma once

#include <rustdesk/ipc.hpp>

#include <hbb_common/fs.hpp>
#include <hbb_common/mpsc.hpp>
#include <hbb_common/tcp.hpp>
#include <hbb_common/util.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Generated protobuf (message.proto): queued payload type.
#include "message.pb.h"

namespace rustdesk {

class Server;

using MessagePtr = std::shared_ptr<hbb::Message>;

// One queued item: (send Instant, message) parity.
struct QueuedMessage {
    std::chrono::steady_clock::time_point instant;
    MessagePtr message;
};

// Unbounded multi-producer channel (mpsc::unbounded_channel parity) carrying
// the services' messages to one connection. send() never blocks. close() is
// the receiver going away: queued items are dropped and later sends are
// discarded (tx.send() fails, and ConnInner::send ignores it).
class ConnQueue {
public:
    void send(MessagePtr msg);
    // Non-blocking drain (test + service fan-out support).
    std::vector<MessagePtr> drain();
    // Blocking receive with timeout; nullopt on timeout or after close().
    std::optional<MessagePtr> wait_receive(uint64_t timeout_ms);
    // Non-blocking recv() keeping the send Instant (the loop measures
    // latency from it).
    std::optional<QueuedMessage> try_recv();
    // Readable after a send (see hbb_common/mpsc.hpp for the discipline).
    // The pipe is created on first use.
    int wake_fd();
    void clear_wake();
    void close();
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QueuedMessage> queue_;
    bool closed_ = false;
    std::unique_ptr<hbb_common::mpsc::WakePipe> wake_;
};

using ConnQueuePtr = std::shared_ptr<ConnQueue>;

// ConnInner parity (id + optional sender). Default-constructed (id 0, no
// queue) sends are silent no-ops, mirroring tx: None. Copyable (id + shared
// queue); upstream derives Clone for the same reason.
struct ConnInner {
    int32_t id = 0;
    ConnQueuePtr tx;  // nullptr == None

    // Structural Subscriber model: ServiceTmpl<T> calls get_id()/send().
    // (Upstream's trait method is id(), which cannot share the field's name
    // in C++; renamed here and in the template, semantics unchanged.)
    int get_id() const { return id; }
    // Subscriber::send parity: forwards into the queue when present.
    void send(const MessagePtr& msg) const;
};

// tokio 0.2 time::Interval: ticks at start, start + period, ...; a late tick
// fires at once and the next deadline stays on the original grid (missed
// ticks burst).
struct Interval {
    std::chrono::steady_clock::time_point next;
    std::chrono::steady_clock::duration period;

    // time::interval_at(start, period)
    static Interval at(std::chrono::steady_clock::time_point start,
                       std::chrono::steady_clock::duration period) {
        return Interval{start, period};
    }
    // time::interval(period): the first tick completes immediately.
    static Interval every(std::chrono::steady_clock::duration period) {
        return at(std::chrono::steady_clock::now(), period);
    }
    bool ready(std::chrono::steady_clock::time_point now) const { return now >= next; }
    void tick() { next += period; }
};

class Connection {
public:
    // `pub async fn start(addr, stream, id, server)`: serves the peer until the
    // connection ends, on the calling thread.
    static void start(const hbb_common::ResolvedAddr& addr, hbb_common::TcpFramedStream stream,
                      int32_t id, std::weak_ptr<Server> server);

private:
    Connection(ConnInner inner, hbb_common::TcpFramedStream stream, std::weak_ptr<Server> server,
               hbb::Hash hash, hbb_common::mpsc::UnboundedSender<ipc::Data> tx_to_cm);

    void send_permisssion(hbb::Permission permission, bool enabled);  // sic (upstream name)
    bool on_open(const hbb_common::ResolvedAddr& addr);
    void send_logon_response();
    bool clipboard_enabled() const;
    bool audio_enabled() const;
    void try_start_cm(const std::string& peer_id, const std::string& name, bool authorized);
    void send_to_cm(ipc::Data data);
    void send_fs(ipc::FS data);
    void send_login_error(const std::string& err);
    bool on_message(const hbb::Message& msg);
    void update_option(const hbb::OptionMessage& o);
    void on_close(const std::string& reason, bool lock);
    void read_dir(const std::string& dir, bool include_hidden);
    void send(const hbb::Message& msg);

    ConnInner inner_;
    hbb_common::TcpFramedStream stream_;
    std::weak_ptr<Server> server_;
    hbb::Hash hash_;
    std::vector<hbb_common::TransferJob> read_jobs_;
    Interval timer_;
    std::optional<std::pair<std::string, bool>> file_transfer_;
    // Framed<TcpStream, tokio_util BytesCodec>: a raw-mode stream.
    std::optional<hbb_common::TcpFramedStream> port_forward_socket_;
    std::string port_forward_address_;
    hbb_common::mpsc::UnboundedSender<ipc::Data> tx_to_cm_;
    bool authorized_ = false;
    bool keyboard_ = false;
    bool clipboard_ = false;
    bool audio_ = false;
    int64_t last_test_delay_ = 0;
    int32_t image_quality_ = 0;
    bool lock_after_session_end_ = false;
    bool show_remote_cursor_ = false;  // by peer
    bool privacy_mode_ = false;
    std::string ip_;
    bool disable_clipboard_ = false;  // by peer
    bool disable_audio_ = false;      // by peer
};

// `async fn start_ipc(rx_to_cm, tx_from_cm)`: waits out the pre-login
// screen, connects to the cm at postfix "_cm" (starting `<exe> --cm`, via
// run_as_user when root, if none answers), then relays both ways until either
// side goes away. Throws the error that ended it.
void start_ipc(hbb_common::mpsc::UnboundedReceiver<ipc::Data> rx_to_cm,
               hbb_common::mpsc::UnboundedSender<ipc::Data> tx_from_cm);

// In case the screen is asleep and blank, wake it (Windows only upstream).
void try_activate_screen();

// CLICK_TIME: when an authorized peer last released the left button or hit
// Enter (ms); the cm asks for it with Data::ClickTime.
int64_t click_time();

}  // namespace rustdesk
