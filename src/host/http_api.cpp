// GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
// Copyright (c) 2026 albertplastauto
// SPDX-License-Identifier: MIT
//
#include "http_api.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "Ws2_32.lib")

namespace grin {
namespace {

// Winsock lifetime shared by every user in this process.
std::atomic<int> g_wsaRefs{0};

bool wsa_acquire() {
    if (g_wsaRefs.fetch_add(1) == 0) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            g_wsaRefs.fetch_sub(1);
            return false;
        }
    }
    return true;
}

void wsa_release() {
    if (g_wsaRefs.fetch_sub(1) == 1) WSACleanup();
}

} // namespace

HttpApi::~HttpApi() { stop(); }

bool HttpApi::start(uint16_t port, bool bindAll, SnapshotFn snapshot, LogFn log) {
    if (running_.load()) return true;
    snapshot_ = std::move(snapshot);
    log_ = std::move(log);
    port_ = port;
    bindAll_ = bindAll;

    if (!wsa_acquire()) {
        if (log_) log_("http api: WSAStartup failed");
        return false;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (log_) log_("http api: socket() failed");
        wsa_release();
        return false;
    }

    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = bindAll ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        if (log_) log_("http api: bind failed on port " + std::to_string(port));
        closesocket(s);
        wsa_release();
        return false;
    }
    if (listen(s, 8) != 0) {
        if (log_) log_("http api: listen failed");
        closesocket(s);
        wsa_release();
        return false;
    }

    listenSock_ = (std::intptr_t)s;
    stopping_.store(false);
    running_.store(true);
    thread_ = std::thread([this]() { loop(); });
    if (log_) {
        log_("http api listening on " + std::string(bindAll_ ? "0.0.0.0:" : "127.0.0.1:") +
             std::to_string(port_) + "  (GET / or /stat)");
    }
    return true;
}

void HttpApi::stop() {
    if (!running_.load()) return;
    stopping_.store(true);
    const SOCKET s = (SOCKET)listenSock_;
    if (s != INVALID_SOCKET) {
        // Shut the listener down so accept() returns immediately.
        shutdown(s, SD_BOTH);
        closesocket(s);
        listenSock_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    wsa_release();
}

void HttpApi::loop() {
    while (!stopping_.load()) {
        SOCKET s = (SOCKET)listenSock_;
        if (s == INVALID_SOCKET) break;

        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        SOCKET client = accept(s, (sockaddr*)&peer, &peerLen);
        if (client == INVALID_SOCKET) {
            if (stopping_.load()) break;
            continue;
        }

        // Read just the request line; we do not implement anything but GET.
        char buf[2048];
        const int n = recv(client, buf, (int)sizeof(buf) - 1, 0);
        std::string path = "/";
        if (n > 0) {
            buf[n] = '\0';
            const char* sp1 = std::strchr(buf, ' ');
            if (sp1) {
                const char* sp2 = std::strchr(sp1 + 1, ' ');
                if (sp2) path.assign(sp1 + 1, (size_t)(sp2 - sp1 - 1));
            }
        }

        std::string body;
        int status = 200;
        if (path == "/" || path == "/stat" || path == "/index.html") {
            body = snapshot_ ? snapshot_() : std::string("{}");
        } else {
            status = 404;
            body = "{\"error\":\"not found\"}";
        }

        char header[256];
        const int headerLen = std::snprintf(
            header, sizeof(header),
            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
            "Connection: close\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
            status, status == 200 ? "OK" : "Not Found", body.size());

        std::string response;
        response.reserve((size_t)headerLen + body.size());
        response.append(header, (size_t)headerLen);
        response += body;

        size_t sent = 0;
        while (sent < response.size()) {
            const int chunk = send(client, response.data() + sent, (int)(response.size() - sent), 0);
            if (chunk <= 0) break;
            sent += (size_t)chunk;
        }
        shutdown(client, SD_BOTH);
        closesocket(client);
    }
}

} // namespace grin
