// GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
// Copyright (c) 2026 albertplastauto
// SPDX-License-Identifier: MIT
//
// GrinForge — GRIN Cuckatoo32 GPU miner. Open source, no developer fee.
//
// Responsibilities of this file:
//   * configuration (CLI + key=value config file)
//   * the mining loop: job -> nonce iteration -> solve -> submit
//   * watchdog: thermal guard, stall recovery, pool failover
//   * console dashboard and graceful shutdown
//
// The heavy lifting lives elsewhere:
//   src/solver/    Cuckatoo32 lean GPU solver (CUDA)
//   src/stratum/   GRIN stratum client (see docs/stratum-protocol.md)
//   src/monitor/   NVML telemetry and NVAPI/nvidia-smi GPU control

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "host/http_api.hpp"
#include "monitor/gpu_control.hpp"
#include "monitor/telemetry.hpp"
#include "solver/lean_solver.hpp"
#include "solver/grin_verify.hpp"
#include "stratum/stratum_client.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

uint64_t now_unix_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::atomic<bool> g_stop{false};

BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct PoolSpec {
    std::string host = "grin.2miners.com";
    uint16_t    port = 3030;
};

struct Config {
    std::string user;                       // <wallet>.<worker>
    std::string pass = "x";
    std::vector<PoolSpec> pools;

    int      device = 0;
    uint32_t ntrims = 128;
    uint32_t blocks = 128;
    uint32_t tpb = 128;

    double   temp_limit_c = 83.0;
    double   stall_seconds = 180.0;
    uint64_t min_leading_zeros = 0;         // 0 = submit every valid cycle

    bool     apply_power_limit = false;
    uint32_t power_limit_w = 0;
    bool     apply_undervolt = false;
    grin::UndervoltPlan undervolt;
    int      lock_core_mhz = 0;             // >0 = lock the core clock (needs elevation)
    int      install_gpu_profile_mhz = 0;   // >0 = one-shot setup: apply the cap and exit
    int      fan_percent = -1;              // -1 = leave alone

    double   bench_seconds = 0.0;           // >0 = run without a pool, for measurement
    std::string bench_pre_pow_file;
    double   tune_seconds = 0.0;            // >0 = sweep GPU profiles and measure each
    double   report_seconds = 5.0;
    std::string log_file;                   // optional append-only copy of all output
    bool     stratum_debug = false;         // log raw pool protocol messages

    uint16_t api_port = 4068;               // 0 disables the HTTP API
    bool     api_bind_all = false;          // expose the API to the LAN

    // Wallet guard: if this list is non-empty, the miner refuses to start unless the
    // address inside --user is one of these. This is what makes a silently tampered
    // .bat file or config file unable to redirect mining to somebody else's wallet.
    std::vector<std::string> allowed_addresses;
};

// Split "<wallet>.<worker>". GRIN bech32 addresses contain no '.', so the first dot
// separates the address from the worker name.
inline void split_login(const std::string& login, std::string& address, std::string& worker) {
    const size_t dot = login.find('.');
    address = (dot == std::string::npos) ? login : login.substr(0, dot);
    worker = (dot == std::string::npos) ? std::string() : login.substr(dot + 1);
}

// GRIN mainnet addresses are bech32: "grin1" plus 58 characters, 63 in total.
inline bool looks_like_grin_address(const std::string& a) {
    if (a.size() != 63) return false;
    if (a.compare(0, 5, "grin1") != 0) return false;
    for (char c : a) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}

void usage() {
    std::printf(
        "GrinForge — GRIN Cuckatoo32 miner (open source, 0%% dev fee)\n"
        "\n"
        "  --pool <host:port>            pool address (repeatable; later ones are failover)\n"
        "  --user <wallet.worker>        pool login\n"
        "  --pass <password>             pool password (default: x)\n"
        "  --allow-address <grin1...>    wallet guard: refuse to start unless the address\n"
        "                                inside --user is exactly this one (repeatable)\n"
        "  --device <n>                  CUDA device index (default 0)\n"
        "  --ntrims <n>                  edge trimming rounds (default 128)\n"
        "  --blocks <n> --tpb <n>        kernel launch geometry (default 128 128)\n"
        "  --temp-limit <c>              thermal guard, Celsius (default 83)\n"
        "  --stall-seconds <n>           restart the solver if no graph completes (default 180)\n"
        "  --min-lz <n>                  only submit cycles with >= n leading zero bits (default 0)\n"
        "  --power-limit <w>             set the GPU power limit in watts\n"
        "  --undervolt-core <mhz>        core clock offset for the pstate undervolt\n"
        "  --undervolt-voltage <uv>      voltage offset in microvolts (e.g. -50000)\n"
        "  --lock-core <mhz>             lock the core clock (measured optimum here: 2500;\n"
        "                                needs elevation, same as --power-limit)\n"
        "  --install-gpu-profile <mhz>   one-shot setup, no mining: cap the core clock and\n"
        "                                print how to keep it across reboots (needs elevation)\n"
        "  --fan <percent>               fixed fan speed (0 = restore automatic)\n"
        "  --report <seconds>            dashboard interval (default 5)\n"
        "  --log-file <path>             also append all console output to this file, so a\n"
        "                                visible window still leaves a log behind\n"
        "  --stratum-debug               log every raw pool message. Off by default: those\n"
        "                                lines carry a full 476-hex pre_pow per job and are\n"
        "                                only useful when diagnosing the protocol itself\n"
        "  --api-port <port>             HTTP monitoring API on 127.0.0.1 (default 4068, 0 = off)\n"
        "  --api-bind-all                expose the API on all interfaces (not just loopback)\n"
        "  --bench-seconds <s>           run the solver without a pool for s seconds\n"
        "  --bench-pre-pow <file>        file holding a captured pre_pow for --bench-seconds\n"
        "  --tune <s>                    sweep GPU profiles (power limit, clock locks), measure\n"
        "                                GPS and GPS/W for each, then restore defaults\n"
        "  --config <file>               read key=value settings from a file\n"
        "  --help\n");
}

bool parse_pool(const std::string& text, PoolSpec& out) {
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos) { out.host = text; return !text.empty(); }
    out.host = text.substr(0, colon);
    const long port = std::strtol(text.c_str() + colon + 1, nullptr, 10);
    if (port <= 0 || port > 65535 || out.host.empty()) return false;
    out.port = (uint16_t)port;
    return true;
}

void apply_setting(Config& c, const std::string& key, const std::string& value) {
    auto num = [&]() { return std::strtoll(value.c_str(), nullptr, 10); };
    if (key == "pool") { PoolSpec p; if (parse_pool(value, p)) c.pools.push_back(p); }
    else if (key == "user") c.user = value;
    else if (key == "allow-address") c.allowed_addresses.push_back(value);
    else if (key == "pass") c.pass = value;
    else if (key == "device") c.device = (int)num();
    else if (key == "ntrims") c.ntrims = (uint32_t)num();
    else if (key == "blocks") c.blocks = (uint32_t)num();
    else if (key == "tpb") c.tpb = (uint32_t)num();
    else if (key == "temp-limit") c.temp_limit_c = std::strtod(value.c_str(), nullptr);
    else if (key == "stall-seconds") c.stall_seconds = std::strtod(value.c_str(), nullptr);
    else if (key == "min-lz") c.min_leading_zeros = (uint64_t)num();
    else if (key == "power-limit") { c.apply_power_limit = true; c.power_limit_w = (uint32_t)num(); }
    else if (key == "undervolt-core") { c.apply_undervolt = true; c.undervolt.core_clock_offset_mhz = (int)num(); }
    else if (key == "undervolt-voltage") { c.apply_undervolt = true; c.undervolt.voltage_offset_uv = (int)num(); }
    else if (key == "lock-core") c.lock_core_mhz = (int)num();
    else if (key == "install-gpu-profile") c.install_gpu_profile_mhz = (int)num();
    else if (key == "fan") c.fan_percent = (int)num();
    else if (key == "report") c.report_seconds = std::strtod(value.c_str(), nullptr);
    else if (key == "log-file") c.log_file = value;
    else if (key == "stratum-debug") c.stratum_debug = true;
    else if (key == "api-port") c.api_port = (uint16_t)num();
    else if (key == "api-bind-all") c.api_bind_all = (num() != 0);
    else if (key == "bench-seconds") c.bench_seconds = std::strtod(value.c_str(), nullptr);
    else if (key == "tune-seconds") c.tune_seconds = std::strtod(value.c_str(), nullptr);
    else if (key == "bench-pre-pow") c.bench_pre_pow_file = value;
    else std::printf("warning: unknown config key '%s'\n", key.c_str());
}

bool load_config_file(Config& c, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::printf("cannot open config file: %s\n", path.c_str()); return false; }
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        const size_t hash = s.find_first_of("#;");
        if (hash != std::string::npos) s = s.substr(0, hash);
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        auto trim = [](std::string& t) {
            const char* ws = " \t\r\n";
            const size_t b = t.find_first_not_of(ws);
            const size_t e = t.find_last_not_of(ws);
            t = (b == std::string::npos) ? std::string() : t.substr(b, e - b + 1);
        };
        trim(key); trim(val);
        if (!key.empty()) apply_setting(c, key, val);
    }
    std::fclose(f);
    return true;
}

bool parse_args(Config& c, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::printf("%s requires a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else if (a == "--config") { if (!load_config_file(c, next("--config"))) return false; }
        else if (a == "--pool") { PoolSpec p; if (!parse_pool(next("--pool"), p)) { std::printf("bad pool\n"); return false; } c.pools.push_back(p); }
        else if (a == "--user") c.user = next("--user");
        else if (a == "--allow-address") c.allowed_addresses.push_back(next("--allow-address"));
        else if (a == "--pass") c.pass = next("--pass");
        else if (a == "--device") c.device = std::atoi(next("--device"));
        else if (a == "--ntrims") c.ntrims = (uint32_t)std::strtoul(next("--ntrims"), nullptr, 10);
        else if (a == "--blocks") c.blocks = (uint32_t)std::strtoul(next("--blocks"), nullptr, 10);
        else if (a == "--tpb") c.tpb = (uint32_t)std::strtoul(next("--tpb"), nullptr, 10);
        else if (a == "--temp-limit") c.temp_limit_c = std::strtod(next("--temp-limit"), nullptr);
        else if (a == "--stall-seconds") c.stall_seconds = std::strtod(next("--stall-seconds"), nullptr);
        else if (a == "--min-lz") c.min_leading_zeros = std::strtoull(next("--min-lz"), nullptr, 10);
        else if (a == "--power-limit") { c.apply_power_limit = true; c.power_limit_w = (uint32_t)std::strtoul(next("--power-limit"), nullptr, 10); }
        else if (a == "--undervolt-core") { c.apply_undervolt = true; c.undervolt.core_clock_offset_mhz = std::atoi(next("--undervolt-core")); }
        else if (a == "--undervolt-voltage") { c.apply_undervolt = true; c.undervolt.voltage_offset_uv = std::atoi(next("--undervolt-voltage")); }
        else if (a == "--lock-core") c.lock_core_mhz = std::atoi(next("--lock-core"));
        else if (a == "--install-gpu-profile") c.install_gpu_profile_mhz = std::atoi(next("--install-gpu-profile"));
        else if (a == "--fan") c.fan_percent = std::atoi(next("--fan"));
        else if (a == "--report") c.report_seconds = std::strtod(next("--report"), nullptr);
        else if (a == "--log-file") c.log_file = next("--log-file");
        else if (a == "--stratum-debug") c.stratum_debug = true;
        else if (a == "--api-port") c.api_port = (uint16_t)std::strtoul(next("--api-port"), nullptr, 10);
        else if (a == "--api-bind-all") c.api_bind_all = true;
        else if (a == "--bench-seconds") c.bench_seconds = std::strtod(next("--bench-seconds"), nullptr);
        else if (a == "--tune") c.tune_seconds = std::strtod(next("--tune"), nullptr);
        else if (a == "--bench-pre-pow") c.bench_pre_pow_file = next("--bench-pre-pow");
        else { std::printf("unknown option: %s (try --help)\n", a.c_str()); return false; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shared mining state
// ---------------------------------------------------------------------------

struct Shared {
    std::mutex mutex;
    grin::Job job;
    bool has_job = false;
    uint64_t job_generation = 0;

    std::atomic<uint64_t> attempts{0};        // graph attempts that COMPLETED
    std::atomic<uint64_t> graphs_overloaded{0};
    std::atomic<uint64_t> graphs_failed{0};
    std::atomic<uint64_t> solutions{0};
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<double>   best_lz{0.0};
    std::atomic<uint64_t> last_progress_unix_ms{0};
};

std::string timestamp_str() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const long ms = (long)(std::chrono::duration_cast<std::chrono::milliseconds>(
                               now.time_since_epoch())
                               .count() %
                           1000);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Console output
// ---------------------------------------------------------------------------

// True when stdout is an interactive console rather than a redirected file. The dashboard
// only repaints in place on a console: a log file must stay line-oriented and greppable.
bool stdout_is_console() {
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    return h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0;
}

// ANSI colours are used only on a real console, and only after that console has been switched
// into virtual-terminal mode: without the switch Windows prints the escape sequences literally,
// which makes a coloured dashboard worse than a plain one, not better.
bool g_color = false;

bool enable_console_colors() {
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode) == 0) return false;
    if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) return false;
    return true;
}

const char* kColTitle = "\x1b[1;36m";   // bold cyan
const char* kColLabel = "\x1b[36m";     // cyan
const char* kColGood  = "\x1b[32m";     // green
const char* kColWarn  = "\x1b[33m";     // yellow
const char* kColBad   = "\x1b[31m";     // red
const char* kColDim   = "\x1b[2m";      // dim

std::string col(const char* code, const std::string& text) {
    if (!g_color) return text;
    return std::string(code) + text + "\x1b[0m";
}

// Optional append-only log file. Running with a visible window must not cost us the log,
// so every line printed to the console is written here as well.
std::FILE* g_log_sink = nullptr;
bool g_stratum_debug = false;   // when false, raw pool messages are dropped from the output

void log_sink_open(const std::string& path) {
    if (path.empty()) return;
    g_log_sink = std::fopen(path.c_str(), "a");
    if (g_log_sink != nullptr) {
        std::fprintf(g_log_sink, "--- GrinForge session started %s ---\n", timestamp_str().c_str());
        std::fflush(g_log_sink);
    }
}

void log_sink_write(const std::string& line) {
    if (g_log_sink == nullptr) return;
    std::fputs(line.c_str(), g_log_sink);
    std::fputc('\n', g_log_sink);
    std::fflush(g_log_sink);
}

// Render a box table whose bars are derived from the cell widths, so the output cannot come
// out ragged the way hand-written padding eventually does.
//
// Colours are applied per cell, and the widths are always measured on the PLAIN text: wrapping
// a padded cell in escape codes would otherwise make every coloured column one padding step too
// wide, which is exactly how coloured tables end up looking worse than plain ones.
std::string render_table(const std::vector<std::string>& header,
                         const std::vector<std::vector<std::string>>& rows,
                         const std::vector<std::vector<const char*>>& colors = {}) {
    std::vector<size_t> w(header.size());
    for (size_t i = 0; i < header.size(); ++i) w[i] = header[i].size();
    for (const auto& r : rows) {
        for (size_t i = 0; i < r.size() && i < w.size(); ++i) {
            if (r[i].size() > w[i]) w[i] = r[i].size();
        }
    }
    const auto bar = [&]() {
        std::string s = "+";
        for (size_t i = 0; i < w.size(); ++i) s += std::string(w[i] + 2, '-') + "+";
        return s + "\n";
    };
    const auto row = [&](const std::vector<std::string>& r, const std::vector<const char*>* cs) {
        std::string s = "|";
        for (size_t i = 0; i < w.size(); ++i) {
            const std::string cell = i < r.size() ? r[i] : std::string();
            const std::string pad(w[i] - cell.size(), ' ');
            const char* c = (cs != nullptr && i < cs->size()) ? (*cs)[i] : nullptr;
            s += " ";
            if (c != nullptr && g_color) s += std::string(c) + cell + "\x1b[0m" + pad;
            else s += cell + pad;
            s += " |";
        }
        return s + "\n";
    };
    std::string out = bar();
    if (g_color) {
        out += row(header, nullptr);   // header styling handled by the caller's colour choice
    } else {
        out += row(header, nullptr);
    }
    out += bar();
    for (size_t i = 0; i < rows.size(); ++i) {
        out += row(rows[i], i < colors.size() ? &colors[i] : nullptr);
    }
    return out + bar();
}

// Number of lines currently occupied by the console dashboard, so the next repaint can erase
// exactly that block instead of scrolling.
int g_dashboard_lines = 0;

void dashboard_clear() {
    if (g_dashboard_lines <= 0) return;
    std::printf("\x1b[%dA\x1b[0J", g_dashboard_lines);
    g_dashboard_lines = 0;
    std::fflush(stdout);
}

void log_line(const std::string& s) {
    // Raw pool traffic is dropped unless explicitly asked for. Each job message carries the
    // whole 476-hex pre_pow, so these lines dominate the log by size while telling the operator
    // nothing the summarised "job height=... job_id=..." line does not; they earn their place
    // only when the protocol itself is under investigation.
    if (!g_stratum_debug && s.find("recv:") != std::string::npos) return;
    // Timestamps are not cosmetic here: the pool drops the connection every couple
    // of minutes and a share submitted just before a drop never gets a response, so
    // correlating submits with drops is the only way to tell those apart.
    const std::string line = "[" + timestamp_str() + "] " + s;
    // Erase the dashboard block first: otherwise a log line would be printed below it and the
    // next repaint would overwrite the log instead of the block.
    dashboard_clear();
    // Tint the events worth noticing, so a wall of stratum chatter does not hide the one line
    // that matters.
    const char* tint = nullptr;
    if (s.find("FAILED") != std::string::npos || s.find("CUDA error") != std::string::npos ||
        s.find("rejected") != std::string::npos) {
        tint = kColBad;
    } else if (s.find("stale") != std::string::npos || s.find("WARNING") != std::string::npos) {
        tint = kColWarn;
    } else if (s.find("accepted") != std::string::npos) {
        tint = kColGood;
    }
    if (tint != nullptr && g_color) std::printf("%s\n", col(tint, line).c_str());
    else std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    log_sink_write(line);
}

// Pull a 238-byte pre_pow out of a captured stratum line or a bare hex string.
// Shared by --bench-seconds and --tune.
bool load_pre_pow_file(const std::string& path, std::vector<uint8_t>& pre, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open " + path; return false; }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    const std::string key = "\"pre_pow\":";
    size_t at = text.find(key);
    std::string hex;
    if (at != std::string::npos) {
        at += key.size();
        while (at < text.size() && (text[at] == ' ' || text[at] == '"')) ++at;
        size_t end = at;
        while (end < text.size() && std::isxdigit((unsigned char)text[end])) ++end;
        hex = text.substr(at, end - at);
    } else {
        for (char c : text) {
            if (std::isxdigit((unsigned char)c)) hex.push_back(c);
        }
    }
    if (hex.size() != GRIN_PRE_POW_SIZE * 2) {
        error = "could not find a " + std::to_string(GRIN_PRE_POW_SIZE * 2) +
                "-char pre_pow in " + path;
        return false;
    }
    auto nib = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return ch - 'A' + 10;
    };
    pre.resize(GRIN_PRE_POW_SIZE);
    for (size_t i = 0; i < pre.size(); ++i) {
        pre[i] = (uint8_t)((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(console_handler, TRUE);

    Config cfg;
    if (!parse_args(cfg, argc, argv)) return 2;
    // Only fall back to the built-in pool when the operator supplied none.
    // (Adding it unconditionally produced a two-entry list containing the same
    // pool twice, which made failover rotate onto the pool that just failed.)
    if (cfg.pools.empty()) cfg.pools.push_back(PoolSpec{});

    // ---- wallet guard -----------------------------------------------------
    // Downloaded miner bundles are known to ship .bat files carrying somebody
    // else's address; if such a file is run, the mining silently pays a stranger.
    // With --allow-address the miner refuses to start unless the address it is
    // about to mine to is exactly the one the operator intends.
    // Modes that never talk to a pool do not need a wallet: a dry benchmark, the
    // GPU tuning sweep, and the one-shot GPU profile installer.
    if (cfg.bench_seconds <= 0.0 && cfg.tune_seconds <= 0.0 && cfg.install_gpu_profile_mhz <= 0) {
        if (cfg.user.empty()) {
            std::printf("--user <wallet.worker> is required (or --bench-seconds for a dry run)\n");
            return 2;
        }
        std::string walletAddress, workerName;
        split_login(cfg.user, walletAddress, workerName);

        std::printf("mining to address: %s\n", walletAddress.c_str());
        if (!workerName.empty()) std::printf("worker name:       %s\n", workerName.c_str());
        if (!looks_like_grin_address(walletAddress)) {
            std::printf("WARNING: this does not look like a GRIN mainnet address "
                        "(expected \"grin1\" + 58 chars = 63 total).\n"
                        "         Check very carefully what you are mining to.\n");
        }

        if (!cfg.allowed_addresses.empty()) {
            bool allowed = false;
            for (const auto& a : cfg.allowed_addresses) {
                if (a == walletAddress) { allowed = true; break; }
            }
            if (!allowed) {
                std::printf(
                    "\nREFUSING TO START: the address in --user is not in the allowlist.\n"
                    "  --user address : %s\n", walletAddress.c_str());
                for (const auto& a : cfg.allowed_addresses) {
                    std::printf("  allowed        : %s\n", a.c_str());
                }
                std::printf(
                    "This guard exists so that a tampered .bat or config file cannot silently\n"
                    "redirect your mining to somebody else's wallet. Pass the address you\n"
                    "actually intend to mine to via --allow-address, or drop the allowlist.\n");
                return 2;
            }
            std::printf("wallet guard:      OK, address matches the allowlist (%zu entr%s)\n",
                        cfg.allowed_addresses.size(),
                        cfg.allowed_addresses.size() == 1 ? "y" : "ies");
        } else {
            std::printf("wallet guard:      no allowlist set; add\n"
                        "                     --allow-address %s\n"
                        "                   to make a silent wallet change impossible.\n",
                        walletAddress.c_str());
        }
    }

    log_sink_open(cfg.log_file);
    g_color = enable_console_colors();
    g_stratum_debug = cfg.stratum_debug;

    // Startup banner, presented the way GPU miners usually are: a configuration block first,
    // then a timestamped log. Everything here is known before the solver is created, so a
    // reader can see at a glance what is mined, to which wallet, and under which limits.
    std::printf("%s\n", col(kColTitle, "+--------------------------------------------------------------+").c_str());
    std::printf("%s\n", col(kColTitle, "|           GrinForge - GRIN Cuckatoo32 GPU miner              |").c_str());
    std::printf("%s\n", col(kColGood,  "|                0 % developer fee, MIT licensed               |").c_str());
    std::printf("%s\n", col(kColTitle, "+--------------------------------------------------------------+").c_str());
    std::printf("Algorithm:         Cuckatoo32 lean (CUDA)\n");
    std::printf("DevFee:            0 %%\n");
    std::printf("Server:\n");
    for (size_t i = 0; i < cfg.pools.size(); ++i) {
        std::printf("  %-15s %s:%u%s\n", i == 0 ? "host:" : "failover:",
                    cfg.pools[i].host.c_str(), (unsigned)cfg.pools[i].port,
                    i == 0 ? "" : "");
    }
    std::printf("  %-15s %s\n", "user:", cfg.user.empty() ? "(none)" : cfg.user.c_str());
    std::printf("  %-15s %s\n", "password:", cfg.pass.c_str());
    std::printf("Wallet guard:      %s\n",
                cfg.allowed_addresses.empty() ? "not set (add --allow-address)"
                                              : "enforced (address must match the allowlist)");
    std::printf("Solver:            ntrims=%u blocks=%u tpb=%u\n", cfg.ntrims, cfg.blocks,
                cfg.tpb);
    std::printf("Temperature limit: %.0f C\n", cfg.temp_limit_c);
    std::printf("HTTP API:          %s\n",
                cfg.api_port == 0 ? "off"
                                  : ("http://127.0.0.1:" + std::to_string(cfg.api_port) +
                                     "/stat")
                                        .c_str());
    std::printf("--------------------------------------------------------------\n");
    // The banner above goes to the console; record the same essentials in the log file, which
    // otherwise would show sessions without saying what they were mining or with which limits.
    log_sink_write("config: algorithm=Cuckatoo32-lean devfee=0% pool=" + cfg.pools[0].host + ":" +
                   std::to_string(cfg.pools[0].port) + " user=" + cfg.user + " ntrims=" +
                   std::to_string(cfg.ntrims) + " blocks=" + std::to_string(cfg.blocks) +
                   " tpb=" + std::to_string(cfg.tpb) + " temp_limit=" +
                   std::to_string((int)cfg.temp_limit_c) + " api_port=" +
                   std::to_string(cfg.api_port) +
                   (cfg.allowed_addresses.empty() ? " wallet_guard=off" : " wallet_guard=on"));

    // ---- device -----------------------------------------------------------
    std::string devName;
    uint64_t vram = 0;
    int cmaj = 0, cmin = 0;
    const int deviceCount = grin::LeanSolver::device_count();
    std::printf("CUDA devices: %d\n", deviceCount);
    if (deviceCount == 0) {
        std::printf("no CUDA device found (is the NVIDIA driver installed?)\n");
        return 1;
    }
    for (int i = 0; i < deviceCount; ++i) {
        if (grin::LeanSolver::device_info(i, devName, vram, cmaj, cmin)) {
            std::printf("  [%d] %s, %llu MiB, sm_%d%d\n", i, devName.c_str(),
                        (unsigned long long)(vram / (1024 * 1024)), cmaj, cmin);
        }
    }
    if (cfg.device < 0 || cfg.device >= deviceCount) {
        std::printf("device index %d is out of range\n", cfg.device);
        return 2;
    }
    grin::LeanSolver::device_info(cfg.device, devName, vram, cmaj, cmin);

    // ---- telemetry and control -------------------------------------------
    std::string error;
    if (grin::Telemetry::init(error)) {
        std::printf("telemetry backend: %s, %zu device(s)\n", grin::Telemetry::backend_name(),
                    grin::Telemetry::device_count());
        // Bind this run to the driver it is measured on: every number in the project is
        // tied to a specific driver (docs/validated-environment.md), so the log should
        // state it instead of leaving it to documentation.
        const std::string driverVersion = grin::Telemetry::driver_version();
        std::printf("nvidia driver:     %s\n",
                    driverVersion.empty() ? "unknown" : driverVersion.c_str());
    } else {
        std::printf("telemetry unavailable: %s (mining continues without monitoring)\n",
                    error.c_str());
    }

    if (grin::GpuControl::init((unsigned)cfg.device, error)) {
        std::printf("gpu control backend: %s\n", grin::GpuControl::backend_name());
        if (cfg.apply_power_limit) {
            grin::PowerLimitRange range;
            if (grin::GpuControl::query_power_limit_range(range, error)) {
                std::printf("power limit range: %u..%u W (default %u)\n", range.min_mw / 1000,
                            range.max_mw / 1000, range.default_mw / 1000);
            }
            const auto r = grin::GpuControl::set_power_limit_watts(cfg.power_limit_w);
            std::printf("power limit %u W: %s%s\n", cfg.power_limit_w, r.ok ? "ok" : "FAILED",
                        r.detail.empty() ? "" : (" (" + r.detail + ")").c_str());
        }
        if (cfg.apply_undervolt) {
            const auto r = grin::GpuControl::set_undervolt(cfg.undervolt);
            std::printf("undervolt core=%+d MHz voltage=%+d uV: %s%s\n",
                        cfg.undervolt.core_clock_offset_mhz, cfg.undervolt.voltage_offset_uv,
                        r.ok ? "ok" : "FAILED", r.detail.empty() ? "" : (" (" + r.detail + ")").c_str());
        }
        if (cfg.lock_core_mhz > 0) {
            // Measured optimum on this card: hashrate is flat between 2500 and
            // 2800 MHz while power is not, so locking at 2500 gives the same GPS at
            // the best GPS/W. Requires elevation, like --power-limit.
            grin::UndervoltPlan plan;
            plan.lock_core_clock = true;
            plan.core_clock_min_mhz = (uint32_t)cfg.lock_core_mhz;
            plan.core_clock_max_mhz = (uint32_t)cfg.lock_core_mhz;
            const auto r = grin::GpuControl::set_undervolt(plan);
            std::printf("core clock lock %d MHz: %s%s\n", cfg.lock_core_mhz, r.ok ? "ok" : "FAILED",
                        r.detail.empty() ? "" : (" (" + r.detail + ")").c_str());
        }
        if (cfg.fan_percent >= 0) {
            const auto r = grin::GpuControl::set_fan_percent((uint32_t)cfg.fan_percent);
            std::printf("fan %d%%: %s%s\n", cfg.fan_percent, r.ok ? "ok" : "FAILED",
                        r.detail.empty() ? "" : (" (" + r.detail + ")").c_str());
        }
    } else {
        std::printf("gpu control unavailable: %s\n", error.c_str());
    }

    // ---- one-shot GPU profile setup --------------------------------------
    // Deliberately a separate mode that applies the cap and EXITS: changing clocks
    // needs administrator rights, and we do not want the mining process itself to
    // hold them. So the operator runs this once per machine, elevated, and then
    // mines unprivileged as usual.
    if (cfg.install_gpu_profile_mhz > 0) {
        std::printf("\ninstalling GPU profile: core clock cap %d MHz\n", cfg.install_gpu_profile_mhz);
        grin::UndervoltPlan plan;
        plan.lock_core_clock = true;
        // 0 would mean "no lower bound" to nvidia-smi, but GpuControl deliberately
        // validates against the card's real envelope (300..3200 MHz here) and refuses
        // anything outside it, so use the hardware minimum instead of 0. The card then
        // still downclocks when idle, which is the whole point of capping rather than
        // pinning.
        plan.core_clock_min_mhz = 300;
        plan.core_clock_max_mhz = (uint32_t)cfg.install_gpu_profile_mhz;
        const auto r = grin::GpuControl::set_undervolt(plan);
        if (!r.ok) {
            std::printf("FAILED to apply the cap: %s\n%s\n", r.detail.c_str(),
                        r.needs_elevation
                            ? "Re-run this command from an elevated console (Run as administrator / accept the UAC prompt)."
                            : "");
            grin::Telemetry::shutdown();
            return 2;
        }
        std::printf("applied: %s\n", r.detail.c_str());
        std::printf(
            "\nThe driver forgets this on reboot. To keep it, run either\n"
            "  * install-gpu-clock-task.bat   (creates a logon task, needs elevation), or\n"
            "  * gpu-lock-2500.bat yourself at every logon.\n"
            "Undo with gpu-unlock.bat (or nvidia-smi --reset-gpu-clocks).\n"
            "Note: this is an efficiency setting, not a speed setting - measured hashrate\n"
            "is the same with and without it (see docs/performance-notes.md, section 7).\n");
        grin::Telemetry::shutdown();
        return 0;
    }

    // ---- solver -----------------------------------------------------------
    grin::SolverConfig scfg;
    scfg.device = cfg.device;
    scfg.ntrims = cfg.ntrims;
    scfg.blocks = cfg.blocks;
    scfg.tpb = cfg.tpb;
    std::printf("solver: ntrims=%u blocks=%u tpb=%u, device image ~%llu MiB\n", scfg.ntrims,
                scfg.blocks, scfg.tpb,
                (unsigned long long)(grin::LeanSolver::device_bytes_for(scfg) / (1024 * 1024)));

    auto solver = std::make_unique<grin::LeanSolver>(scfg);

    // ---- benchmark-only mode ---------------------------------------------
    if (cfg.bench_seconds > 0.0) {
        if (cfg.bench_pre_pow_file.empty()) {
            std::printf("--bench-seconds requires --bench-pre-pow <file with a captured job>\n");
            return 2;
        }
        std::vector<uint8_t> pre;
        std::string loadError;
        if (!load_pre_pow_file(cfg.bench_pre_pow_file, pre, loadError)) {
            std::printf("%s\n", loadError.c_str());
            return 2;
        }
        if (!solver->set_pre_pow(pre.data(), pre.size(), error)) {
            std::printf("set_pre_pow failed: %s\n", error.c_str());
            return 2;
        }

        std::printf("\nbenchmark mode: %.0f s\n", cfg.bench_seconds);
        const auto t0 = Clock::now();
        uint64_t nonce = 0, attempts = 0, verified = 0;
        while (seconds_since(t0) < cfg.bench_seconds && !g_stop.load()) {
            grin::FoundSolution out[8];
            int nfound = 0;
            const auto st = solver->solve(nonce, out, 8, nfound, error);
            ++attempts;
            if (st == grin::SolveStatus::CudaError) {
                std::printf("CUDA error on nonce %llu: %s\n", (unsigned long long)nonce, error.c_str());
                return 1;
            }
            for (int s = 0; s < nfound; ++s) {
                siphash_keys keys;
                uint8_t header[GRIN_HEADER_LEN];
                grin_build_header(pre.data(), pre.size(), out[s].nonce, header);
                grin::grin_setheader(header, (uint32_t)GRIN_HEADER_LEN, &keys);
                if (grin::grin_verify((const word_t*)out[s].proof, &keys) == grin::POW_OK) ++verified;
            }
            ++nonce;
        }
        const double elapsed = seconds_since(t0);
        const auto last = solver->last_run();
        std::printf("attempts=%llu elapsed=%.2fs gps=%.4f verified_solutions=%llu "
                    "last(trim=%.1fms cycles=%.1fms edges=%llu)\n",
                    (unsigned long long)attempts, elapsed, attempts / elapsed,
                    (unsigned long long)verified, last.trim_ms, last.find_cycles_ms,
                    (unsigned long long)last.edges_after_trim);
        if (grin::GpuControl::backend_name()[0] != 'n') grin::GpuControl::restore_defaults();
        grin::Telemetry::shutdown();
        return 0;
    }

    // ---- GPU tuning sweep -------------------------------------------------
    // "Deliberate optimisation": apply one profile at a time, measure the real
    // hashrate and power, print GPS and GPS/W, then restore defaults. Nothing is
    // assumed, and every refusal is reported instead of being papered over.
    //
    // On this particular GPU the honest expected result is "no measurable
    // difference": the card's MINIMUM power limit (100 W) is above this workload's
    // draw (60-76 W), so the power slider cannot bite, and lean Cuckatoo32 is
    // latency-bound rather than bandwidth-bound, so memory clocks do not matter.
    // Measuring that is the point - it is why the real gains have to come from the
    // kernels, not from card settings.
    if (cfg.tune_seconds > 0.0) {
        if (cfg.bench_pre_pow_file.empty()) {
            std::printf("--tune requires --bench-pre-pow <file with a captured job>\n");
            return 2;
        }
        std::vector<uint8_t> pre;
        std::string loadError;
        if (!load_pre_pow_file(cfg.bench_pre_pow_file, pre, loadError)) {
            std::printf("%s\n", loadError.c_str());
            return 2;
        }
        if (!solver->set_pre_pow(pre.data(), pre.size(), error)) {
            std::printf("set_pre_pow failed: %s\n", error.c_str());
            return 2;
        }

        struct Profile {
            const char* name;
            int power_w;
            int core_lock_mhz;
        };
        const Profile profiles[] = {
            {"stock (no limits)",  0,    0},
            {"power limit 100 W",  100,  0},
            {"power limit 120 W",  120,  0},
            {"core lock 1500 MHz", 0,    1500},
            {"core lock 2500 MHz", 0,    2500},
            {"core lock 2800 MHz", 0,    2800},
        };
        const size_t profileCount = sizeof(profiles) / sizeof(profiles[0]);

        std::printf("\nGPU tuning sweep: %.0f s per profile, %zu profiles\n",
                    cfg.tune_seconds, profileCount);
        std::printf("%-21s %-30s %9s %8s %10s\n", "profile", "applied", "GPS", "watts", "GPS/W");
        for (size_t i = 0; i < profileCount; ++i) {
            const Profile& p = profiles[i];

            // stock restores defaults; the rest apply exactly one change
            grin::ControlResult res{true, "defaults", false};
            if (p.power_w > 0) {
                res = grin::GpuControl::set_power_limit_watts((uint32_t)p.power_w);
            } else if (p.core_lock_mhz > 0) {
                grin::UndervoltPlan plan;
                plan.lock_core_clock = true;
                plan.core_clock_min_mhz = (uint32_t)p.core_lock_mhz;
                plan.core_clock_max_mhz = (uint32_t)p.core_lock_mhz;
                res = grin::GpuControl::set_undervolt(plan);
            } else {
                grin::GpuControl::restore_defaults();
            }

            std::string appliedText;
            if (p.power_w == 0 && p.core_lock_mhz == 0) appliedText = "ok (defaults)";
            else if (res.ok) appliedText = "ok";
            else if (res.needs_elevation) appliedText = "NOT APPLIED (needs elevation)";
            else appliedText = "NOT APPLIED";

            if (!res.ok) {
                std::printf("%-21s %-30s %9s %8s %10s  %s\n", p.name, appliedText.c_str(), "-", "-",
                            "-", res.detail.substr(0, 70).c_str());
                continue;
            }

            const auto t0 = Clock::now();
            uint64_t attempts = 0;
            uint64_t nonce = 0;
            double watts = 0.0;
            double peakTemp = 0.0;
            int samples = 0;
            // solve() blocks for ~18 s, so polling once per completed graph gives too
            // few samples for an honest average. Sample immediately before and after
            // each graph instead, and report how many samples the average is based on.
            auto samplePower = [&]() {
                grin::GpuTelemetry t;
                std::string terr;
                if (grin::Telemetry::read((unsigned)cfg.device, t, terr)) {
                    watts += t.power_w;
                    peakTemp = std::max(peakTemp, t.temperature_c);
                    ++samples;
                }
            };
            while (seconds_since(t0) < cfg.tune_seconds && !g_stop.load()) {
                samplePower();
                grin::FoundSolution out[8];
                int nfound = 0;
                std::string err;
                const auto st = solver->solve(nonce++, out, 8, nfound, err);
                samplePower();
                if (st != grin::SolveStatus::Ok) continue;
                ++attempts;
            }
            const double secs = seconds_since(t0);
            const double gps = secs > 0.0 ? (double)attempts / secs : 0.0;
            const double avgW = samples ? watts / samples : 0.0;
            std::printf("%-21s %-30s %9.4f %8.1f %10.5f   n=%d %2.0fC\n", p.name,
                        appliedText.c_str(), gps, avgW, avgW > 0.0 ? gps / avgW : 0.0, samples,
                        peakTemp);
            std::fflush(stdout);
        }

        grin::GpuControl::restore_defaults();
        std::printf("all profiles restored to defaults\n");
        grin::Telemetry::shutdown();
        return 0;
    }

    // ---- pool mode --------------------------------------------------------
    if (cfg.user.empty()) {
        std::printf("--user <wallet.worker> is required (or --bench-seconds for a dry run)\n");
        return 2;
    }
    std::printf("pools:\n");
    for (const auto& p : cfg.pools) std::printf("  %s:%u\n", p.host.c_str(), p.port);
    std::printf("wallet/worker: %s\n\n", cfg.user.c_str());

    Shared shared;
    shared.last_progress_unix_ms.store(now_unix_ms());

    std::mutex clientMutex;
    auto make_client = [&](size_t poolIndex) -> std::shared_ptr<grin::StratumClient> {
        grin::StratumClient::Config cc;
        cc.host = cfg.pools[poolIndex].host;
        cc.port = cfg.pools[poolIndex].port;
        cc.user = cfg.user;
        cc.pass = cfg.pass;
        cc.agent = "grinforge/0.1";
        auto client = std::make_shared<grin::StratumClient>(cc);
        client->set_log_callback([](const std::string& s) { log_line("stratum " + s); });
        client->set_job_callback([&shared](const grin::Job& j) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.job = j;
            shared.has_job = true;
            ++shared.job_generation;
        });
        client->set_disconnect_callback([&shared]() {
            log_line("stratum disconnected, client will reconnect");
        });
        return client;
    };

    size_t poolIndex = 0;
    std::shared_ptr<grin::StratumClient> client = make_client(poolIndex);
    // Failover replaces the client object; every other thread must go through
    // this accessor so it never touches a half-swapped pointer.
    auto get_client = [&]() -> std::shared_ptr<grin::StratumClient> {
        std::lock_guard<std::mutex> lock(clientMutex);
        return client;
    };
    if (!client->start()) {
        std::printf("could not connect to %s:%u\n", cfg.pools[poolIndex].host.c_str(),
                    cfg.pools[poolIndex].port);
        if (cfg.pools.size() > 1) {
            std::printf("trying failover pools...\n");
            for (size_t i = 1; i < cfg.pools.size(); ++i) {
                poolIndex = i;
                client = make_client(poolIndex);
                if (client->start()) break;
                std::printf("  %s:%u failed\n", cfg.pools[i].host.c_str(), cfg.pools[i].port);
            }
        }
        if (!get_client()->is_connected()) {
            std::printf("no pool available\n");
            return 1;
        }
    }
    std::printf("mining on %s:%u\n", cfg.pools[poolIndex].host.c_str(), cfg.pools[poolIndex].port);

    // ---- HTTP monitoring API --------------------------------------------
    // Shape mirrors GMiner's /stat so existing dashboards and scripts work.
    const auto processStart = Clock::now();
    grin::HttpApi api;
    if (cfg.api_port != 0) {
        api.start(
            cfg.api_port, cfg.api_bind_all,
            [&]() -> std::string {
                grin::GpuTelemetry t;
                std::string terr;
                const bool haveT = grin::Telemetry::read((unsigned)cfg.device, t, terr);
                const auto s = get_client()->stats();
                const uint64_t attempts = shared.attempts.load();
                const double up = seconds_since(processStart);
                const double gps = up > 0.0 ? (double)attempts / up : 0.0;

                std::string j;
                j.reserve(1024);
                j += "{\"miner\":\"GrinForge 0.1\"";
                j += ",\"uptime\":" + std::to_string((unsigned long long)up);
                j += ",\"server\":\"" + cfg.pools[poolIndex].host + ":" +
                     std::to_string(cfg.pools[poolIndex].port) + "\"";
                j += ",\"user\":\"" + cfg.user + "\"";
                j += ",\"algorithm\":\"Cuckatoo32\"";
                j += ",\"speed\":" + std::to_string(gps);
                j += ",\"speed_unit\":\"GPS\"";
                j += ",\"attempts\":" + std::to_string((unsigned long long)attempts);
                j += ",\"graphs_overloaded\":" + std::to_string((unsigned long long)shared.graphs_overloaded.load());
                j += ",\"graphs_failed\":" + std::to_string((unsigned long long)shared.graphs_failed.load());
                j += ",\"solutions\":" + std::to_string((unsigned long long)shared.solutions.load());
                j += ",\"total_submitted_shares\":" + std::to_string((unsigned long long)shared.submitted.load());
                j += ",\"total_accepted_shares\":" + std::to_string((unsigned long long)s.accepted);
                j += ",\"total_rejected_shares\":" + std::to_string((unsigned long long)s.rejected);
                j += ",\"total_stale_shares\":" + std::to_string((unsigned long long)s.stale);
                j += ",\"best_leading_zeros\":" + std::to_string((unsigned long long)shared.best_lz.load());
                j += ",\"connected\":";
                j += get_client()->is_connected() ? "true" : "false";
                j += ",\"devices\":[{\"gpu_id\":" + std::to_string(cfg.device);
                j += ",\"name\":\"" + devName + "\"";
                j += ",\"speed\":" + std::to_string(gps);
                j += ",\"accepted_shares\":" + std::to_string((unsigned long long)s.accepted);
                j += ",\"rejected_shares\":" + std::to_string((unsigned long long)s.rejected);
                if (haveT) {
                    j += ",\"temperature\":" + std::to_string((int)t.temperature_c);
                    j += ",\"fan\":" + std::to_string((int)t.fan_percent);
                    j += ",\"power_usage\":" + std::to_string(t.power_w);
                    j += ",\"power_limit\":" + std::to_string(t.power_limit_w);
                    j += ",\"memory_used_mib\":" +
                         std::to_string((unsigned long long)(t.memory_used_bytes / (1024 * 1024)));
                    j += ",\"memory_total_mib\":" +
                         std::to_string((unsigned long long)(t.memory_total_bytes / (1024 * 1024)));
                    j += ",\"core_clock\":" + std::to_string((int)t.core_clock_mhz);
                    j += ",\"memory_clock\":" + std::to_string((int)t.memory_clock_mhz);
                    j += ",\"utilization_gpu\":" + std::to_string((int)t.utilization_gpu_percent);
                    j += ",\"throttle_reasons\":" + std::to_string((unsigned long long)t.throttle_reasons);
                }
                j += "}]}";
                return j;
            },
            [](const std::string& m) { log_line(m); });
    }

    // ---- mining thread ---------------------------------------------------
    std::atomic<uint64_t> currentGeneration{0};
    std::thread miner([&]() {
        // Start each job at a pseudo-random point in the nonce space rather than at 0.
        // A graph takes ~18 s while the pool's job window is comparable in length, so a
        // miner restarted inside the same job window replays the same nonces, finds
        // exactly the same cycle, and submits a duplicate - seen in the log as
        // "Duplicate share". Measured on this machine: 1 of 26 submits in one day, and
        // every one of them followed a restart. Starting elsewhere makes that collision
        // unlikely without changing anything else.
        std::random_device rd;
        std::mt19937_64 rng(((uint64_t)rd() << 32) ^ (uint64_t)rd() ^
                            (uint64_t)std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count());
        const uint64_t nonceBase = rng();
        log_line("nonce space start: " + std::to_string(nonceBase) +
                 " (randomised so a restart cannot replay the same nonces)");
        uint64_t nonce = nonceBase;
        uint64_t localGeneration = 0;
        double   lastSolveSeconds = 0.0;
        bool     jobSeen = false;
        // The job the CURRENT solve was started for. A graph takes ~18 s while the
        // pool issues a new job roughly every 60 s, so reading shared.job after the
        // solve would verify and submit against the WRONG pre_pow: the local
        // verification would then fail with a misleading message and a genuinely
        // valid solution would be thrown away.
        grin::Job activeJob;
        bool     haveActiveJob = false;
        while (!g_stop.load()) {
            grin::Job job;
            bool have = false;
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                if (shared.has_job && shared.job_generation != localGeneration) {
                    job = shared.job;
                    localGeneration = shared.job_generation;
                    currentGeneration.store(localGeneration);
                    have = true;
                }
            }
            if (have) {
                std::string err;
                if (!solver->set_pre_pow(job.pre_pow.data(), job.pre_pow.size(), err)) {
                    log_line("bad job pre_pow: " + err);
                } else {
                    nonce = nonceBase;
                    jobSeen = true;
                    activeJob = job;
                    haveActiveJob = true;
                    log_line("new job height=" + std::to_string(job.height) +
                             " job_id=" + std::to_string(job.job_id) +
                             " difficulty=" + std::to_string(job.difficulty));
                }
            }
            if (!jobSeen) {
                // Nothing to mine yet. Spinning here called solve() on an
                // unconfigured solver thousands of times per second, which showed
                // up as 16168 bogus "failed graphs" in the first second and could
                // not be distinguished from real CUDA failures.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            grin::FoundSolution out[8];
            int nfound = 0;
            std::string err;
            const auto t0 = Clock::now();
            const auto st = solver->solve(nonce, out, 8, nfound, err);
            lastSolveSeconds = seconds_since(t0);

            // Only a graph that actually completed counts towards the hashrate.
            // Counting failed attempts produced a reported "6440 GPS" while the
            // GPU sat at 0% utilisation, because every solve() returned instantly
            // with Overloaded. Progress is likewise only recorded on success, so
            // the stall watchdog can still fire when nothing completes.
            if (st == grin::SolveStatus::Ok) {
                shared.attempts.fetch_add(1);
                shared.last_progress_unix_ms.store(now_unix_ms());
            } else if (st == grin::SolveStatus::Overloaded) {
                const uint64_t n = shared.graphs_overloaded.fetch_add(1) + 1;
                if (n == 1 || n % 200 == 0) {
                    log_line("graph overloaded " + std::to_string(n) +
                             " time(s): too many edges survived trimming - raise --ntrims");
                }
            } else if (st == grin::SolveStatus::Aborted) {
                // asked to stop by the watchdog; not a failure
            } else {
                shared.graphs_failed.fetch_add(1);
            }

            if (st == grin::SolveStatus::CudaError) {
                log_line("CUDA error: " + err + " - restarting solver");
                solver.reset();
                solver = std::make_unique<grin::LeanSolver>(scfg);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }

            for (int s = 0; s < nfound; ++s) {
                shared.solutions.fetch_add(1);
                if (!haveActiveJob) continue;

                // Verify and submit against the job this graph was actually solved
                // for, never against whatever the newest job happens to be.
                siphash_keys keys;
                uint8_t header[GRIN_HEADER_LEN];
                const std::vector<uint8_t>& pre = activeJob.pre_pow;
                if (pre.size() != GRIN_PRE_POW_SIZE) continue;
                grin_build_header(pre.data(), pre.size(), out[s].nonce, header);
                grin::grin_setheader(header, (uint32_t)GRIN_HEADER_LEN, &keys);
                const int rc = grin::grin_verify((const word_t*)out[s].proof, &keys);
                if (rc != grin::POW_OK) {
                    log_line(std::string("locally found solution FAILED verification: ") +
                             grin::verify_str(rc));
                    continue;
                }
                if (out[s].cyclehash_leading_zeros > (uint64_t)shared.best_lz.load()) {
                    shared.best_lz.store((double)out[s].cyclehash_leading_zeros);
                }
                if (out[s].cyclehash_leading_zeros < cfg.min_leading_zeros) continue;

                // Note whether a newer job has arrived in the meantime: the pool may
                // then count the share as stale, which is worth seeing in the log.
                bool stale = false;
                {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    stale = shared.has_job && shared.job_generation != localGeneration;
                }

                grin::Solution sol;
                sol.nonce = out[s].nonce;
                sol.edge_bits = EDGEBITS;
                for (int i = 0; i < PROOFSIZE; ++i) sol.proof[i] = out[s].proof[i];

                if (get_client()->submit(activeJob, sol)) {
                    shared.submitted.fetch_add(1);
                    log_line("submitted solution height=" + std::to_string(activeJob.height) +
                             " job_id=" + std::to_string(activeJob.job_id) +
                             " nonce=" + std::to_string(out[s].nonce) +
                             " lz=" + std::to_string(out[s].cyclehash_leading_zeros) +
                             (stale ? " (a newer job has arrived; may be stale)" : ""));
                } else {
                    log_line("submit queue rejected a solution (not connected?)");
                }
            }
            ++nonce;
        }
    });

    // ---- watchdog and dashboard -----------------------------------------
    auto lastReport = Clock::now();
    bool clockAdviceGiven = false;
    double energyKwh = 0.0;      // accumulated from measured power, shown in the dashboard
    uint64_t lastAttempts = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        grin::GpuTelemetry t;
        bool telemetryOk = grin::Telemetry::read((unsigned)cfg.device, t, error);

        // Thermal guard: stop issuing new graphs until the card cools down.
        if (telemetryOk && t.temperature_c >= cfg.temp_limit_c) {
            log_line("thermal guard: " + std::to_string((int)t.temperature_c) +
                     " C >= limit " + std::to_string((int)cfg.temp_limit_c) + " C, pausing 5 s");
            solver->request_abort();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            solver->clear_abort();
        }

        // Stall detection: a healthy C32 graph takes well under a minute.
        const double sinceProgress = (now_unix_ms() - shared.last_progress_unix_ms.load()) / 1000.0;
        if (cfg.stall_seconds > 0 && sinceProgress > cfg.stall_seconds) {
            log_line("watchdog: no graph completed in " + std::to_string((int)sinceProgress) +
                     " s - restarting the solver");
            solver->request_abort();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            solver->clear_abort();
            shared.last_progress_unix_ms.store(now_unix_ms());
        }

        // Failover if the pool has been unreachable for a while.
        if (cfg.pools.size() > 1 && !get_client()->is_connected()) {
            static int disconnectedPolls = 0;
            if (++disconnectedPolls > 20) {
                disconnectedPolls = 0;
                poolIndex = (poolIndex + 1) % cfg.pools.size();
                log_line("failover: switching to " + cfg.pools[poolIndex].host + ":" +
                         std::to_string(cfg.pools[poolIndex].port));
                std::shared_ptr<grin::StratumClient> old;
                {
                    std::lock_guard<std::mutex> lock(clientMutex);
                    old = client;
                    client = make_client(poolIndex);
                }
                old->stop();                 // joins the old IO thread outside the lock
                get_client()->start();
            }
        }

        // One-time advisory: nvidia-smi cannot report whether a clock cap is active
        // (Max Clocks always shows the hardware maximum), so judge it from the clock
        // actually observed while the GPU is loaded. Never blocks anything.
        if (!clockAdviceGiven && telemetryOk && seconds_since(processStart) > 90.0 &&
            t.utilization_gpu_percent > 50.0) {
            clockAdviceGiven = true;
            if (t.core_clock_mhz > 2600.0) {
                log_line("core clock is running at ~" + std::to_string((int)t.core_clock_mhz) +
                         " MHz. Measured on this GPU the hashrate is identical at ~2500 MHz "
                         "with ~12% better GPS/W. To cap it (one time, needs elevation): "
                         "grinforge.exe --install-gpu-profile 2500, then "
                         "install-gpu-clock-task.bat to keep it across reboots.");
            } else {
                log_line("core clock is running at ~" + std::to_string((int)t.core_clock_mhz) +
                         " MHz, which is at or below the measured optimum - the efficiency "
                         "profile appears to be active.");
            }
        }

        if (seconds_since(lastReport) >= cfg.report_seconds) {
            lastReport = Clock::now();
            const uint64_t attempts = shared.attempts.load();
            const double interval = cfg.report_seconds;
            const double gps = (double)(attempts - lastAttempts) / interval;
            lastAttempts = attempts;
            energyKwh += (telemetryOk ? t.power_w : 0.0) / 1000.0 * interval / 3600.0;
            const auto s = get_client()->stats();

            // One greppable line, always: the log file must stay machine-readable even while
            // the console gets the pretty version.
            char dash[512];
            std::snprintf(dash, sizeof(dash),
                          "%-8s %6.3f G/s | graphs %llu | sol %llu | sub %llu | acc %llu | "
                          "rej %llu | fail %llu | %2.0f C %5.1f/%.0f W fan %3.0f%% core %4.0f MHz "
                          "VRAM %llu MiB",
                          gps > 0 ? "mining" : "idle", gps, (unsigned long long)attempts,
                          (unsigned long long)shared.solutions.load(),
                          (unsigned long long)shared.submitted.load(),
                          (unsigned long long)s.accepted, (unsigned long long)s.rejected,
                          (unsigned long long)shared.graphs_failed.load(),
                          telemetryOk ? t.temperature_c : 0.0, telemetryOk ? t.power_w : 0.0,
                          telemetryOk ? t.power_limit_w : 0.0, telemetryOk ? t.fan_percent : 0.0,
                          telemetryOk ? t.core_clock_mhz : 0.0,
                          (unsigned long long)(telemetryOk ? t.memory_used_bytes / (1024 * 1024)
                                                           : 0));

            if (stdout_is_console()) {
                // Two small tables plus a session line, the shape GPU miners usually use.
                std::string gpuShort = devName;
                const std::string vendor = "NVIDIA GeForce ";
                if (gpuShort.rfind(vendor, 0) == 0) gpuShort = gpuShort.substr(vendor.size());

                char cell[64];
                std::vector<std::string> row1, row2;
                row1.push_back(std::to_string(cfg.device));
                row1.push_back(gpuShort);
                std::snprintf(cell, sizeof(cell), "%.4f G/s", gps);
                row1.push_back(cell);
                std::snprintf(cell, sizeof(cell), "%llu / %llu",
                              (unsigned long long)s.accepted, (unsigned long long)s.rejected);
                row1.push_back(cell);
                std::snprintf(cell, sizeof(cell), "%.1f W", telemetryOk ? t.power_w : 0.0);
                row1.push_back(cell);
                std::snprintf(cell, sizeof(cell), "%.3f mG/W",
                              (telemetryOk && t.power_w > 0.0) ? gps * 1000.0 / t.power_w : 0.0);
                row1.push_back(cell);

                row2.push_back(std::to_string(cfg.device));
                row2.push_back(gpuShort);
                std::snprintf(cell, sizeof(cell), "%.0f C", telemetryOk ? t.temperature_c : 0.0);
                row2.push_back(cell);
                std::snprintf(cell, sizeof(cell), "%.0f %%", telemetryOk ? t.fan_percent : 0.0);
                row2.push_back(cell);
                std::snprintf(cell, sizeof(cell), "%.0f MHz",
                              telemetryOk ? t.core_clock_mhz : 0.0);
                row2.push_back(cell);
                std::snprintf(cell, sizeof(cell), "%.0f MHz",
                              telemetryOk ? t.memory_clock_mhz : 0.0);
                row2.push_back(cell);
                std::snprintf(cell, sizeof(cell), "%llu / %llu MiB",
                              (unsigned long long)(telemetryOk ? t.memory_used_bytes / (1024 * 1024)
                                                               : 0),
                              (unsigned long long)(vram / (1024 * 1024)));
                row2.push_back(cell);

                // Colour carries meaning rather than decoration: green is healthy, yellow wants
                // a look, red needs action. Only the data cells are coloured; the frame and
                // labels stay quiet so the eye lands on the numbers.
                std::vector<std::vector<const char*>> colors1(
                    1, std::vector<const char*>(row1.size(), nullptr));
                std::vector<std::vector<const char*>> colors2(
                    1, std::vector<const char*>(row2.size(), nullptr));
                colors1[0][1] = kColLabel;                                     // GPU name
                colors1[0][2] = (gps > 0.0) ? kColGood : kColWarn;             // speed
                colors1[0][3] = (s.accepted >= s.rejected) ? kColGood : kColWarn;
                colors1[0][5] = kColGood;                                     // efficiency
                const double tempC = telemetryOk ? t.temperature_c : 0.0;
                colors2[0][1] = kColLabel;
                colors2[0][2] = (tempC >= cfg.temp_limit_c - 8.0) ? kColBad
                               : (tempC >= 65.0)                 ? kColWarn
                                                                 : kColGood;
                colors2[0][5] = kColDim;
                colors2[0][6] = kColDim;

                std::string block =
                    render_table({"ID", "GPU", "Speed", "Shares a/r", "Power", "Efficiency"},
                                 {row1}, colors1) +
                    render_table({"ID", "GPU", "Temp", "Fan", "Core", "Mem", "VRAM"}, {row2},
                                 colors2);

                const int ups = (int)seconds_since(processStart);
                std::snprintf(dash, sizeof(dash),
                              "Pool %s:%u | uptime %dd %02d:%02d:%02d | graphs %llu | "
                              "energy %.3f kWh",
                              cfg.pools[0].host.c_str(), (unsigned)cfg.pools[0].port,
                              ups / 86400, (ups / 3600) % 24, (ups / 60) % 60, ups % 60,
                              (unsigned long long)attempts, energyKwh);
                block += col(kColDim, std::string(dash)) + "\n";

                dashboard_clear();
                std::printf("%s", block.c_str());
                g_dashboard_lines = (int)std::count(block.begin(), block.end(), '\n');
                std::fflush(stdout);
            } else {
                std::printf("[%s] %s\n", timestamp_str().c_str(), dash);
                std::fflush(stdout);
            }
            log_sink_write("[" + timestamp_str() + "] " + dash);
        }
    }

    std::printf("\nshutting down...\n");
    g_stop.store(true);
    solver->request_abort();
    if (miner.joinable()) miner.join();
    api.stop();
    get_client()->stop();
    if (grin::GpuControl::backend_name()[0] != 'n') grin::GpuControl::restore_defaults();
    grin::Telemetry::shutdown();
    std::printf("stopped. attempts=%llu solutions=%llu submitted=%llu\n",
                (unsigned long long)shared.attempts.load(),
                (unsigned long long)shared.solutions.load(),
                (unsigned long long)shared.submitted.load());
    return 0;
}
