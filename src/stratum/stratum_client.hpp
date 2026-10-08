#pragma once

// GRIN Cuckatoo32 stratum client.
//
// Transport: plain TCP, newline-delimited JSON-RPC 2.0 (no TLS on port 3030).
// Handshake: login -> getjobtemplate. Jobs arrive either as an unsolicited
// `method:"job"` notification or as the `result` of getjobtemplate. A
// `keepalive` is sent every `keepalive_seconds`. Shares are submitted with one
// of two `pow` envelopes (see Config::use_edge_bits_submit_form).
//
// See docs/stratum-protocol.md for the verified wire format. The pool is known
// to emit optional whitespace after ':' ("method": "login", "pre_pow": "..."),
// so every field lookup tolerates it.
//
// Threading model
// ---------------
// * The socket is owned by one IO thread spawned by start().
// * submit() is safe to call from any thread: it appends one pre-serialised
//   JSON line to a mutex-protected queue and never performs network I/O.
// * Callbacks (job / log / disconnect) are invoked from the IO thread. They
//   must be cheap, must not destroy the client, and must not call stop() and
//   then expect it to join (stop() detects that case and only signals).
// * No part of the public API throws; failures are reported through the return
//   value and the log callback.
//
// This header deliberately does not include any Windows header: the socket
// handle is stored as std::intptr_t so that consumers stay free of winsock2.h.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace grin {

// --- Protocol constants (verified against the live pool + reference client) ---

// HEADER_SIZE_EXCLUDING_NONCE: the pre_pow blob is the 238-byte header without
// the 8-byte nonce. The pool sends it as exactly 476 hex characters.
inline constexpr std::size_t kHeaderSizeExcludingNonce = 238;
inline constexpr std::size_t kPrePowHexLength = kHeaderSizeExcludingNonce * 2;  // 476
// PROOFSIZE: a Cuckatoo32 cycle is 42 edges, sent as 42 decimal integers.
inline constexpr std::size_t kProofSize = 42;
inline constexpr std::size_t kEdgeBits = 32;

struct Job {
    std::uint64_t height = 0;
    std::uint64_t job_id = 0;
    std::uint64_t difficulty = 1;
    std::vector<std::uint8_t> pre_pow;  // exactly 238 bytes for GRIN C32
    std::string algorithm = "Cuckoo";
    std::uint64_t arrival_unix_ms = 0;
};

struct Solution {
    std::uint64_t nonce = 0;
    std::uint64_t edge_bits = 32;
    std::uint32_t proof[42] = {};  // PROOFSIZE = 42 ascending edge nonces
};

class StratumClient {
public:
    struct Config {
        std::string host;
        std::uint16_t port = 3030;
        std::string user;  // <wallet>.<worker>
        std::string pass = "x";
        std::string agent = "grinforge/0.1";
        std::string algorithm = "Cuckoo";
        int keepalive_seconds = 10;
        int io_timeout_seconds = 60;  // receive/send timeout
        int reconnect_delay_seconds = 1;
        bool use_edge_bits_submit_form = false;  // false => {"pow":{"Cuckoo":[32,[...]]}}
    };

    using JobCallback = std::function<void(const Job&)>;
    using LogCallback = std::function<void(const std::string&)>;
    using DisconnectCallback = std::function<void()>;

    explicit StratumClient(Config cfg);
    ~StratumClient();
    StratumClient(const StratumClient&) = delete;
    StratumClient& operator=(const StratumClient&) = delete;

    void set_job_callback(JobCallback cb);
    void set_log_callback(LogCallback cb);
    void set_disconnect_callback(DisconnectCallback cb);

    // Connects, logs in, requests a job template and spawns the IO thread.
    // Returns false when the initial connect/login could not be performed.
    // On success the connection is maintained (and re-established) by the IO
    // thread until stop() is called.
    bool start();

    // Idempotent. Signals the IO thread, shuts the socket down to unblock
    // select()/recv(), joins the thread and releases Winsock.
    void stop();

    bool is_connected() const;

    // Thread-safe and non-blocking: queues one submit for the IO thread.
    // Returns false (and logs) when the client is not connected, when the job
    // payload is not a 238-byte pre_pow, or when the queue is full.
    bool submit(const Job& job, const Solution& sol);

    struct Stats {
        std::uint64_t jobs = 0, accepted = 0, rejected = 0, stale = 0, malformed = 0, reconnects = 0;
    };
    Stats stats() const;

private:
    // Winsock lifetime (process-wide reference count).
    bool wsa_acquire();
    void wsa_release();

    // IO-thread side.
    bool connect_and_login();
    void close_socket();
    void io_loop();
    void run_session();
    void process_buffer();
    bool drain_outbound();
    bool send_all(const std::string& data);
    bool interruptible_sleep_seconds(int seconds);

    void handle_line(const std::string& line);
    void consider_envelope_fallback(const std::string& server_text);

    // Callback / logging helpers (safe to call from any thread).
    void emit_job(const Job& job);
    void emit_disconnect();
    void log(const std::string& message);

    void add_stat(std::uint64_t Stats::*field, std::uint64_t amount = 1);

    std::string next_request_id();
    std::string build_login();
    std::string build_getjobtemplate();
    std::string build_keepalive();
    std::string build_submit(const Job& job, const Solution& sol);

    Config cfg_;

    JobCallback job_cb_;
    LogCallback log_cb_;
    DisconnectCallback disconnect_cb_;
    mutable std::mutex callback_mutex_;

    mutable std::mutex stats_mutex_;
    Stats stats_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::string> out_queue_;

    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> next_request_id_value_{0};
    std::atomic<bool> use_edge_bits_form_{false};
    std::atomic<bool> envelope_fallback_done_{false};

    // Handle to the live socket, INVALID_SOCKET (-1) when disconnected.
    std::atomic<std::intptr_t> sock_{-1};

    // IO thread only.
    std::string rx_buffer_;
    std::chrono::steady_clock::time_point last_keepalive_{};
    bool job_request_retry_pending_ = false;
};

}  // namespace grin
