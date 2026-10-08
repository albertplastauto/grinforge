// Minimal HTTP monitoring API.
//
// Deliberately tiny and dependency-free (WinSock2 only): a single-threaded
// listener that answers GET with a JSON snapshot supplied by the caller. The
// JSON shape follows GMiner's /stat so existing dashboards and scripts keep
// working; that is why the field names are not "pretty".
//
// Binds to 127.0.0.1 by default. Exposing a miner's API to the LAN is a
// deliberate decision that must be made by the operator, not by a default.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <atomic>

namespace grin {

class HttpApi {
public:
    // Returns the JSON body for GET requests. Must be cheap and thread-safe.
    using SnapshotFn = std::function<std::string()>;
    using LogFn = std::function<void(const std::string&)>;

    HttpApi() = default;
    ~HttpApi();
    HttpApi(const HttpApi&) = delete;
    HttpApi& operator=(const HttpApi&) = delete;

    // `bindAll` = false binds 127.0.0.1 only.
    bool start(uint16_t port, bool bindAll, SnapshotFn snapshot, LogFn log);
    void stop();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }

private:
    void loop();

    SnapshotFn snapshot_;
    LogFn log_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::intptr_t listenSock_{-1};
    uint16_t port_ = 0;
    bool bindAll_ = false;
};

} // namespace grin
