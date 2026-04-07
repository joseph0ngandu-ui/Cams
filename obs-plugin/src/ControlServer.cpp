// ControlServer.cpp
// Cams OBS Plugin — UDP control back-channel server implementation.

#include "ControlServer.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

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
    std::lock_guard<std::mutex> lk(m_targetMutex);
    m_targetHost = host;
    m_targetPort = port;
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

double ControlServer::ping(int timeoutMs) {
    auto t0 = std::chrono::steady_clock::now();
    sendCommand(ControlCommand::Ping);

    // Wait briefly for a pong (best-effort; no dedicated synchronisation).
    std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    auto t1 = std::chrono::steady_clock::now();

    return std::chrono::duration<double, std::milli>(t1 - t0).count();
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
    return true;
}

void ControlServer::destroySocket() {
    if (m_socket != INVALID_SOCK) {
        CLOSE_SOCK(m_socket);
        m_socket = INVALID_SOCK;
    }
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
        }
    }
}

void ControlServer::sendCommand(ControlCommand cmd, const std::vector<uint8_t> &payload) {
    if (m_socket == INVALID_SOCK) return;

    std::string host;
    uint16_t    port;
    {
        std::lock_guard<std::mutex> lk(m_targetMutex);
        host = m_targetHost;
        port = m_targetPort;
    }
    if (host.empty()) return;

    ControlPacket pkt{cmd, payload};
    auto data = pkt.serialise();

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(port);
    inet_pton(AF_INET, host.c_str(), &dest.sin_addr);

    ::sendto(m_socket,
             reinterpret_cast<const char *>(data.data()),
             static_cast<int>(data.size()),
             0,
             reinterpret_cast<const sockaddr *>(&dest),
             sizeof(dest));
}

} // namespace cams
