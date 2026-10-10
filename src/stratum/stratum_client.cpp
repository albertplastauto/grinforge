// GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
// Copyright (c) 2026 albertplastauto
// SPDX-License-Identifier: MIT
//
// GRIN Cuckatoo32 stratum client implementation.
//
// Why hand-rolled JSON?
// ---------------------
// The client must be dependency-free, and it only ever needs a handful of
// fields out of shallow JSON-RPC objects. A tiny scanner that skips whitespace
// after ':' is both smaller and more tolerant than a full parser for this
// protocol (the pool really does send `"method": "login"` with a space).
//
// One IO thread owns the socket. It uses select() with a <= 200 ms timeout so
// that a single loop can service the outbound submit queue, keepalives and
// incoming data. Windows select() cannot wait on a condition variable, so the
// condition variable is used for shutdown/reconnect sleeping instead of for
// queue waiting; the queue is drained on every loop iteration, which bounds
// outbound latency to 200 ms.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// winsock2.h must precede windows.h (and therefore any header that might pull
// it in), hence these includes sit in front of the project header.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "stratum_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

static_assert(grin::kProofSize == 42, "PROOFSIZE must match the Solution::proof array");
static_assert(grin::kHeaderSizeExcludingNonce == 238, "GRIN C32 header size must be 238 bytes");

namespace grin {
namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr std::size_t kMaxReceiveBufferBytes = 1u << 20;  // 1 MiB of unterminated line is a protocol failure
constexpr std::size_t kMaxQueuedSubmits = 256;            // deeper queues only add stale shares
constexpr std::size_t kSendChunkBytes = 1u << 20;

bool is_json_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::uint64_t unix_now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char raw : input) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* const kHexDigits = "0123456789abcdef";
                    out += "\\u00";
                    out += kHexDigits[(c >> 4) & 0x0F];
                    out += kHexDigits[c & 0x0F];
                } else {
                    out += raw;
                }
                break;
        }
    }
    return out;
}

// Case-insensitive substring search; used to spot "Stale"/"malformed" wording
// in server error messages without allocating temporary strings.
bool contains_ci(const std::string& haystack, const char* needle) {
    const std::size_t needle_len = std::strlen(needle);
    if (needle_len == 0 || haystack.size() < needle_len) return false;
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    for (std::size_t i = 0; i + needle_len <= haystack.size(); ++i) {
        std::size_t j = 0;
        while (j < needle_len && lower(haystack[i + j]) == lower(needle[j])) ++j;
        if (j == needle_len) return true;
    }
    return false;
}

// Returns the index of the first character of the value of "key", i.e. just
// past the ':' and any whitespace. Scans string tokens so that a key name
// occurring inside a value cannot produce a false match. Returns npos when the
// key is absent.
std::size_t find_key_value(const std::string& text, const char* key) {
    const std::size_t key_len = std::strlen(key);
    const std::size_t size = text.size();
    std::size_t i = 0;
    while (i < size) {
        if (text[i] != '"') {
            ++i;
            continue;
        }
        const std::size_t token_start = i + 1;
        std::size_t j = token_start;
        bool closed = false;
        while (j < size) {
            if (text[j] == '\\') {
                j += 2;
                continue;
            }
            if (text[j] == '"') {
                closed = true;
                break;
            }
            ++j;
        }
        if (!closed) return std::string::npos;
        if (j - token_start == key_len && text.compare(token_start, key_len, key) == 0) {
            std::size_t k = j + 1;
            while (k < size && is_json_ws(text[k])) ++k;
            if (k < size && text[k] == ':') {
                ++k;
                while (k < size && is_json_ws(text[k])) ++k;
                return k;
            }
        }
        i = j + 1;
    }
    return std::string::npos;
}

// Decodes a JSON string starting at `pos` (which must hold the opening quote).
bool parse_json_string(const std::string& text, std::size_t pos, std::string& out) {
    if (pos >= text.size() || text[pos] != '"') return false;
    out.clear();
    std::size_t i = pos + 1;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\\') {
            if (i + 1 >= text.size()) return false;
            const char esc = text[i + 1];
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    // Not expected from the pool; keep the raw escape so that at
                    // least field values are never silently corrupted.
                    if (i + 6 > text.size()) return false;
                    for (int n = 0; n < 6; ++n) {
                        const char h = text[i + n];
                        if (n >= 2 && hex_value(h) < 0) return false;
                    }
                    out.append(text, i, 6);
                    i += 6;
                    continue;
                }
                default: return false;
            }
            i += 2;
        } else if (c == '"') {
            return true;
        } else {
            out += c;
            ++i;
        }
    }
    return false;
}

// Parses an unsigned decimal integer at `pos`, accepting either a bare number
// or a quoted number (some pools quote numeric fields).
bool parse_json_u64(const std::string& text, std::size_t pos, std::uint64_t& out) {
    while (pos < text.size() && is_json_ws(text[pos])) ++pos;
    if (pos >= text.size()) return false;

    std::string digits;
    if (text[pos] == '"') {
        if (!parse_json_string(text, pos, digits)) return false;
    } else {
        if (text[pos] == '-') return false;
        std::size_t end = pos;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
        if (end == pos) return false;
        digits = text.substr(pos, end - pos);
    }
    if (digits.empty()) return false;

    std::uint64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

enum class JsonValueKind { Missing, Null, Bool, Number, String, Object, Array };

struct JsonValueRef {
    JsonValueKind kind = JsonValueKind::Missing;
    bool boolean = false;
    std::uint64_t number = 0;
    std::string text;
};

JsonValueRef json_value_at(const std::string& text, std::size_t pos) {
    JsonValueRef value;
    if (pos == std::string::npos || pos >= text.size()) return value;
    const char c = text[pos];
    if (c == 'n') {
        if (text.compare(pos, 4, "null") == 0) value.kind = JsonValueKind::Null;
    } else if (c == 't') {
        if (text.compare(pos, 4, "true") == 0) {
            value.kind = JsonValueKind::Bool;
            value.boolean = true;
        }
    } else if (c == 'f') {
        if (text.compare(pos, 5, "false") == 0) {
            value.kind = JsonValueKind::Bool;
            value.boolean = false;
        }
    } else if (c == '"') {
        if (parse_json_string(text, pos, value.text)) value.kind = JsonValueKind::String;
    } else if (c == '{') {
        value.kind = JsonValueKind::Object;
    } else if (c == '[') {
        value.kind = JsonValueKind::Array;
    } else if ((c >= '0' && c <= '9') || c == '-') {
        std::uint64_t number = 0;
        if (parse_json_u64(text, pos, number)) {
            value.kind = JsonValueKind::Number;
            value.number = number;
        }
    }
    return value;
}

// True when an "error" member is present, is not null, and is not the empty
// array/object that some pools use to mean "no error".
bool error_field_is_set(const std::string& text) {
    const std::size_t pos = find_key_value(text, "error");
    if (pos == std::string::npos) return false;
    const JsonValueRef value = json_value_at(text, pos);
    if (value.kind == JsonValueKind::Null || value.kind == JsonValueKind::Missing) return false;
    if (value.kind == JsonValueKind::Array || value.kind == JsonValueKind::Object) {
        std::size_t k = pos + 1;
        while (k < text.size() && is_json_ws(text[k])) ++k;
        const char closer = (value.kind == JsonValueKind::Array) ? ']' : '}';
        if (k < text.size() && text[k] == closer) return false;
    }
    return true;
}

std::string extract_method(const std::string& text) {
    const std::size_t pos = find_key_value(text, "method");
    if (pos == std::string::npos) return std::string();
    std::string method;
    if (!parse_json_string(text, pos, method)) return std::string();
    return method;
}

enum class SubmitOutcome { Accepted, Rejected, Stale, Malformed, Unknown };

SubmitOutcome classify_submit(const std::string& text, bool has_error, const JsonValueRef& result) {
    if (contains_ci(text, "stale")) return SubmitOutcome::Stale;
    if (has_error) {
        if (contains_ci(text, "malformed") || contains_ci(text, "invalid") || contains_ci(text, "cannot parse")) {
            return SubmitOutcome::Malformed;
        }
        return SubmitOutcome::Rejected;
    }
    switch (result.kind) {
        case JsonValueKind::Bool: return result.boolean ? SubmitOutcome::Accepted : SubmitOutcome::Rejected;
        case JsonValueKind::String: return SubmitOutcome::Accepted;  // {"result":"ok"}
        case JsonValueKind::Number: return result.number != 0 ? SubmitOutcome::Accepted : SubmitOutcome::Rejected;
        case JsonValueKind::Object:
        case JsonValueKind::Array: return SubmitOutcome::Accepted;
        case JsonValueKind::Null:
        case JsonValueKind::Missing:
        default: return SubmitOutcome::Unknown;
    }
}

// Parses a job from a `method:"job"` notification or from the `result` object
// of getjobtemplate. Both shapes are shallow, so fields are located by name.
bool parse_job_from_line(const std::string& line, const std::string& algorithm, Job& out, std::string& error) {
    const std::size_t height_pos = find_key_value(line, "height");
    if (height_pos == std::string::npos) {
        error = "no \"height\" field";
        return false;
    }
    std::uint64_t height = 0;
    if (!parse_json_u64(line, height_pos, height)) {
        error = "\"height\" is not a non-negative integer";
        return false;
    }
    if (height == 0) {
        error = "\"height\" is zero";
        return false;
    }

    std::uint64_t job_id = 0;
    const std::size_t job_id_pos = find_key_value(line, "job_id");
    if (job_id_pos != std::string::npos && !parse_json_u64(line, job_id_pos, job_id)) {
        error = "\"job_id\" is not a non-negative integer";
        return false;
    }
    // A missing job_id is tolerated: 2Miners sends 0 for C32, and pools that
    // omit it are treated as "no id".

    std::uint64_t difficulty = 1;
    const std::size_t difficulty_pos = find_key_value(line, "difficulty");
    if (difficulty_pos != std::string::npos) {
        std::uint64_t parsed = 0;
        if (parse_json_u64(line, difficulty_pos, parsed) && parsed != 0) difficulty = parsed;
    }

    const std::size_t pre_pow_pos = find_key_value(line, "pre_pow");
    if (pre_pow_pos == std::string::npos) {
        error = "no \"pre_pow\" field";
        return false;
    }
    std::string hex;
    if (!parse_json_string(line, pre_pow_pos, hex)) {
        error = "\"pre_pow\" is not a JSON string";
        return false;
    }
    if (hex.size() != kPrePowHexLength) {
        error = "\"pre_pow\" is " + std::to_string(hex.size()) + " hex chars (" +
                std::to_string(hex.size() / 2) + " bytes), expected 476 hex chars (238 bytes)";
        return false;
    }

    std::vector<std::uint8_t> bytes(kHeaderSizeExcludingNonce);
    for (std::size_t i = 0; i < kHeaderSizeExcludingNonce; ++i) {
        const int hi = hex_value(hex[i * 2]);
        const int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            error = "\"pre_pow\" contains a non-hex character";
            return false;
        }
        bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }

    out.height = height;
    out.job_id = job_id;
    out.difficulty = difficulty;
    out.pre_pow = std::move(bytes);
    out.algorithm = algorithm;
    out.arrival_unix_ms = unix_now_ms();
    return true;
}

// Socket errors that simply mean "the connection is gone" and should not be
// reported as a failure of our own sending/receiving logic.
bool is_connection_gone(int code) {
    return code == WSAECONNRESET || code == WSAECONNABORTED || code == WSAESHUTDOWN ||
           code == WSAENOTCONN || code == WSAECONNREFUSED;
}

std::string wsa_error_text(int code) {
    std::string text = "Winsock error " + std::to_string(code);
    char* buffer = nullptr;
    // English on purpose, not LANG_NEUTRAL: neutral returns the OS display language, so on a
    // Russian Windows the operator's log was getting the system's Russian rendering of
    // "an existing connection was forcibly closed by the remote host" appended to the code -
    // in a project that is otherwise entirely English and published internationally. If the
    // English message table is not installed, FormatMessage fails and the numeric code stands on
    // its own, which is why the line above is built from the code first.
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(code), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<LPSTR>(&buffer), 0,
        nullptr);
    if (length != 0 && buffer != nullptr) {
        std::string detail(buffer, length);
        while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n' || detail.back() == ' ')) {
            detail.pop_back();
        }
        if (!detail.empty()) text += " (" + detail + ")";
    }
    if (buffer != nullptr) ::LocalFree(buffer);
    return text;
}

// Process-wide Winsock reference count. Function-local statics keep this out of
// namespace scope; the mutex makes start()/stop() from different clients safe.
std::mutex& wsa_mutex() {
    static std::mutex mutex;
    return mutex;
}

int& wsa_refcount() {
    static int count = 0;
    return count;
}

constexpr std::intptr_t kInvalidHandle = static_cast<std::intptr_t>(-1);

}  // namespace

// --- construction -----------------------------------------------------------

StratumClient::StratumClient(Config cfg) : cfg_(std::move(cfg)) {}

StratumClient::~StratumClient() {
    stop();
}

void StratumClient::set_job_callback(JobCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    job_cb_ = std::move(cb);
}

void StratumClient::set_log_callback(LogCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    log_cb_ = std::move(cb);
}

void StratumClient::set_disconnect_callback(DisconnectCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    disconnect_cb_ = std::move(cb);
}

// --- stats / callbacks ------------------------------------------------------

void StratumClient::add_stat(std::uint64_t Stats::*field, std::uint64_t amount) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.*field += amount;
}

StratumClient::Stats StratumClient::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

bool StratumClient::is_connected() const {
    return connected_.load();
}

void StratumClient::log(const std::string& message) {
    LogCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = log_cb_;
    }
    if (cb) {
        try {
            cb("[stratum] " + message);
        } catch (...) {
            // A throwing callback must never break the IO thread.
        }
    }
}

void StratumClient::emit_job(const Job& job) {
    JobCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = job_cb_;
    }
    if (cb) {
        try {
            cb(job);
        } catch (...) {
        }
    }
}

void StratumClient::emit_disconnect() {
    DisconnectCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = disconnect_cb_;
    }
    if (cb) {
        try {
            cb();
        } catch (...) {
        }
    }
}

// --- Winsock lifetime -------------------------------------------------------

bool StratumClient::wsa_acquire() {
    std::lock_guard<std::mutex> lock(wsa_mutex());
    if (wsa_refcount() == 0) {
        WSADATA data{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            log("WSAStartup failed: " + std::to_string(rc));
            return false;
        }
    }
    ++wsa_refcount();
    return true;
}

void StratumClient::wsa_release() {
    std::lock_guard<std::mutex> lock(wsa_mutex());
    if (wsa_refcount() > 0) {
        --wsa_refcount();
        if (wsa_refcount() == 0) ::WSACleanup();
    }
}

// --- lifecycle --------------------------------------------------------------

bool StratumClient::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        log("start() ignored: the client is already running");
        return true;
    }

    stopping_.store(false);
    use_edge_bits_form_.store(cfg_.use_edge_bits_submit_form);
    envelope_fallback_done_.store(cfg_.use_edge_bits_submit_form);

    if (cfg_.host.empty() || cfg_.user.empty()) {
        log("start() failed: host and user must both be set");
        running_.store(false);
        return false;
    }
    if (cfg_.keepalive_seconds <= 0) cfg_.keepalive_seconds = 10;
    if (cfg_.io_timeout_seconds <= 0) cfg_.io_timeout_seconds = 60;
    if (cfg_.reconnect_delay_seconds <= 0) cfg_.reconnect_delay_seconds = 1;

    if (!wsa_acquire()) {
        running_.store(false);
        return false;
    }

    if (!connect_and_login() || stopping_.load()) {
        close_socket();
        wsa_release();
        running_.store(false);
        return false;
    }

    try {
        io_thread_ = std::thread([this]() { io_loop(); });
    } catch (...) {
        log("start() failed: could not create the IO thread");
        close_socket();
        wsa_release();
        running_.store(false);
        return false;
    }
    return true;
}

void StratumClient::stop() {
    if (!running_.load()) {
        if (io_thread_.joinable() && io_thread_.get_id() != std::this_thread::get_id()) io_thread_.join();
        return;
    }

    if (io_thread_.joinable() && io_thread_.get_id() == std::this_thread::get_id()) {
        // stop() was called from one of our own callbacks (which run on the IO
        // thread): only request the shutdown, the owner must call stop() again
        // from another thread to join.
        stopping_.store(true);
        queue_cv_.notify_all();
        return;
    }

    stopping_.store(true);
    queue_cv_.notify_all();

    if (!io_thread_.joinable()) {
        // start() is still connecting on another thread. It observes stopping_
        // right after connect_and_login() and performs the full cleanup itself,
        // so stop() must not release Winsock underneath it.
        return;
    }

    const std::intptr_t handle = sock_.load();
    if (handle != kInvalidHandle) {
        // Unblocks select()/recv() in the IO thread without closing a handle it
        // is still using.
        ::shutdown(static_cast<SOCKET>(handle), SD_BOTH);
    }

    if (io_thread_.joinable()) io_thread_.join();

    close_socket();
    connected_.store(false);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        out_queue_.clear();
    }
    wsa_release();
    running_.store(false);
    log("stopped");
}

void StratumClient::close_socket() {
    const std::intptr_t handle = sock_.exchange(kInvalidHandle);
    if (handle != kInvalidHandle) ::closesocket(static_cast<SOCKET>(handle));
}

bool StratumClient::interruptible_sleep_seconds(int seconds) {
    if (seconds <= 0) return !stopping_.load();
    std::unique_lock<std::mutex> lock(queue_mutex_);
    // Returns true when the predicate (stopping_) became true, so `!stopped`
    // means the full delay elapsed.
    const bool stopped = queue_cv_.wait_for(lock, std::chrono::seconds(seconds), [this]() { return stopping_.load(); });
    return !stopped;
}

// --- connect / login --------------------------------------------------------

bool StratumClient::connect_and_login() {
    addrinfo hints{};
    hints.ai_family = AF_INET;  // GRIN pools are reached over IPv4; avoids IPv6 fallback stalls
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string port_text = std::to_string(cfg_.port);
    addrinfo* addresses = nullptr;
    const int gai = ::getaddrinfo(cfg_.host.c_str(), port_text.c_str(), &hints, &addresses);
    if (gai != 0 || addresses == nullptr) {
        if (addresses != nullptr) ::freeaddrinfo(addresses);
        log("DNS lookup failed for " + cfg_.host + ": " + std::string(gai_strerrorA(gai)));
        return false;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    for (addrinfo* entry = addresses; entry != nullptr; entry = entry->ai_next) {
        socket_handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (socket_handle == INVALID_SOCKET) continue;
        if (::connect(socket_handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) break;
        const int connect_error = ::WSAGetLastError();
        log("connect attempt to " + cfg_.host + ":" + port_text + " failed: " + wsa_error_text(connect_error));
        ::closesocket(socket_handle);
        socket_handle = INVALID_SOCKET;
    }
    ::freeaddrinfo(addresses);

    if (socket_handle == INVALID_SOCKET) {
        log("could not connect to " + cfg_.host + ":" + port_text);
        return false;
    }

    // SO_RCVTIMEO / SO_SNDTIMEO: the requirement's receive/send timeout. select()
    // normally prevents a blocking recv(); the timeouts are the safety net.
    const DWORD timeout_ms = static_cast<DWORD>(std::max(1, cfg_.io_timeout_seconds)) * 1000u;
    ::setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                 static_cast<int>(sizeof timeout_ms));
    ::setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                 static_cast<int>(sizeof timeout_ms));
    const BOOL no_delay = TRUE;  // keepalives and shares are tiny and latency-sensitive
    ::setsockopt(socket_handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay),
                 static_cast<int>(sizeof no_delay));

    sock_.store(static_cast<std::intptr_t>(socket_handle));
    rx_buffer_.clear();
    last_keepalive_ = SteadyClock::now();
    job_request_retry_pending_ = false;

    if (!send_all(build_login())) {
        log("failed to send the login request");
        close_socket();
        return false;
    }
    if (!send_all(build_getjobtemplate())) {
        log("failed to send the getjobtemplate request");
        close_socket();
        return false;
    }

    connected_.store(true);
    log("connected to " + cfg_.host + ":" + port_text + " as " + cfg_.user + " (agent " + cfg_.agent + ")");
    return true;
}

// --- request builders -------------------------------------------------------

std::string StratumClient::next_request_id() {
    // Ids are unique per request; the pool echoes them, which helps humans read
    // the log. They are never used to correlate state (classification is done
    // from the message body), so ordering across threads does not matter.
    return std::to_string(next_request_id_value_.fetch_add(1) + 1);
}

std::string StratumClient::build_login() {
    std::string message;
    message.reserve(192);
    message += "{\"id\":\"";
    message += next_request_id();
    message += "\",\"jsonrpc\":\"2.0\",\"method\":\"login\",\"params\":{\"login\":\"";
    message += json_escape(cfg_.user);
    message += "\",\"pass\":\"";
    message += json_escape(cfg_.pass);
    message += "\",\"agent\":\"";
    message += json_escape(cfg_.agent);
    message += "\"}}\n";
    return message;
}

std::string StratumClient::build_getjobtemplate() {
    std::string message;
    message.reserve(128);
    message += "{\"id\":\"";
    message += next_request_id();
    message += "\",\"jsonrpc\":\"2.0\",\"method\":\"getjobtemplate\",\"params\":{\"algorithm\":\"";
    message += json_escape(cfg_.algorithm);
    message += "\"}}\n";
    return message;
}

std::string StratumClient::build_keepalive() {
    std::string message;
    message.reserve(96);
    message += "{\"id\":\"";
    message += next_request_id();
    message += "\",\"jsonrpc\":\"2.0\",\"method\":\"keepalive\",\"params\":null}\n";
    return message;
}

std::string StratumClient::build_submit(const Job& job, const Solution& sol) {
    const bool edge_bits_form = use_edge_bits_form_.load();
    std::string message;
    message.reserve(512);
    message += "{\"id\":\"";
    message += next_request_id();
    message += "\",\"jsonrpc\":\"2.0\",\"method\":\"submit\",\"params\":{";
    if (edge_bits_form) {
        // {"edge_bits":32,"height":H,"job_id":J,"nonce":N,"pow":[e0,...,e41]}
        message += "\"edge_bits\":";
        message += std::to_string(sol.edge_bits);
        message += ",";
    }
    message += "\"height\":";
    message += std::to_string(job.height);
    message += ",\"job_id\":";
    message += std::to_string(job.job_id);
    message += ",\"nonce\":";
    message += std::to_string(sol.nonce);
    message += ",\"pow\":";
    if (edge_bits_form) {
        message += "[";
    } else {
        // {"pow":{"Cuckoo":[32,[e0,...,e41]]}}
        message += "{\"";
        message += json_escape(cfg_.algorithm);
        message += "\":[";
        message += std::to_string(sol.edge_bits);
        message += ",[";
    }
    for (std::size_t i = 0; i < kProofSize; ++i) {
        if (i != 0) message += ",";
        message += std::to_string(sol.proof[i]);
    }
    if (edge_bits_form) {
        message += "]";
    } else {
        message += "]]}";
    }
    message += "}}\n";
    return message;
}

// --- submit (any thread) ----------------------------------------------------

bool StratumClient::submit(const Job& job, const Solution& sol) {
    if (job.pre_pow.size() != kHeaderSizeExcludingNonce) {
        add_stat(&Stats::malformed);
        log("submit refused: pre_pow is " + std::to_string(job.pre_pow.size()) + " bytes, expected 238");
        return false;
    }
    if (!running_.load() || !connected_.load()) {
        log("submit dropped: the client is not connected");
        return false;
    }

    std::string message = build_submit(job, sol);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (out_queue_.size() >= kMaxQueuedSubmits) {
            log("submit dropped: the outbound queue is full");
            return false;
        }
        out_queue_.push_back(std::move(message));
    }
    queue_cv_.notify_all();
    return true;
}

// --- IO thread --------------------------------------------------------------

void StratumClient::io_loop() {
    log("IO thread started");
    bool attempted_connect = false;

    while (!stopping_.load()) {
        if (sock_.load() == kInvalidHandle) {
            if (!connect_and_login()) {
                if (attempted_connect) add_stat(&Stats::reconnects);
                attempted_connect = true;
                if (stopping_.load()) break;
                if (!interruptible_sleep_seconds(cfg_.reconnect_delay_seconds)) break;
                continue;
            }
        }
        attempted_connect = true;

        run_session();
        close_socket();
        connected_.store(false);
        if (stopping_.load()) break;

        std::size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            dropped = out_queue_.size();
            out_queue_.clear();  // a share queued for a dead connection is worthless
        }

        emit_disconnect();
        add_stat(&Stats::reconnects);
        std::string message = "connection lost; reconnecting in " + std::to_string(cfg_.reconnect_delay_seconds) + " s";
        if (dropped != 0) message += " (dropped " + std::to_string(dropped) + " queued submit(s))";
        log(message);

        if (!interruptible_sleep_seconds(cfg_.reconnect_delay_seconds)) break;
    }

    close_socket();
    connected_.store(false);
    log("IO thread exiting");
}

void StratumClient::run_session() {
    const SOCKET socket_handle = static_cast<SOCKET>(sock_.load());
    if (socket_handle == INVALID_SOCKET) return;

    rx_buffer_.clear();
    last_keepalive_ = SteadyClock::now();
    const int keepalive_seconds = std::max(1, cfg_.keepalive_seconds);

    while (!stopping_.load()) {
        // Serve queued submits first so they are not delayed behind a keepalive.
        if (!drain_outbound()) {
            log("closing the session after a send failure");
            return;
        }
        if (sock_.load() == kInvalidHandle) return;

        if (job_request_retry_pending_) {
            job_request_retry_pending_ = false;
            log("requesting a job template again after an error response");
            if (!send_all(build_getjobtemplate())) return;
        }

        const auto now = SteadyClock::now();
        if (now - last_keepalive_ >= std::chrono::seconds(keepalive_seconds)) {
            if (!send_all(build_keepalive())) {
                log("keepalive send failed");
                return;
            }
            last_keepalive_ = now;
        }

        // Wait for readable data. The timeout is capped at 200 ms so that newly
        // queued submits are picked up promptly (Windows select() cannot wait on
        // a condition variable) and so the keepalive deadline is honoured.
        long wait_usec = 200000L;
        const auto until_keepalive =
            std::chrono::seconds(keepalive_seconds) - (SteadyClock::now() - last_keepalive_);
        if (until_keepalive < std::chrono::milliseconds(200)) {
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(until_keepalive).count();
            wait_usec = static_cast<long>(micros);
            if (wait_usec < 1000L) wait_usec = 1000L;
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket_handle, &read_set);
        timeval wait{};
        wait.tv_sec = 0;
        wait.tv_usec = wait_usec;

        const int ready = ::select(0, &read_set, nullptr, nullptr, &wait);
        if (ready == SOCKET_ERROR) {
            const int select_error = ::WSAGetLastError();
            if (select_error == WSAEINTR) continue;
            if (stopping_.load()) return;
            if (is_connection_gone(select_error)) {
                log("the connection went away: " + wsa_error_text(select_error));
                return;
            }
            log("select failed: " + wsa_error_text(select_error));
            return;
        }
        if (ready == 0) continue;  // timeout: loop to service the queue/keepalive

        char buffer[16384];
        const int received = ::recv(socket_handle, buffer, static_cast<int>(sizeof buffer), 0);
        if (received == 0) {
            log("the server closed the connection");
            return;
        }
        if (received == SOCKET_ERROR) {
            const int recv_error = ::WSAGetLastError();
            if (recv_error == WSAEWOULDBLOCK || recv_error == WSAETIMEDOUT || recv_error == WSAEINTR) continue;
            // After stop() shuts the socket down, recv() reports WSAESHUTDOWN;
            // that is a normal shutdown, not an error worth logging.
            if (stopping_.load()) return;
            if (is_connection_gone(recv_error)) {
                log("the connection was closed by the peer: " + wsa_error_text(recv_error));
                return;
            }
            log("recv failed: " + wsa_error_text(recv_error));
            return;
        }

        rx_buffer_.append(buffer, static_cast<std::size_t>(received));
        process_buffer();
        if (rx_buffer_.size() > kMaxReceiveBufferBytes) {
            log("receive buffer overflow: no newline in " + std::to_string(rx_buffer_.size()) +
                " bytes; discarding buffered data");
            rx_buffer_.clear();
        }
    }
}

void StratumClient::process_buffer() {
    for (;;) {
        const std::size_t newline = rx_buffer_.find('\n');
        if (newline == std::string::npos) return;
        std::string line = rx_buffer_.substr(0, newline);
        rx_buffer_.erase(0, newline + 1);

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        if (start != 0) line.erase(0, start);
        if (line.empty()) continue;

        handle_line(line);
    }
}

bool StratumClient::drain_outbound() {
    for (;;) {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (out_queue_.empty()) return true;
            message = std::move(out_queue_.front());
            out_queue_.pop_front();
        }
        if (!send_all(message)) return false;
    }
}

bool StratumClient::send_all(const std::string& data) {
    std::size_t sent = 0;
    int consecutive_stalls = 0;
    while (sent < data.size()) {
        const SOCKET socket_handle = static_cast<SOCKET>(sock_.load());
        if (socket_handle == INVALID_SOCKET) return false;

        const std::size_t remaining = data.size() - sent;
        const std::size_t chunk = remaining < kSendChunkBytes ? remaining : kSendChunkBytes;
        const int written = ::send(socket_handle, data.data() + sent, static_cast<int>(chunk), 0);
        if (written == SOCKET_ERROR) {
            const int send_error = ::WSAGetLastError();
            if (stopping_.load()) return false;
            if (send_error == WSAEWOULDBLOCK || send_error == WSAETIMEDOUT || send_error == WSAEINTR) {
                // Blocking socket with SO_SNDTIMEO: the peer is not draining.
                // Retry a bounded number of times instead of spinning forever.
                if (++consecutive_stalls <= 3) continue;
                log("send stalled repeatedly; treating the connection as dead");
                return false;
            }
            if (is_connection_gone(send_error)) {
                log("the connection was closed by the peer: " + wsa_error_text(send_error));
                return false;
            }
            log("send failed: " + wsa_error_text(send_error));
            return false;
        }
        if (written <= 0) return false;
        sent += static_cast<std::size_t>(written);
        consecutive_stalls = 0;
    }
    return true;
}

// --- message handling (IO thread) -------------------------------------------

void StratumClient::handle_line(const std::string& line) {
    log("recv: " + line);

    // A `method:"job"` notification and a job returned as the `result` of
    // getjobtemplate both carry "pre_pow", so one parser covers both shapes.
    if (find_key_value(line, "pre_pow") != std::string::npos) {
        Job job;
        std::string error;
        if (parse_job_from_line(line, cfg_.algorithm, job, error)) {
            add_stat(&Stats::jobs);
            log("job height=" + std::to_string(job.height) + " job_id=" + std::to_string(job.job_id) +
                " difficulty=" + std::to_string(job.difficulty) +
                " pre_pow=" + std::to_string(job.pre_pow.size()) + " bytes");
            emit_job(job);
        } else {
            add_stat(&Stats::malformed);
            log("malformed job rejected: " + error);
        }
        return;
    }

    const std::string method = extract_method(line);
    const bool has_error = error_field_is_set(line);
    const JsonValueRef result = json_value_at(line, find_key_value(line, "result"));

    if (method == "submit" || (method.empty() && (has_error || result.kind != JsonValueKind::Missing))) {
        switch (classify_submit(line, has_error, result)) {
            case SubmitOutcome::Accepted:
                add_stat(&Stats::accepted);
                log("submit accepted by the pool");
                break;
            case SubmitOutcome::Rejected:
                add_stat(&Stats::rejected);
                log("submit rejected by the pool");
                consider_envelope_fallback(line);
                break;
            case SubmitOutcome::Stale:
                add_stat(&Stats::stale);
                log("submit stale: the job moved on before the share arrived");
                break;
            case SubmitOutcome::Malformed:
                add_stat(&Stats::rejected);
                add_stat(&Stats::malformed);
                log("submit rejected as malformed");
                consider_envelope_fallback(line);
                break;
            case SubmitOutcome::Unknown:
            default:
                log("submit response could not be classified as accepted/rejected");
                break;
        }
        return;
    }

    if (method == "login") {
        if (has_error) log("login rejected by the pool");
        else log("login accepted by the pool");
        return;
    }

    if (method == "getjobtemplate") {
        if (has_error) {
            log("getjobtemplate returned an error; will request a job again");
            job_request_retry_pending_ = true;
        } else {
            log("getjobtemplate response carried no job");
        }
        return;
    }

    if (method == "keepalive") {
        if (has_error) log("keepalive returned an error");
        return;
    }

    log("unhandled stratum message");
}

void StratumClient::consider_envelope_fallback(const std::string& server_text) {
    if (cfg_.use_edge_bits_submit_form) return;        // already on the alternative envelope
    if (envelope_fallback_done_.load()) return;        // never flip more than once
    const bool looks_like_envelope_problem =
        contains_ci(server_text, "malformed") || contains_ci(server_text, "invalid request") ||
        contains_ci(server_text, "invalid params") || contains_ci(server_text, "unsupported") ||
        contains_ci(server_text, "unknown method") || contains_ci(server_text, "cannot parse");
    if (!looks_like_envelope_problem) return;

    // The doc leaves the accepted pow envelope unconfirmed, so fall back to the
    // other form for *subsequent* submits. The rejected share itself is not
    // resent (that would risk a duplicate share on pools that accept both).
    envelope_fallback_done_.store(true);
    use_edge_bits_form_.store(true);
    log("pool rejected the Cuckoo-keyed pow envelope; switching to the edge_bits form for later submits");
}

}  // namespace grin
