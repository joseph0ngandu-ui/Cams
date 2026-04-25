// NetworkListener.h
// Cams OBS Plugin — UDP socket listener with Bonjour discovery.
//
// Listens on kVideoPort (8888) for incoming Cams packets from the iOS device.
// Uses getaddrinfo + POSIX sockets for maximum portability across macOS, Linux,
// and Windows (WinSock 2).
//
// Bonjour discovery: uses dns_sd (mDNSResponder / Avahi) to browse for
// _cams-video._udp services and exposes the list of discovered devices via
// `discoveredDevices()`.

#pragma once

#include "CamsPacket.h"
#include "FrameReassembler.h"
#include "JitterBuffer.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

// POSIX sockets
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCK(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCK (-1)
#define CLOSE_SOCK(s) ::close(s)
#endif

// Bonjour / dns_sd
#ifdef __APPLE__
#include <dns_sd.h>
#define CAMS_HAS_DNSSD 1
#elif defined(__linux__)
// On Linux, Avahi provides a compatible dns_sd.h.
// Compile with -lavahi-compat-libdns_sd
#if __has_include(<dns_sd.h>)
#include <dns_sd.h>
#define CAMS_HAS_DNSSD 1
#endif
#endif

namespace cams {

// ---------------------------------------------------------------------------
// DiscoveredDevice
// ---------------------------------------------------------------------------

struct DiscoveredDevice {
    std::string name;          ///< Human-readable service instance name.
    std::string hostName;      ///< Resolved hostname or IP address string.
    uint16_t    port = 0;
};

// ---------------------------------------------------------------------------
// NetworkListener
// ---------------------------------------------------------------------------

class NetworkListener {
public:
    /// Called when a complete packet arrives.
    using PacketCallback = std::function<void(PacketHeader, std::vector<uint8_t>)>;
    /// Called when loss or malformed input requires decoder recovery.
    using LossCallback = std::function<void(const char *reason)>;

    NetworkListener();
    ~NetworkListener();

    NetworkListener(const NetworkListener &)            = delete;
    NetworkListener &operator=(const NetworkListener &) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle

    /// Binds the UDP socket and starts the receive loop thread.
    /// Returns true on success.
    bool start(uint16_t port = kVideoPort);

    /// Stops listening and joins the receive thread.
    void stop();

    bool isRunning() const { return m_running.load(); }

    // -----------------------------------------------------------------------
    // Callbacks

    /// Register the callback invoked for each valid incoming packet.
    void setPacketCallback(PacketCallback cb);
    /// Register the callback invoked when packet loss is detected.
    void setLossCallback(LossCallback cb);

    // -----------------------------------------------------------------------
    // Bonjour discovery

    /// Starts browsing for _cams-video._udp services.
    void startDiscovery();
    void stopDiscovery();

    /// Snapshot of currently discovered devices (thread-safe copy).
    std::vector<DiscoveredDevice> discoveredDevices() const;

private:
    // ── Socket ───────────────────────────────────────────────────────────
    socket_t           m_socket = INVALID_SOCK;
    std::atomic<bool>  m_running{false};
    std::thread        m_receiveThread;
    PacketCallback     m_packetCallback;
    LossCallback       m_lossCallback;
    mutable std::mutex m_callbackMutex;
    std::atomic<uint64_t> m_invalidPacketCount{0};

    mutable std::mutex m_assemblyMutex;
    FrameReassembler m_reassembler;

    void receiveLoop();
    bool createSocket(uint16_t port);
    void destroySocket();
    void notifyLoss(const char *reason);
    void recordInvalidPacket(const char *reason);

    // ── Bonjour ──────────────────────────────────────────────────────────
#if CAMS_HAS_DNSSD
    DNSServiceRef               m_browseRef  = nullptr;
    std::thread                 m_dnsThread;
    std::atomic<bool>           m_browsing{false};

    static void DNSSD_API browseCB(
        DNSServiceRef        ref,
        DNSServiceFlags      flags,
        uint32_t             interfaceIndex,
        DNSServiceErrorType  errorCode,
        const char          *serviceName,
        const char          *regtype,
        const char          *replyDomain,
        void                *context
    );

    static void DNSSD_API resolveCB(
        DNSServiceRef        ref,
        DNSServiceFlags      flags,
        uint32_t             interfaceIndex,
        DNSServiceErrorType  errorCode,
        const char          *fullname,
        const char          *hosttarget,
        uint16_t             port,
        uint16_t             txtLen,
        const unsigned char *txtRecord,
        void                *context
    );
#endif

    mutable std::mutex              m_devicesMutex;
    std::vector<DiscoveredDevice>   m_devices;

    void addDevice(DiscoveredDevice dev);
    void removeDevice(const std::string &name);
};

} // namespace cams
