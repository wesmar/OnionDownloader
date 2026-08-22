#include "Socks5.h"

#include <mutex>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace od::net {

namespace {

struct WinsockScope {
    WinsockScope() {
        WSADATA data{};
        ::WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockScope() {
        ::WSACleanup();
    }
};

/// Tor's SOCKS5 reply codes carry the actual diagnosis (0x04 = onion service
/// unreachable, 0x06 = TTL expired / rendezvous failed). Mapping them is the
/// difference between "Unknown error" and an actionable message.
const char* Socks5ReplyText(uint8_t code) {
    switch (code) {
        case 0x00: return "succeeded";
        case 0x01: return "general SOCKS server failure";
        case 0x02: return "connection not allowed by ruleset";
        case 0x03: return "network unreachable";
        case 0x04: return "host unreachable (onion service down or wrong address)";
        case 0x05: return "connection refused by destination";
        case 0x06: return "TTL expired (Tor could not build a circuit)";
        case 0x07: return "command not supported";
        case 0x08: return "address type not supported";
        default:   return "unknown SOCKS5 failure";
    }
}

void Fail(std::string* errorOut, std::string message) {
    if (errorOut != nullptr) {
        *errorOut = std::move(message);
    }
}

} // namespace

void EnsureWinsock() {
    static WinsockScope scope;
    (void)&scope;
}

TcpStream::~TcpStream() {
    Close();
}

TcpStream::TcpStream(TcpStream&& other) noexcept
    : m_sock(other.m_sock.exchange(INVALID_SOCKET)) {
}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        Close();
        m_sock = other.m_sock.exchange(INVALID_SOCKET);
    }
    return *this;
}

bool TcpStream::ConnectLoopback(uint16_t port, uint32_t connectTimeoutMs) {
    EnsureWinsock();
    Close();

    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    m_sock = socket;
    if (socket == INVALID_SOCKET) {
        return false;
    }

    // Loopback connect: use a non-blocking connect purely to bound the wait,
    // then return the socket to blocking mode for the rest of its life.
    u_long nonBlocking = 1;
    ::ioctlsocket(socket, FIONBIO, &nonBlocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    bool connected = false;
    if (::connect(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        connected = true;
    } else if (::WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writeSet{};
        FD_ZERO(&writeSet);
        FD_SET(socket, &writeSet);

        timeval tv{};
        tv.tv_sec = static_cast<long>(connectTimeoutMs / 1000);
        tv.tv_usec = static_cast<long>((connectTimeoutMs % 1000) * 1000);

        if (::select(0, nullptr, &writeSet, nullptr, &tv) > 0) {
            int soError = 0;
            int len = sizeof(soError);
            if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len) == 0) {
                connected = (soError == 0);
            }
        }
    }

    nonBlocking = 0;
    ::ioctlsocket(socket, FIONBIO, &nonBlocking);

    if (!connected) {
        Close();
        return false;
    }

    BOOL noDelay = TRUE;
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    return true;
}

void TcpStream::SetTimeouts(uint32_t recvMs, uint32_t sendMs) {
    const SOCKET socket = m_sock.load();
    if (socket == INVALID_SOCKET) {
        return;
    }
    DWORD rcv = recvMs;
    DWORD snd = sendMs;
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
    ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd), sizeof(snd));
}

bool TcpStream::SendAll(const void* data, size_t len) {
    const SOCKET socket = m_sock.load();
    if (socket == INVALID_SOCKET) {
        return false;
    }
    const char* cursor = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        const int chunk = static_cast<int>(remaining > 65536 ? 65536 : remaining);
        const int sent = ::send(socket, cursor, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

int TcpStream::Recv(void* buf, size_t len) {
    const SOCKET socket = m_sock.load();
    if (socket == INVALID_SOCKET || len == 0) {
        return -1;
    }
    const int cap = static_cast<int>(len > 262144 ? 262144 : len);
    return ::recv(socket, static_cast<char*>(buf), cap, 0);
}

bool TcpStream::RecvExact(void* buf, size_t len) {
    char* cursor = static_cast<char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        const int got = Recv(cursor, remaining);
        if (got <= 0) {
            return false;
        }
        cursor += got;
        remaining -= static_cast<size_t>(got);
    }
    return true;
}

void TcpStream::Close() {
    const SOCKET socket = m_sock.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) {
        ::shutdown(socket, SD_BOTH);
        ::closesocket(socket);
    }
}

bool Socks5Connect(TcpStream& stream,
                   uint16_t socksPort,
                   const std::string& user,
                   const std::string& password,
                   const std::string& host,
                   uint16_t port,
                   uint32_t timeoutMs,
                   std::string* errorOut) {
    if (host.empty() || host.size() > 255) {
        Fail(errorOut, "invalid destination host");
        return false;
    }
    if (user.size() > 255 || password.size() > 255) {
        Fail(errorOut, "SOCKS5 credential too long");
        return false;
    }

    if (!stream.ConnectLoopback(socksPort, 5000)) {
        Fail(errorOut, "cannot reach local Tor SOCKS port " + std::to_string(socksPort));
        return false;
    }

    // Building a rendezvous circuit to an onion service regularly takes tens of
    // seconds, so the handshake gets the full caller-supplied budget.
    stream.SetTimeouts(timeoutMs, 15000);

    // Greeting: offer username/password auth only. Tor accepts it unconditionally
    // and uses the credential as the stream-isolation key.
    const unsigned char greeting[] = {0x05, 0x01, 0x02};
    if (!stream.SendAll(greeting, sizeof(greeting))) {
        Fail(errorOut, "SOCKS5 greeting failed");
        return false;
    }

    unsigned char methodReply[2] = {};
    if (!stream.RecvExact(methodReply, sizeof(methodReply))) {
        Fail(errorOut, "no SOCKS5 method reply from Tor");
        return false;
    }
    if (methodReply[0] != 0x05 || methodReply[1] != 0x02) {
        Fail(errorOut, "Tor rejected username/password isolation");
        return false;
    }

    std::vector<unsigned char> auth;
    auth.reserve(3 + user.size() + password.size());
    auth.push_back(0x01);
    auth.push_back(static_cast<unsigned char>(user.size()));
    auth.insert(auth.end(), user.begin(), user.end());
    auth.push_back(static_cast<unsigned char>(password.size()));
    auth.insert(auth.end(), password.begin(), password.end());

    if (!stream.SendAll(auth.data(), auth.size())) {
        Fail(errorOut, "SOCKS5 authentication send failed");
        return false;
    }

    unsigned char authReply[2] = {};
    if (!stream.RecvExact(authReply, sizeof(authReply)) || authReply[1] != 0x00) {
        Fail(errorOut, "SOCKS5 authentication rejected");
        return false;
    }

    std::vector<unsigned char> request;
    request.reserve(7 + host.size());
    request.push_back(0x05);                                       // version
    request.push_back(0x01);                                       // CONNECT
    request.push_back(0x00);                                       // reserved
    request.push_back(0x03);                                       // ATYP: domain name
    request.push_back(static_cast<unsigned char>(host.size()));
    request.insert(request.end(), host.begin(), host.end());
    request.push_back(static_cast<unsigned char>((port >> 8) & 0xFF));
    request.push_back(static_cast<unsigned char>(port & 0xFF));

    if (!stream.SendAll(request.data(), request.size())) {
        Fail(errorOut, "SOCKS5 CONNECT send failed");
        return false;
    }

    unsigned char head[4] = {};
    if (!stream.RecvExact(head, sizeof(head))) {
        Fail(errorOut, "Tor closed the stream during CONNECT (circuit build timed out)");
        return false;
    }
    if (head[0] != 0x05) {
        Fail(errorOut, "malformed SOCKS5 reply");
        return false;
    }
    if (head[1] != 0x00) {
        Fail(errorOut, std::string("Tor: ") + Socks5ReplyText(head[1]));
        return false;
    }

    // Drain the bound-address field so the stream is positioned at the payload.
    switch (head[3]) {
        case 0x01: {
            unsigned char skip[4 + 2] = {};
            if (!stream.RecvExact(skip, sizeof(skip))) {
                Fail(errorOut, "truncated SOCKS5 reply");
                return false;
            }
            break;
        }
        case 0x03: {
            unsigned char nameLen = 0;
            if (!stream.RecvExact(&nameLen, 1)) {
                Fail(errorOut, "truncated SOCKS5 reply");
                return false;
            }
            std::vector<unsigned char> skip(static_cast<size_t>(nameLen) + 2);
            if (!stream.RecvExact(skip.data(), skip.size())) {
                Fail(errorOut, "truncated SOCKS5 reply");
                return false;
            }
            break;
        }
        case 0x04: {
            unsigned char skip[16 + 2] = {};
            if (!stream.RecvExact(skip, sizeof(skip))) {
                Fail(errorOut, "truncated SOCKS5 reply");
                return false;
            }
            break;
        }
        default:
            Fail(errorOut, "unsupported SOCKS5 address type");
            return false;
    }

    return true;
}

} // namespace od::net
