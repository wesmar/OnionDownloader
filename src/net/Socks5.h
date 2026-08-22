#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <string>
#include <cstdint>

namespace od::net {

/**
 * @brief Ensures WSAStartup/WSACleanup happen exactly once per process.
 *
 * Constructed on first use through EnsureWinsock(); the destructor of the
 * function-local static runs during normal CRT teardown.
 */
void EnsureWinsock();

/**
 * @brief Move-only RAII wrapper around a blocking TCP socket with I/O timeouts.
 *
 * Every worker owns its own stream, so no synchronisation is needed on the
 * socket itself. Timeouts are mandatory: a Tor circuit that silently stalls
 * must surface as a recv error, otherwise a single dead exit relay would
 * freeze one chunk of the download forever.
 */
class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream();

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;

    /// Connect to 127.0.0.1:port (the local Tor SOCKS listener).
    bool ConnectLoopback(uint16_t port, uint32_t connectTimeoutMs);

    /// Apply receive/send timeouts in milliseconds.
    void SetTimeouts(uint32_t recvMs, uint32_t sendMs);

    /// Write the whole buffer, looping over partial sends.
    bool SendAll(const void* data, size_t len);

    /// Single recv(). Returns bytes read, 0 on orderly shutdown, -1 on error/timeout.
    int Recv(void* buf, size_t len);

    /// Read exactly len bytes or fail.
    bool RecvExact(void* buf, size_t len);

    void Close();
    [[nodiscard]] bool IsValid() const noexcept { return m_sock.load() != INVALID_SOCKET; }

private:
    // Close() may be called by the controller while a worker is blocked in
    // recv(). Exchanging the handle atomically makes that cancellation safe.
    std::atomic<SOCKET> m_sock{INVALID_SOCKET};
};

/**
 * @brief RFC 1928 / RFC 1929 SOCKS5 CONNECT through a local Tor SOCKS port.
 *
 * MULTI-CIRCUIT RATIONALE:
 * Tor enables IsolateSOCKSAuth on every SOCKS listener by default. Two streams
 * that present different username/password pairs are therefore never allowed to
 * share a circuit. Handing each download worker its own credential is what turns
 * one Tor daemon into N independent 3-hop paths - the same effect the Docker
 * stack achieves with N containers behind a load balancer, minus the containers.
 *
 * The destination host is sent as an ATYP=0x03 domain name so that .onion
 * resolution happens inside Tor; no local DNS lookup is ever attempted.
 */
bool Socks5Connect(TcpStream& stream,
                   uint16_t socksPort,
                   const std::string& user,
                   const std::string& password,
                   const std::string& host,
                   uint16_t port,
                   uint32_t timeoutMs,
                   std::string* errorOut);

} // namespace od::net
