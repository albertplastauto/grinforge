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

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
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
    int      fan_percent = -1;              // -1 = leave alone

    double   bench_seconds = 0.0;           // >0 = run without a pool, for measurement
    std::string bench_pre_pow_file;
    double   report_seconds = 5.0;

    uint16_t api_port = 4068;               // 0 disables the HTTP API
    bool     api_bind_all = false;          // expose the API to the LAN
};

void usage() {
    std::printf(
        "GrinForge — GRIN Cuckatoo32 miner (open source, 0%% dev fee)\n"
        "\n"
        "  --pool <host:port>            pool address (repeatable; later ones are failover)\n"
        "  --user <wallet.worker>        pool login\n"
        "  --pass <password>             pool password (default: x)\n"
        "  --device <n>                  CUDA device index (default 0)\n"
        "  --ntrims <n>                  edge trimming rounds (default 128)\n"
        "  --blocks <n> --tpb <n>        kernel launch geometry (default 128 128)\n"
        "  --temp-limit <c>              thermal guard, Celsius (default 83)\n"
        "  --stall-seconds <n>           restart the solver if no graph completes (default 180)\n"
        "  --min-lz <n>                  only submit cycles with >= n leading zero bits (default 0)\n"
        "  --power-limit <w>             set the GPU power limit in watts\n"
        "  --undervolt-core <mhz>        core clock offset for the pstate undervolt\n"
        "  --undervolt-voltage <uv>      voltage offset in microvolts (e.g. -50000)\n"
        "  --fan <percent>               fixed fan speed (0 = restore automatic)\n"
        "  --report <seconds>            dashboard interval (default 5)\n"
        "  --api-port <port>             HTTP monitoring API on 127.0.0.1 (default 4068, 0 = off)\n"
        "  --api-bind-all                expose the API on all interfaces (not just loopback)\n"
        "  --bench-seconds <s>           run the solver without a pool for s seconds\n"
        "  --bench-pre-pow <file>        file holding a captured pre_pow for --bench-seconds\n"
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
    else if (key == "fan") c.fan_percent = (int)num();
    else if (key == "report") c.report_seconds = std::strtod(value.c_str(), nullptr);
    else if (key == "api-port") c.api_port = (uint16_t)num();
    else if (key == "api-bind-all") c.api_bind_all = (num() != 0);
    else if (key == "bench-seconds") c.bench_seconds = std::strtod(value.c_str(), nullptr);
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
        else if (a == "--fan") c.fan_percent = std::atoi(next("--fan"));
        else if (a == "--report") c.report_seconds = std::strtod(next("--report"), nullptr);
        else if (a == "--api-port") c.api_port = (uint16_t)std::strtoul(next("--api-port"), nullptr, 10);
        else if (a == "--api-bind-all") c.api_bind_all = true;
        else if (a == "--bench-seconds") c.bench_seconds = std::strtod(next("--bench-seconds"), nullptr);
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

    std::atomic<uint64_t> attempts{0};
    std::atomic<uint64_t> solutions{0};
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<double>   best_lz{0.0};
    std::atomic<uint64_t> last_progress_unix_ms{0};
};

void log_line(const std::string& s) {
    std::printf("[grinforge] %s\n", s.c_str());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(console_handler, TRUE);

    Config cfg;
    cfg.pools.push_back(PoolSpec{});   // default pool
    if (!parse_args(cfg, argc, argv)) return 2;

    std::printf("GrinForge - GRIN Cuckatoo32 miner (0%% dev fee)\n");
    std::printf("================================================\n");

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
        if (cfg.fan_percent >= 0) {
            const auto r = grin::GpuControl::set_fan_percent((uint32_t)cfg.fan_percent);
            std::printf("fan %d%%: %s%s\n", cfg.fan_percent, r.ok ? "ok" : "FAILED",
                        r.detail.empty() ? "" : (" (" + r.detail + ")").c_str());
        }
    } else {
        std::printf("gpu control unavailable: %s\n", error.c_str());
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
        FILE* f = std::fopen(cfg.bench_pre_pow_file.c_str(), "rb");
        if (!f) { std::printf("cannot open %s\n", cfg.bench_pre_pow_file.c_str()); return 2; }
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
            for (char c : text) if (std::isxdigit((unsigned char)c)) hex.push_back(c);
        }
        if (hex.size() != GRIN_PRE_POW_SIZE * 2) {
            std::printf("could not find a %zu-char pre_pow in %s\n", GRIN_PRE_POW_SIZE * 2,
                        cfg.bench_pre_pow_file.c_str());
            return 2;
        }
        std::vector<uint8_t> pre(GRIN_PRE_POW_SIZE);
        for (size_t i = 0; i < pre.size(); ++i) {
            auto nib = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                return ch - 'A' + 10;
            };
            pre[i] = (uint8_t)((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
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
        uint64_t nonce = 0;
        uint64_t localGeneration = 0;
        double   lastSolveSeconds = 0.0;
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
                    nonce = 0;
                    log_line("new job height=" + std::to_string(job.height) +
                             " job_id=" + std::to_string(job.job_id) +
                             " difficulty=" + std::to_string(job.difficulty));
                }
            }

            grin::FoundSolution out[8];
            int nfound = 0;
            std::string err;
            const auto t0 = Clock::now();
            const auto st = solver->solve(nonce, out, 8, nfound, err);
            lastSolveSeconds = seconds_since(t0);
            shared.attempts.fetch_add(1);
            shared.last_progress_unix_ms.store(now_unix_ms());

            if (st == grin::SolveStatus::CudaError) {
                log_line("CUDA error: " + err + " - restarting solver");
                solver.reset();
                solver = std::make_unique<grin::LeanSolver>(scfg);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }

            for (int s = 0; s < nfound; ++s) {
                shared.solutions.fetch_add(1);
                // Independent verification before anything is sent to the pool.
                siphash_keys keys;
                uint8_t header[GRIN_HEADER_LEN];
                std::vector<uint8_t> pre;
                {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    pre = shared.job.pre_pow;
                }
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

                grin::Solution sol;
                sol.nonce = out[s].nonce;
                sol.edge_bits = EDGEBITS;
                for (int i = 0; i < PROOFSIZE; ++i) sol.proof[i] = out[s].proof[i];
                grin::Job submitJob;
                {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    submitJob = shared.job;
                }
                if (get_client()->submit(submitJob, sol)) {
                    shared.submitted.fetch_add(1);
                    log_line("submitted solution nonce=" + std::to_string(out[s].nonce) +
                             " lz=" + std::to_string(out[s].cyclehash_leading_zeros));
                } else {
                    log_line("submit queue rejected a solution (not connected?)");
                }
            }
            ++nonce;
        }
    });

    // ---- watchdog and dashboard -----------------------------------------
    auto lastReport = Clock::now();
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

        if (seconds_since(lastReport) >= cfg.report_seconds) {
            lastReport = Clock::now();
            const uint64_t attempts = shared.attempts.load();
            const double interval = cfg.report_seconds;
            const double gps = (double)(attempts - lastAttempts) / interval;
            lastAttempts = attempts;
            const auto s = get_client()->stats();

            std::printf("[%-8s] %6.3f GPS | attempts %-7llu sol %-4llu sub %-4llu "
                        "acc %-4llu rej %-4llu | ",
                        gps > 0 ? "mining" : "idle", gps, (unsigned long long)attempts,
                        (unsigned long long)shared.solutions.load(),
                        (unsigned long long)shared.submitted.load(),
                        (unsigned long long)s.accepted, (unsigned long long)s.rejected);
            if (telemetryOk) {
                std::printf("%2.0f C %5.1f/%.0f W VRAM %4llu MiB fan %3.0f%%\n",
                            t.temperature_c, t.power_w, t.power_limit_w,
                            (unsigned long long)(t.memory_used_bytes / (1024 * 1024)),
                            t.fan_percent);
            } else {
                std::printf("telemetry n/a\n");
            }
            std::fflush(stdout);
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
