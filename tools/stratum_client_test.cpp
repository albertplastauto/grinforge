// GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
// Copyright (c) 2026 albertplastauto
// SPDX-License-Identifier: MIT
//
// stratum_client_test.cpp -- live protocol regression test for the GRIN stratum
// client.
//
// Usage:
//   stratum_client_test <host:port> <wallet.worker> [seconds]
//
// Connects, logs in, requests a job template, then prints every job and every
// log line for the requested duration (default 20 s) and finally prints the
// client statistics. It deliberately never submits anything: no valid
// Cuckatoo32 solution exists in this build yet.
//
// Use it the same way as tools/stratum_probe.py:
//   stratum_client_test grin.2miners.com:3030 <wallet>.TEST 30

#include "../src/stratum/stratum_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Callbacks run on the client's IO thread while main() also prints periodic
// statistics, so all console output goes through one lock.
std::mutex g_print_mutex;

void print_line(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << text << '\n';
    std::cout.flush();
}

std::string to_hex(const std::uint8_t* data, std::size_t count) {
    static const char* const kHexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out += kHexDigits[(data[i] >> 4) & 0x0F];
        out += kHexDigits[data[i] & 0x0F];
    }
    return out;
}

std::string hex_prefix(const std::vector<std::uint8_t>& bytes, std::size_t count) {
    const std::size_t n = bytes.size() < count ? bytes.size() : count;
    if (n == 0) return std::string("<empty>");
    return to_hex(bytes.data(), n);
}

std::string hex_suffix(const std::vector<std::uint8_t>& bytes, std::size_t count) {
    const std::size_t n = bytes.size() < count ? bytes.size() : count;
    if (n == 0) return std::string("<empty>");
    return to_hex(bytes.data() + (bytes.size() - n), n);
}

void print_stats(const char* label, const grin::StratumClient::Stats& stats, bool connected) {
    print_line(std::string(label) + " jobs=" + std::to_string(stats.jobs) +
               " accepted=" + std::to_string(stats.accepted) +
               " rejected=" + std::to_string(stats.rejected) +
               " stale=" + std::to_string(stats.stale) +
               " malformed=" + std::to_string(stats.malformed) +
               " reconnects=" + std::to_string(stats.reconnects) +
               " connected=" + (connected ? "yes" : "no"));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <host:port> <wallet.worker> [seconds]\n",
                    (argc > 0 && argv[0] != nullptr) ? argv[0] : "stratum_client_test");
        std::printf("example: %s grin.2miners.com:3030 <wallet>.TEST 20\n",
                    (argc > 0 && argv[0] != nullptr) ? argv[0] : "stratum_client_test");
        return 2;
    }

    const std::string host_port = argv[1];
    const std::string user = argv[2];
    double seconds = 20.0;
    if (argc >= 4) {
        seconds = std::atof(argv[3]);
        if (!(seconds > 0.0)) seconds = 20.0;
    }

    std::string host = host_port;
    std::uint16_t port = 3030;
    const std::size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        const long parsed = std::strtol(host_port.c_str() + colon + 1, nullptr, 10);
        if (parsed > 0 && parsed <= 65535) port = static_cast<std::uint16_t>(parsed);
    }

    grin::StratumClient::Config cfg;
    cfg.host = host;
    cfg.port = port;
    cfg.user = user;
    cfg.pass = "x";
    cfg.agent = "grinforge/0.1";
    cfg.algorithm = "Cuckoo";
    cfg.keepalive_seconds = 10;
    cfg.io_timeout_seconds = 60;
    cfg.reconnect_delay_seconds = 1;
    cfg.use_edge_bits_submit_form = false;

    grin::StratumClient client(cfg);

    client.set_log_callback([](const std::string& line) { print_line("LOG  " + line); });

    client.set_job_callback([](const grin::Job& job) {
        print_line("JOB  height=" + std::to_string(job.height) +
                   " job_id=" + std::to_string(job.job_id) +
                   " difficulty=" + std::to_string(job.difficulty) +
                   " algorithm=" + job.algorithm +
                   " pre_pow_bytes=" + std::to_string(job.pre_pow.size()) +
                   " first16=" + hex_prefix(job.pre_pow, 16) +
                   " last16=" + hex_suffix(job.pre_pow, 16) +
                   " arrival_unix_ms=" + std::to_string(job.arrival_unix_ms));
    });

    client.set_disconnect_callback([]() { print_line("EVENT  disconnected; the client will reconnect"); });

    print_line("# connecting to " + host + ":" + std::to_string(port) + " as " + user +
               " (no shares will be submitted)");

    if (!client.start()) {
        print_line("# FAILED: start() could not connect/log in");
        return 1;
    }

    print_line("# connected; collecting jobs for " + std::to_string(static_cast<int>(seconds)) + " s");

    const auto start_time = std::chrono::steady_clock::now();
    const auto deadline = start_time + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
    auto next_report = start_time + std::chrono::seconds(5);

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (std::chrono::steady_clock::now() >= next_report) {
            next_report += std::chrono::seconds(5);
            print_stats("STAT", client.stats(), client.is_connected());
        }
    }

    client.stop();

    print_line("");
    print_line("final stats:");
    const grin::StratumClient::Stats final_stats = client.stats();
    print_line("  jobs        = " + std::to_string(final_stats.jobs));
    print_line("  accepted    = " + std::to_string(final_stats.accepted));
    print_line("  rejected    = " + std::to_string(final_stats.rejected));
    print_line("  stale       = " + std::to_string(final_stats.stale));
    print_line("  malformed   = " + std::to_string(final_stats.malformed));
    print_line("  reconnects  = " + std::to_string(final_stats.reconnects));
    print_line("  connected   = " + std::string(client.is_connected() ? "yes" : "no"));
    print_line("# done");
    return 0;
}
