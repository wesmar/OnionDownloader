#include "HttpClient.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace od::net {

const char* const kDefaultUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0";

namespace {

void Fail(std::string* errorOut, std::string message) {
    if (errorOut != nullptr) {
        *errorOut = std::move(message);
    }
}

char LowerAscii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) { return LowerAscii(x) == LowerAscii(y); });
}

std::string_view TrimAscii(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

bool ParseUint(std::string_view text, uint64_t& out) {
    text = TrimAscii(text);
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool Url::IsOnion() const {
    return host.size() > 6 && EqualsIgnoreCaseAscii(std::string_view(host).substr(host.size() - 6), ".onion");
}

std::string Url::Authority() const {
    if (port == 80) {
        return host;
    }
    return host + ":" + std::to_string(port);
}

std::string Url::ToString() const {
    return scheme + "://" + Authority() + path;
}

std::string PercentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = HexValue(text[i + 1]);
            const int lo = HexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

bool ParseUrl(std::string_view raw, Url& out, std::string* errorOut) {
    std::string_view text = TrimAscii(raw);
    if (text.empty()) {
        Fail(errorOut, "empty URL");
        return false;
    }

    const size_t schemeEnd = text.find("://");
    if (schemeEnd == std::string_view::npos) {
        Fail(errorOut, "URL must start with http://");
        return false;
    }

    std::string scheme(text.substr(0, schemeEnd));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), LowerAscii);
    if (scheme != "http") {
        Fail(errorOut, "unsupported scheme '" + scheme + "' - onion downloads use http://");
        return false;
    }

    text.remove_prefix(schemeEnd + 3);

    const size_t pathStart = text.find_first_of("/?#");
    std::string_view authority = (pathStart == std::string_view::npos) ? text : text.substr(0, pathStart);
    std::string path = (pathStart == std::string_view::npos) ? std::string("/") : std::string(text.substr(pathStart));

    // Credentials in the authority are not meaningful for onion mirrors; drop them.
    const size_t atSign = authority.rfind('@');
    if (atSign != std::string_view::npos) {
        authority.remove_prefix(atSign + 1);
    }

    uint16_t port = 80;
    std::string host;
    const size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        uint64_t parsed = 0;
        if (!ParseUint(authority.substr(colon + 1), parsed) || parsed == 0 || parsed > 65535) {
            Fail(errorOut, "invalid port in URL");
            return false;
        }
        port = static_cast<uint16_t>(parsed);
        host = std::string(authority.substr(0, colon));
    } else {
        host = std::string(authority);
    }

    if (host.empty()) {
        Fail(errorOut, "URL has no host");
        return false;
    }

    std::transform(host.begin(), host.end(), host.begin(), LowerAscii);

    out.scheme = std::move(scheme);
    out.host = std::move(host);
    out.port = port;
    out.path = path.empty() ? std::string("/") : std::move(path);
    return true;
}

bool ResolveRedirect(const Url& base, std::string_view location, Url& out, std::string* errorOut) {
    location = TrimAscii(location);
    if (location.empty()) {
        Fail(errorOut, "redirect without Location header");
        return false;
    }
    if (location.find("://") != std::string_view::npos) {
        return ParseUrl(location, out, errorOut);
    }

    out = base;
    if (location.front() == '/') {
        out.path = std::string(location);
    } else {
        const size_t lastSlash = base.path.rfind('/');
        out.path = base.path.substr(0, lastSlash == std::string::npos ? 0 : lastSlash + 1) + std::string(location);
    }
    return true;
}

std::string HttpResponse::Header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (EqualsIgnoreCaseAscii(key, name)) {
            return value;
        }
    }
    return {};
}

bool HttpResponse::IsChunked() const {
    return EqualsIgnoreCaseAscii(Header("Transfer-Encoding"), "chunked");
}

bool HttpResponse::WantsClose() const {
    return EqualsIgnoreCaseAscii(Header("Connection"), "close");
}

bool HttpConnection::Open(uint16_t socksPort,
                          const std::string& isolationUser,
                          const std::string& isolationPassword,
                          const Url& url,
                          uint32_t handshakeTimeoutMs,
                          std::string* errorOut) {
    Close();
    if (!Socks5Connect(m_stream, socksPort, isolationUser, isolationPassword,
                       url.host, url.port, handshakeTimeoutMs, errorOut)) {
        m_stream.Close();
        return false;
    }
    return true;
}

bool HttpConnection::RequestRange(const Url& url,
                                  bool useRange,
                                  uint64_t firstByte,
                                  uint64_t lastByte,
                                  HttpResponse& response,
                                  std::string* errorOut) {
    if (!m_stream.IsValid()) {
        Fail(errorOut, "connection is closed");
        return false;
    }

    std::string request;
    request.reserve(320);
    request += "GET ";
    request += url.path;
    request += " HTTP/1.1\r\nHost: ";
    request += url.Authority();
    request += "\r\nUser-Agent: ";
    request += kDefaultUserAgent;
    request += "\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n";
    if (useRange) {
        request += "Range: bytes=";
        request += std::to_string(firstByte);
        request += "-";
        request += std::to_string(lastByte);
        request += "\r\n";
    }
    request += "\r\n";

    if (!m_stream.SendAll(request.data(), request.size())) {
        Fail(errorOut, "sending request failed");
        return false;
    }

    std::string headerBlock;
    if (!ReadHeaderBlock(headerBlock, errorOut)) {
        return false;
    }

    response = HttpResponse{};

    size_t lineStart = 0;
    const size_t firstEol = headerBlock.find("\r\n");
    if (firstEol == std::string::npos) {
        Fail(errorOut, "malformed HTTP response");
        return false;
    }

    {
        std::string_view statusLine(headerBlock.data(), firstEol);
        const size_t sp1 = statusLine.find(' ');
        if (sp1 == std::string_view::npos) {
            Fail(errorOut, "malformed HTTP status line");
            return false;
        }
        std::string_view rest = statusLine.substr(sp1 + 1);
        const size_t sp2 = rest.find(' ');
        std::string_view codeText = (sp2 == std::string_view::npos) ? rest : rest.substr(0, sp2);
        uint64_t code = 0;
        if (!ParseUint(codeText, code)) {
            Fail(errorOut, "malformed HTTP status code");
            return false;
        }
        response.statusCode = static_cast<int>(code);
        if (sp2 != std::string_view::npos) {
            response.statusText = std::string(TrimAscii(rest.substr(sp2 + 1)));
        }
        lineStart = firstEol + 2;
    }

    while (lineStart < headerBlock.size()) {
        size_t eol = headerBlock.find("\r\n", lineStart);
        if (eol == std::string::npos) {
            eol = headerBlock.size();
        }
        std::string_view line(headerBlock.data() + lineStart, eol - lineStart);
        lineStart = eol + 2;
        if (line.empty()) {
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        response.headers.emplace_back(std::string(TrimAscii(line.substr(0, colon))),
                                      std::string(TrimAscii(line.substr(colon + 1))));
    }

    const std::string lengthHeader = response.Header("Content-Length");
    if (!lengthHeader.empty()) {
        response.hasContentLength = ParseUint(lengthHeader, response.contentLength);
    }

    // Content-Range: bytes <first>-<last>/<instance-length>
    const std::string rangeHeader = response.Header("Content-Range");
    if (!rangeHeader.empty()) {
        const size_t slash = rangeHeader.rfind('/');
        if (slash != std::string::npos) {
            uint64_t total = 0;
            if (ParseUint(std::string_view(rangeHeader).substr(slash + 1), total)) {
                response.hasContentRange = true;
                response.instanceLength = total;
            }
        }
    }

    return true;
}

bool HttpConnection::ReadHeaderBlock(std::string& headerBlock, std::string* errorOut) {
    // Header bytes and the first body bytes arrive in the same TCP segments, so
    // the scan keeps whatever it over-reads in m_pending for ReadBody().
    std::string buffer;
    buffer.assign(m_pending.begin() + static_cast<std::ptrdiff_t>(m_pendingOffset), m_pending.end());
    m_pending.clear();
    m_pendingOffset = 0;

    size_t searchFrom = 0;
    char chunk[8192];

    for (;;) {
        const size_t terminator = buffer.find("\r\n\r\n", searchFrom);
        if (terminator != std::string::npos) {
            headerBlock = buffer.substr(0, terminator + 2);
            m_pending = buffer.substr(terminator + 4);
            m_pendingOffset = 0;
            return true;
        }

        if (buffer.size() > 65536) {
            Fail(errorOut, "HTTP response headers too large");
            return false;
        }
        searchFrom = (buffer.size() >= 3) ? buffer.size() - 3 : 0;

        const int got = m_stream.Recv(chunk, sizeof(chunk));
        if (got <= 0) {
            Fail(errorOut, buffer.empty() ? "server closed the connection without responding"
                                          : "connection lost while reading response headers");
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(got));
    }
}

int HttpConnection::ReadBody(void* buffer, size_t len) {
    if (len == 0) {
        return 0;
    }
    if (m_pendingOffset < m_pending.size()) {
        const size_t available = m_pending.size() - m_pendingOffset;
        const size_t take = (available < len) ? available : len;
        std::memcpy(buffer, m_pending.data() + m_pendingOffset, take);
        m_pendingOffset += take;
        if (m_pendingOffset == m_pending.size()) {
            m_pending.clear();
            m_pendingOffset = 0;
        }
        return static_cast<int>(take);
    }
    return m_stream.Recv(buffer, len);
}

bool HttpConnection::DrainBody(uint64_t byteCount) {
    char scratch[16384];
    while (byteCount > 0) {
        const size_t want = (byteCount < sizeof(scratch)) ? static_cast<size_t>(byteCount) : sizeof(scratch);
        const int got = ReadBody(scratch, want);
        if (got <= 0) {
            return false;
        }
        byteCount -= static_cast<uint64_t>(got);
    }
    return true;
}

void HttpConnection::Close() {
    m_stream.Close();
    m_pending.clear();
    m_pendingOffset = 0;
}

} // namespace od::net
