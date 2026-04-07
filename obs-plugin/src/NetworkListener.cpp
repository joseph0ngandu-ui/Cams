// NetworkListener.cpp
// Cams OBS Plugin — UDP socket listener implementation.

#include "NetworkListener.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#pragma comment(lib, "Ws2_32.lib")
static bool wsaInitialised = false;
#endif

namespace cams {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

NetworkListener::NetworkListener() {
#ifdef _WIN32
    if (!wsaInitialised) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wsaInitialised = true;
    }
#endif
}

NetworkListener::~NetworkListener() {
    stop();
    stopDiscovery();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool NetworkListener::start(uint16_t port) {
    if (m_running.load()) return true;

    if (!createSocket(port)) return false;

    m_running.store(true);
    m_receiveThread = std::thread(&NetworkListener::receiveLoop, this);
    return true;
}

void NetworkListener::stop() {
    if (!m_running.exchange(false)) return;
    destroySocket();
    if (m_receiveThread.joinable()) m_receiveThread.join();
}

void NetworkListener::setPacketCallback(PacketCallback cb) {
    std::lock_guard<std::mutex> lk(m_callbackMutex);
    m_packetCallback = std::move(cb);
}

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

bool NetworkListener::createSocket(uint16_t port) {
    m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCK) {
        fprintf(stderr, "[Cams] socket() failed: %s\n", strerror(errno));
        return false;
    }

    // Allow port reuse so the plugin can be reloaded without waiting for
    // TIME_WAIT to expire.
    int opt = 1;
#ifdef _WIN32
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (::bind(m_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        fprintf(stderr, "[Cams] bind() failed on port %u: %s\n",
                port, strerror(errno));
        CLOSE_SOCK(m_socket);
        m_socket = INVALID_SOCK;
        return false;
    }

    return true;
}

void NetworkListener::destroySocket() {
    if (m_socket != INVALID_SOCK) {
        CLOSE_SOCK(m_socket);
        m_socket = INVALID_SOCK;
    }
}

// ---------------------------------------------------------------------------
// Receive loop
// ---------------------------------------------------------------------------

void NetworkListener::receiveLoop() {
    // Maximum expected packet size: 64 KB (max UDP datagram).
    static constexpr size_t kBufSize = 65536;
    std::vector<uint8_t> buf(kBufSize);

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

        if (n < 0) {
            if (!m_running.load()) break;  // Socket was closed deliberately.
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEINTR || err == WSAEWOULDBLOCK) continue;
#else
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
            fprintf(stderr, "[Cams] recvfrom error: %s — restarting in 1s\n",
                    strerror(errno));
            // Brief pause to avoid spinning on a persistent error.
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (static_cast<size_t>(n) < kHeaderSize) continue;

        auto headerOpt = PacketHeader::deserialise(buf.data(), static_cast<size_t>(n));
        if (!headerOpt) continue;

        const PacketHeader &hdr = *headerOpt;
        size_t payloadStart = kHeaderSize;
        size_t payloadLen   = static_cast<size_t>(n) - kHeaderSize;

        // Validate declared payload length.
        if (hdr.payloadLength > payloadLen) continue;

        std::vector<uint8_t> payload(
            buf.begin() + static_cast<ptrdiff_t>(payloadStart),
            buf.begin() + static_cast<ptrdiff_t>(payloadStart + hdr.payloadLength)
        );

        PacketCallback cb;
        {
            std::lock_guard<std::mutex> lk(m_callbackMutex);
            cb = m_packetCallback;
        }
        if (cb) cb(hdr, std::move(payload));
    }
}

// ---------------------------------------------------------------------------
// Bonjour discovery
// ---------------------------------------------------------------------------

void NetworkListener::startDiscovery() {
#if CAMS_HAS_DNSSD
    if (m_browsing.load()) return;
    m_browsing.store(true);

    DNSServiceErrorType err = DNSServiceBrowse(
        &m_browseRef,
        0,           // flags
        0,           // interfaceIndex — all interfaces
        kServiceType,
        nullptr,     // domain — all domains
        &NetworkListener::browseCB,
        this
    );

    if (err != kDNSServiceErr_NoError) {
        fprintf(stderr, "[Cams] DNSServiceBrowse failed: %d\n", err);
        m_browsing.store(false);
        return;
    }

    m_dnsThread = std::thread([this] {
        while (m_browsing.load()) {
            int fd = DNSServiceRefSockFD(m_browseRef);
            if (fd < 0) break;

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);

            timeval tv{0, 200000};  // 200 ms timeout so we can check m_browsing frequently.
            int sel = select(fd + 1, &readfds, nullptr, nullptr, &tv);
            if (sel > 0) {
                DNSServiceErrorType procErr =
                    DNSServiceProcessResult(m_browseRef);
                if (procErr != kDNSServiceErr_NoError) break;
            }
        }
    });
#endif
}

void NetworkListener::stopDiscovery() {
#if CAMS_HAS_DNSSD
    m_browsing.store(false);
    if (m_browseRef) {
        DNSServiceRefDeallocate(m_browseRef);
        m_browseRef = nullptr;
    }
    if (m_dnsThread.joinable()) m_dnsThread.join();
#endif
}

std::vector<DiscoveredDevice> NetworkListener::discoveredDevices() const {
    std::lock_guard<std::mutex> lk(m_devicesMutex);
    return m_devices;
}

void NetworkListener::addDevice(DiscoveredDevice dev) {
    std::lock_guard<std::mutex> lk(m_devicesMutex);
    auto it = std::find_if(m_devices.begin(), m_devices.end(),
        [&dev](const DiscoveredDevice &d) { return d.name == dev.name; });
    if (it == m_devices.end())
        m_devices.push_back(std::move(dev));
    else
        *it = std::move(dev);
}

void NetworkListener::removeDevice(const std::string &name) {
    std::lock_guard<std::mutex> lk(m_devicesMutex);
    m_devices.erase(
        std::remove_if(m_devices.begin(), m_devices.end(),
            [&name](const DiscoveredDevice &d) { return d.name == name; }),
        m_devices.end()
    );
}

// ---------------------------------------------------------------------------
// Bonjour callbacks (static)
// ---------------------------------------------------------------------------

#if CAMS_HAS_DNSSD

void DNSSD_API NetworkListener::browseCB(
    DNSServiceRef        /*ref*/,
    DNSServiceFlags      flags,
    uint32_t             interfaceIndex,
    DNSServiceErrorType  errorCode,
    const char          *serviceName,
    const char          *regtype,
    const char          *replyDomain,
    void                *context
) {
    if (errorCode != kDNSServiceErr_NoError) return;
    auto *self = static_cast<NetworkListener *>(context);

    if (flags & kDNSServiceFlagsAdd) {
        // Resolve the service to get host/port.
        DNSServiceRef resolveRef = nullptr;
        DNSServiceErrorType err = DNSServiceResolve(
            &resolveRef, 0, interfaceIndex,
            serviceName, regtype, replyDomain,
            &NetworkListener::resolveCB, context
        );
        if (err == kDNSServiceErr_NoError) {
            // Process the resolve synchronously.
            DNSServiceProcessResult(resolveRef);
            DNSServiceRefDeallocate(resolveRef);
        }
    } else {
        // Service removed.
        self->removeDevice(std::string(serviceName));
    }
}

void DNSSD_API NetworkListener::resolveCB(
    DNSServiceRef        /*ref*/,
    DNSServiceFlags      /*flags*/,
    uint32_t             /*interfaceIndex*/,
    DNSServiceErrorType  errorCode,
    const char          *fullname,
    const char          *hosttarget,
    uint16_t             port,
    uint16_t             /*txtLen*/,
    const unsigned char */*txtRecord*/,
    void                *context
) {
    if (errorCode != kDNSServiceErr_NoError) return;
    auto *self = static_cast<NetworkListener *>(context);

    // Extract instance name from fullname ("Name._cams-video._udp.local.")
    std::string name(fullname);
    auto dot = name.find('.');
    if (dot != std::string::npos) name = name.substr(0, dot);

    self->addDevice({name, std::string(hosttarget), ntohs(port)});
}

#endif  // CAMS_HAS_DNSSD

} // namespace cams
