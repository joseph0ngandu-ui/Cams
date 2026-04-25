// ControlServer.h
// Cams OBS Plugin — UDP control back-channel server (port 8889).
//
// Receives decoded device information from NetworkListener and sends control
// commands (focus lock, exposure lock, keyframe request) back to the iOS app.

#pragma once

#include "CamsPacket.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCK(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCK (-1)
#define CLOSE_SOCK(s) ::close(s)
#endif

namespace cams {

class ControlServer {
public:
    ControlServer();
    ~ControlServer();

    ControlServer(const ControlServer &)            = delete;
    ControlServer &operator=(const ControlServer &) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle

    /// Binds the control socket and starts the receive loop.
    bool start(uint16_t port = kControlPort);

    /// Stops the control server.
    void stop();

    bool isRunning() const { return m_running.load(); }

    // -----------------------------------------------------------------------
    // Targeting

    /// Sets the IP address and port to which outbound commands are sent.
    void setTarget(const std::string &host, uint16_t port = kControlPort);

    // -----------------------------------------------------------------------
    // Commands

    /// Sends a focus lock or unlock command to the iOS device.
    void sendFocusLock(bool locked);

    /// Sends an exposure lock or unlock command to the iOS device.
    void sendExposureLock(bool locked);

    /// Requests an immediate IDR keyframe from the encoder.
    void sendKeyframeRequest();
    /// Requests a quality preset change on the iOS app.
    void sendQuality(int qualityPreset);
    /// Enables/disables audio transport from iOS.
    void sendAudioEnabled(bool enabled);

    /// Sends a ping and returns the round-trip latency in milliseconds.
    /// Blocks for up to `timeoutMs` milliseconds.
    double ping(int timeoutMs = 500);

private:
    socket_t          m_socket      = INVALID_SOCK;
    std::atomic<bool> m_running{false};
    std::thread       m_recvThread;

    std::string       m_targetHost;
    uint16_t          m_targetPort  = kControlPort;
    sockaddr_in       m_targetAddr{};
    bool              m_targetResolved = false;
    mutable std::mutex m_targetMutex;

    bool createSocket(uint16_t port);
    void destroySocket();
    void receiveLoop();
    void sendCommand(ControlCommand cmd, const std::vector<uint8_t> &payload = {});
    static bool resolveIPv4Target(const std::string &host, uint16_t port, sockaddr_in &out);
};

} // namespace cams
