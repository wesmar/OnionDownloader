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

std::string SanitizeRequestPath(std::string_view rawPath) {
    if (rawPath.empty()) {
        return "/";
    }
    std::string out;
    out.reserve(rawPath.size() + 8);
    for (unsigned char ch : rawPath) {
        if (ch == ' ') {
            out += "%20";
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

void Sha256Block(uint32_t state[8], const uint8_t block[64]) {
    static constexpr uint32_t kK[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    const auto rotr = [](uint32_t x, unsigned n) -> uint32_t { return (x >> n) | (x << (32 - n)); };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + S1 + ch + kK[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void Sha256Words(std::string_view input, uint32_t outState[8]) {
    outState[0] = 0x6a09e667u; outState[1] = 0xbb67ae85u;
    outState[2] = 0x3c6ef372u; outState[3] = 0xa54ff53au;
    outState[4] = 0x510e527fu; outState[5] = 0x9b05688cu;
    outState[6] = 0x1f83d9abu; outState[7] = 0x5be0cd19u;

    const auto* bytes = reinterpret_cast<const uint8_t*>(input.data());
    size_t len = input.size();
    size_t offset = 0;
    while (len - offset >= 64) {
        Sha256Block(outState, bytes + offset);
        offset += 64;
    }
    uint8_t tail[128] = {};
    const size_t rem = len - offset;
    if (rem > 0) {
        std::memcpy(tail, bytes + offset, rem);
    }
    tail[rem] = 0x80;
    const size_t totalBlocks = (rem >= 56) ? 2 : 1;
    const uint64_t bitLen = static_cast<uint64_t>(len) * 8;
    const size_t lenPos = totalBlocks * 64 - 8;
    for (int i = 0; i < 8; ++i) {
        tail[lenPos + i] = static_cast<uint8_t>((bitLen >> (56 - i * 8)) & 0xFFu);
    }
    Sha256Block(outState, tail);
    if (totalBlocks == 2) {
        Sha256Block(outState, tail + 64);
    }
}

bool ParseHexDigest256(std::string_view hex, uint32_t outWords[8]) {
    if (hex.size() != 64) {
        return false;
    }
    for (int w = 0; w < 8; ++w) {
        uint32_t val = 0;
        for (int i = 0; i < 8; ++i) {
            const int nib = HexValue(hex[static_cast<size_t>(w * 8 + i)]);
            if (nib < 0) return false;
            val = (val << 4) | static_cast<uint32_t>(nib);
        }
        outWords[w] = val;
    }
    return true;
}

std::string Base64Encode(std::string_view input) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    const auto* data = reinterpret_cast<const uint8_t*>(input.data());
    size_t i = 0;
    for (; i + 2 < input.size(); i += 3) {
        const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                                (static_cast<uint32_t>(data[i + 1]) << 8) |
                                static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back(kTable[(triple >> 6) & 0x3F]);
        out.push_back(kTable[triple & 0x3F]);
    }
    if (i < input.size()) {
        uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < input.size()) {
            triple |= static_cast<uint32_t>(data[i + 1]) << 8;
        }
        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < input.size()) ? kTable[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::string ExtractJsonValue(std::string_view json, std::string_view key) {
    std::string needle = "\"";
    needle += key;
    needle += "\":";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return {};
    if (json[pos] == '"') {
        const size_t end = json.find('"', pos + 1);
        return (end == std::string_view::npos) ? std::string{} : std::string(json.substr(pos + 1, end - pos - 1));
    }
    const size_t end = json.find_first_of(",}\r\n ", pos);
    return std::string(json.substr(pos, (end == std::string_view::npos ? json.size() : end) - pos));
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
    out.path = SanitizeRequestPath(path);
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
        out.path = SanitizeRequestPath(location);
    } else {
        const size_t lastSlash = base.path.rfind('/');
        out.path = SanitizeRequestPath(
            base.path.substr(0, lastSlash == std::string::npos ? 0 : lastSlash + 1) + std::string(location));
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
                                  std::string* errorOut,
                                  std::string_view cookie) {
    if (!m_stream.IsValid()) {
        Fail(errorOut, "connection is closed");
        return false;
    }

    std::string request;
    request.reserve(360);
    request += "GET ";
    request += url.path;
    request += " HTTP/1.1\r\nHost: ";
    request += url.Authority();
    request += "\r\nUser-Agent: ";
    request += kDefaultUserAgent;
    request += "\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n";
    if (!cookie.empty()) {
        request += "Cookie: ";
        request += cookie;
        request += "\r\n";
    }
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

bool HttpConnection::SolvePoWGate(const Url& originUrl, std::string& cookieOut, std::string* errorOut) {
    if (!m_stream.IsValid()) {
        Fail(errorOut, "connection is closed");
        return false;
    }

    Url challengeUrl = originUrl;
    challengeUrl.path = "/_gate/challenge";
    HttpResponse chResp;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (!RequestRange(challengeUrl, false, 0, 0, chResp, errorOut)) {
            return false;
        }
        if (chResp.statusCode == 429) {
            if (chResp.hasContentLength && chResp.contentLength > 0 && chResp.contentLength <= 65536) {
                DrainBody(chResp.contentLength);
            }
            ::Sleep(2000);
            continue;
        }
        break;
    }
    if (chResp.statusCode != 200 || !chResp.hasContentLength || chResp.contentLength == 0 || chResp.contentLength > 65536) {
        Fail(errorOut, "PoW challenge endpoint returned HTTP " + std::to_string(chResp.statusCode));
        return false;
    }

    std::string chBody(static_cast<size_t>(chResp.contentLength), '\0');
    size_t readTotal = 0;
    while (readTotal < chBody.size()) {
        const int got = ReadBody(chBody.data() + readTotal, chBody.size() - readTotal);
        if (got <= 0) {
            Fail(errorOut, "reading PoW challenge failed");
            return false;
        }
        readTotal += static_cast<size_t>(got);
    }

    const std::string algorithm = ExtractJsonValue(chBody, "algorithm");
    const std::string challengeHex = ExtractJsonValue(chBody, "challenge");
    const std::string maxNumStr = ExtractJsonValue(chBody, "maxnumber");
    const std::string salt = ExtractJsonValue(chBody, "salt");
    const std::string signature = ExtractJsonValue(chBody, "signature");

    uint64_t maxNumber = 100000;
    ParseUint(maxNumStr, maxNumber);
    if (maxNumber > 5000000) {
        maxNumber = 5000000;
    }

    uint32_t targetWords[8] = {};
    if (salt.empty() || signature.empty() || !ParseHexDigest256(challengeHex, targetWords)) {
        Fail(errorOut, "invalid PoW challenge format");
        return false;
    }

    const ULONGLONG t0 = ::GetTickCount64();
    int64_t solvedN = -1;
    std::string candidate;
    candidate.reserve(salt.size() + 16);
    for (uint64_t n = 0; n <= maxNumber; ++n) {
        candidate.assign(salt);
        candidate += std::to_string(n);
        uint32_t digest[8];
        Sha256Words(candidate, digest);
        if (std::memcmp(digest, targetWords, sizeof(digest)) == 0) {
            solvedN = static_cast<int64_t>(n);
            break;
        }
    }
    if (solvedN < 0) {
        Fail(errorOut, "no solution found for PoW challenge");
        return false;
    }

    const uint64_t tookMs = std::max<uint64_t>(50, ::GetTickCount64() - t0);
    std::string innerJson = "{\"algorithm\":\"" + (algorithm.empty() ? std::string("SHA-256") : algorithm) +
                            "\",\"challenge\":\"" + challengeHex +
                            "\",\"number\":" + std::to_string(solvedN) +
                            ",\"salt\":\"" + salt +
                            "\",\"signature\":\"" + signature +
                            "\",\"took\":" + std::to_string(tookMs) + "}";
    const std::string postBody = "{\"payload\":\"" + Base64Encode(innerJson) + "\"}";

    std::string postReq;
    postReq.reserve(384 + postBody.size());
    postReq += "POST /_gate/verify HTTP/1.1\r\nHost: ";
    postReq += originUrl.Authority();
    postReq += "\r\nUser-Agent: ";
    postReq += kDefaultUserAgent;
    postReq += "\r\nAccept: */*\r\nContent-Type: application/json\r\nContent-Length: ";
    postReq += std::to_string(postBody.size());
    postReq += "\r\nConnection: keep-alive\r\n\r\n";
    postReq += postBody;

    if (!m_stream.SendAll(postReq.data(), postReq.size())) {
        Fail(errorOut, "sending PoW verification failed");
        return false;
    }

    std::string verifyHeaderBlock;
    if (!ReadHeaderBlock(verifyHeaderBlock, errorOut)) {
        return false;
    }

    std::string setCookie;
    uint64_t verifyLen = 0;
    bool hasVerifyLen = false;
    size_t lineStart = 0;
    while (lineStart < verifyHeaderBlock.size()) {
        size_t eol = verifyHeaderBlock.find("\r\n", lineStart);
        if (eol == std::string::npos) eol = verifyHeaderBlock.size();
        std::string_view line(verifyHeaderBlock.data() + lineStart, eol - lineStart);
        lineStart = eol + 2;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        std::string_view hKey = TrimAscii(line.substr(0, colon));
        std::string_view hVal = TrimAscii(line.substr(colon + 1));
        if (EqualsIgnoreCaseAscii(hKey, "Set-Cookie")) {
            const size_t semi = hVal.find(';');
            setCookie = std::string(TrimAscii(semi == std::string_view::npos ? hVal : hVal.substr(0, semi)));
        } else if (EqualsIgnoreCaseAscii(hKey, "Content-Length")) {
            hasVerifyLen = ParseUint(hVal, verifyLen);
        }
    }
    if (hasVerifyLen && verifyLen > 0 && verifyLen <= 65536) {
        DrainBody(verifyLen);
    }

    if (setCookie.empty()) {
        Fail(errorOut, "PoW gate did not return a session cookie");
        return false;
    }
    cookieOut = std::move(setCookie);
    return true;
}

} // namespace od::net
