#pragma once

#include "Socks5.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace od::net {

/// Parsed absolute URL. Only the http scheme is supported: onion services are
/// plain HTTP behind Tor's own end-to-end encrypted rendezvous circuit, so a
/// TLS layer would add cost without adding protection.
struct Url {
    std::string scheme;
    std::string host;
    std::string path{"/"};
    uint16_t port{80};

    [[nodiscard]] bool IsOnion() const;
    [[nodiscard]] std::string Authority() const;
    [[nodiscard]] std::string ToString() const;
};

bool ParseUrl(std::string_view raw, Url& out, std::string* errorOut);

/// Resolve a possibly relative Location header against the current URL.
bool ResolveRedirect(const Url& base, std::string_view location, Url& out, std::string* errorOut);

/// Percent-decode a URL path component into a displayable file name.
std::string PercentDecode(std::string_view text);

struct HttpResponse {
    int statusCode{0};
    std::string statusText;
    std::vector<std::pair<std::string, std::string>> headers;

    bool hasContentLength{false};
    uint64_t contentLength{0};

    bool hasContentRange{false};
    uint64_t instanceLength{0};   ///< total resource size parsed from Content-Range

    [[nodiscard]] std::string Header(std::string_view name) const;
    [[nodiscard]] bool IsChunked() const;
    [[nodiscard]] bool WantsClose() const;
};

/**
 * @brief One persistent HTTP/1.1 connection pinned to a single Tor circuit.
 *
 * The connection is opened through SOCKS5 with a caller-supplied credential;
 * every subsequent ranged request therefore reuses the exact same circuit,
 * which is what makes keep-alive worth having here - rebuilding a rendezvous
 * circuit costs far more than any HTTP round trip.
 */
class HttpConnection {
public:
    HttpConnection() = default;

    HttpConnection(const HttpConnection&) = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;

    /// Open a fresh circuit to url.host through the given local Tor SOCKS port.
    bool Open(uint16_t socksPort,
              const std::string& isolationUser,
              const std::string& isolationPassword,
              const Url& url,
              uint32_t handshakeTimeoutMs,
              std::string* errorOut);

    /// Issue GET with an optional Range header and read the response headers.
    /// The body is left on the wire for ReadBody().
    bool RequestRange(const Url& url,
                      bool useRange,
                      uint64_t firstByte,
                      uint64_t lastByte,
                      HttpResponse& response,
                      std::string* errorOut,
                      std::string_view cookie = {});

    /// Solve an onion service's /_gate/challenge + /_gate/verify SHA-256 PoW gate.
    bool SolvePoWGate(const Url& originUrl, std::string& cookieOut, std::string* errorOut);

    /// Read up to len body bytes. Returns 0 on clean end of stream, -1 on error.
    int ReadBody(void* buffer, size_t len);

    /// Discard the remaining body so the connection can be reused.
    bool DrainBody(uint64_t byteCount);

    void Close();
    /// Abort blocking socket I/O from another thread. Used by Pause/Shutdown.
    void Cancel() { m_stream.Close(); }
    [[nodiscard]] bool IsOpen() const { return m_stream.IsValid(); }

    void SetIoTimeouts(uint32_t recvMs, uint32_t sendMs) { m_stream.SetTimeouts(recvMs, sendMs); }

private:
    bool ReadHeaderBlock(std::string& headerBlock, std::string* errorOut);

    TcpStream m_stream;
    std::string m_pending;    ///< body bytes already pulled off the socket
    size_t m_pendingOffset{0};
};

/// Default User-Agent. Mirrors a current Firefox ESR string so that onion
/// mirrors which filter obvious download tools do not reject the request.
extern const char* const kDefaultUserAgent;

} // namespace od::net
