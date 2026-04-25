// ControlServer.cpp
// Cams OBS Plugin — UDP control back-channel server implementation.

#include "ControlServer.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>
#ifndef _WIN32
#include <netdb.h>
#endif

namespace cams {

ControlServer::ControlServer() {}

ControlServer::~ControlServer() {
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ControlServer::start(uint16_t port) {
    if (m_running.load()) return true;
    if (!createSocket(port)) return false;
    m_running.store(true);
    m_recvThread = std::thread(&ControlServer::receiveLoop, this);
    return true;
}

void ControlServer::stop() {
    if (!m_running.exchange(false)) return;
    destroySocket();
    if (m_recvThread.joinable()) m_recvThread.join();
}

void ControlServer::setTarget(const std::string &host, uint16_t port) {
    sockaddr_in resolved{};
    bool ok = resolveIPv4Target(host, port, resolved);

    std::lock_guard<std::mutex> lk(m_targetMutex);
    m_targetHost = host;
    m_targetPort = port;
    m_targetAddr = resolved;
    m_targetResolved = ok;

    if (!ok && !host.empty()) {
        fprintf(stderr, "[Cams] Control target could not be resolved: %s:%u\n",
                host.c_str(), port);
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void ControlServer::sendFocusLock(bool locked) {
    sendCommand(locked ? ControlCommand::LockFocus : ControlCommand::UnlockFocus);
}

void ControlServer::sendExposureLock(bool locked) {
    sendCommand(locked ? ControlCommand::LockExposure : ControlCommand::UnlockExposure);
}

void ControlServer::sendKeyframeRequest() {
    sendCommand(ControlCommand::RequestKeyframe);
}

void ControlServer::sendQuality(int qualityPreset) {
    switch (qualityPreset) {
    case 0:
        sendCommand(ControlCommand::SetQualityLow);
        break;
    case 1:
        sendCommand(ControlCommand::SetQualityMedium);
        break;
    default:
        sendCommand(ControlCommand::SetQualityHigh);
        break;
    }
}

std::optional<double> ControlServer::ping(int timeoutMs) {
    if (m_socket == INVALID_SOCK) return std::nullopt;

    const auto started = std::chrono::steady_clock::now();
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lk(m_pingMutex);
        token = m_nextPingToken++;
        m_pendingPings[token] = started;
    }

    sendCommand(ControlCommand::Ping, encodePingToken(token));

    std::unique_lock<std::mutex> lk(m_pingMutex);
    const bool received = m_pingCV.wait_for(
        lk,
        std::chrono::milliseconds(timeoutMs),
        [this, token] {
            return m_completedPings.find(token) != m_completedPings.end() ||
                   m_pendingPings.find(token) == m_pendingPings.end();
        }
    );

    if (!received || m_completedPings.find(token) == m_completedPings.end()) {
        m_pendingPings.erase(token);
        return std::nullopt;
    }

    const auto completed = m_completedPings[token];
    m_completedPings.erase(token);
    m_pendingPings.erase(token);
    return std::chrono::duration<double, std::milli>(completed - started).count();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool ControlServer::createSocket(uint16_t port) {
    m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCK) {
        fprintf(stderr, "[Cams] ControlServer socket() failed\n");
        return false;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (::bind(m_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        fprintf(stderr, "[Cams] ControlServer bind() failed on port %u\n", port);
        CLOSE_SOCK(m_socket);
        m_socket = INVALID_SOCK;
        return false;
    }

    sockaddr_in actualAddr{};
    socklen_t actualLen = sizeof(actualAddr);
    if (::getsockname(m_socket, reinterpret_cast<sockaddr *>(&actualAddr), &actualLen) == 0) {
        m_boundPort = ntohs(actualAddr.sin_port);
    } else {
        m_boundPort = port;
    }
    return true;
}

void ControlServer::destroySocket() {
    if (m_socket != INVALID_SOCK) {
        CLOSE_SOCK(m_socket);
        m_socket = INVALID_SOCK;
    }
    m_boundPort = 0;
    {
        std::lock_guard<std::mutex> lk(m_pingMutex);
        m_pendingPings.clear();
        m_completedPings.clear();
    }
    m_pingCV.notify_all();
}

void ControlServer::receiveLoop() {
    std::vector<uint8_t> buf(512);
    while (m_running.load()) {
        sockaddr_in senderAddr{};
        socklen_t   senderLen = sizeof(senderAddr);

        ssize_t n = ::recvfrom(
            m_socket,
            reinterpret_cast<char *>(buf.data()),
            static_cast<int>(buf.size()),
            0,
            reinterpret_cast<sockaddr *>(&senderAddr),
            &senderLen
        );

        if (n <= 0) {
            if (!m_running.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto pktOpt = ControlPacket::deserialise(buf.data(), static_cast<size_t>(n));
        if (!pktOpt) continue;

        // If we receive a ping from the iOS side, reply with a pong.
        if (pktOpt->command == ControlCommand::Ping) {
            ControlPacket pong{ControlCommand::Pong, pktOpt->payload};
            auto data = pong.serialise();
            ::sendto(m_socket,
                     reinterpret_cast<const char *>(data.data()),
                     static_cast<int>(data.size()),
                     0,
                     reinterpret_cast<sockaddr *>(&senderAddr),
                     senderLen);
        } else if (pktOpt->command == ControlCommand::Pong) {
            handlePong(pktOpt->payload);
        }
    }
}

void ControlServer::sendCommand(ControlCommand cmd, const std::vector<uint8_t> &payload) {
    if (m_socket == INVALID_SOCK) return;

    sockaddr_in dest{};
    {
        std::lock_guard<std::mutex> lk(m_targetMutex);
        if (!m_targetResolved) return;
        dest = m_targetAddr;
    }

    ControlPacket pkt{cmd, payload};
    auto data = pkt.serialise();

    ::sendto(m_socket,
             reinterpret_cast<const char *>(data.data()),
             static_cast<int>(data.size()),
             0,
             reinterpret_cast<const sockaddr *>(&dest),
             sizeof(dest));
}

void ControlServer::handlePong(const std::vector<uint8_t> &payload) {
    auto tokenOpt = decodePingToken(payload);
    if (!tokenOpt) return;

    std::lock_guard<std::mutex> lk(m_pingMutex);
    const uint64_t token = *tokenOpt;
    if (m_pendingPings.find(token) == m_pendingPings.end()) return;
    m_completedPings[token] = std::chrono::steady_clock::now();
    m_pingCV.notify_all();
}

std::vector<uint8_t> ControlServer::encodePingToken(uint64_t token) {
    std::vector<uint8_t> out(8);
    for (int i = 7; i >= 0; --i) {
        out[static_cast<size_t>(7 - i)] = static_cast<uint8_t>((token >> (i * 8)) & 0xFF);
    }
    return out;
}

std::optional<uint64_t> ControlServer::decodePingToken(const std::vector<uint8_t> &payload) {
    if (payload.size() < 8) return std::nullopt;

    uint64_t token = 0;
    for (size_t i = 0; i < 8; ++i) {
        token = (token << 8) | payload[i];
    }
    return token;
}

bool ControlServer::resolveIPv4Target(const std::string &host, uint16_t port, sockaddr_in &out) {
    if (host.empty()) return false;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &dest.sin_addr) == 1) {
        out = dest;
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result || !result->ai_addr) {
        if (result) freeaddrinfo(result);
        return false;
    }

    auto *in = reinterpret_cast<sockaddr_in *>(result->ai_addr);
    dest.sin_addr = in->sin_addr;
    freeaddrinfo(result);
    out = dest;
    return true;
}

} // namespace cams
