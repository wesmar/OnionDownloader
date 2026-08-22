#include "SegmentMap.h"

#include <algorithm>

namespace od::core {

namespace {

constexpr uint64_t kMiB = 1024ull * 1024;

uint64_t CeilDiv(uint64_t numerator, uint64_t denominator) {
    return (denominator == 0) ? 0 : (numerator + denominator - 1) / denominator;
}

uint64_t RoundUpToMiB(uint64_t value) {
    return CeilDiv(value, kMiB) * kMiB;
}

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

uint64_t SegmentMap::ChooseSegmentSize(uint64_t totalBytes, uint32_t workers) {
    if (totalBytes == 0) {
        return kMinSegmentBytes;
    }
    const uint64_t workerCount = std::max<uint64_t>(workers, 1);
    const uint64_t target = workerCount * kSegmentsPerWorker;
    const uint64_t raw = CeilDiv(totalBytes, target);
    const uint64_t rounded = RoundUpToMiB(std::max<uint64_t>(raw, 1));
    return std::clamp(rounded, kMinSegmentBytes, kMaxSegmentBytes);
}

SegmentMap::SegmentMap(uint64_t totalBytes, uint64_t segmentSize)
    : m_totalBytes(totalBytes),
      m_segmentSize(std::max<uint64_t>(segmentSize, 1)),
      m_segmentCount(CeilDiv(totalBytes, std::max<uint64_t>(segmentSize, 1))) {
    m_states.assign(static_cast<size_t>(m_segmentCount), State::Pending);
}

Segment SegmentMap::SegmentAt(uint64_t index) const {
    Segment segment;
    segment.index = index;
    segment.firstByte = index * m_segmentSize;
    const uint64_t end = std::min(segment.firstByte + m_segmentSize, m_totalBytes);
    segment.lastByte = (end == 0) ? 0 : end - 1;
    return segment;
}

std::optional<Segment> SegmentMap::ClaimNext() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Scanning forward from a hint keeps allocation roughly sequential, which
    // matters for disk write locality on a 100 MB+ file.
    for (uint64_t pass = 0; pass < 2; ++pass) {
        const uint64_t start = (pass == 0) ? m_nextScanHint : 0;
        for (uint64_t i = start; i < m_segmentCount; ++i) {
            if (m_states[static_cast<size_t>(i)] == State::Pending) {
                m_states[static_cast<size_t>(i)] = State::InFlight;
                m_nextScanHint = i + 1;
                return SegmentAt(i);
            }
        }
        if (m_nextScanHint == 0) {
            break;
        }
    }
    return std::nullopt;
}

void SegmentMap::MarkComplete(uint64_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < m_segmentCount) {
        m_states[static_cast<size_t>(index)] = State::Complete;
    }
}

void SegmentMap::Release(uint64_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < m_segmentCount && m_states[static_cast<size_t>(index)] == State::InFlight) {
        m_states[static_cast<size_t>(index)] = State::Pending;
        m_nextScanHint = std::min(m_nextScanHint, index);
    }
}

bool SegmentMap::IsComplete() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::all_of(m_states.begin(), m_states.end(),
                       [](State state) { return state == State::Complete; });
}

uint64_t SegmentMap::CompletedBytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t total = 0;
    for (uint64_t i = 0; i < m_segmentCount; ++i) {
        if (m_states[static_cast<size_t>(i)] == State::Complete) {
            total += SegmentAt(i).Length();
        }
    }
    return total;
}

std::string SegmentMap::EncodeBitmap() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    static constexpr char kDigits[] = "0123456789abcdef";

    std::string hex;
    hex.reserve(static_cast<size_t>(CeilDiv(m_segmentCount, 4)));
    for (uint64_t base = 0; base < m_segmentCount; base += 4) {
        unsigned nibble = 0;
        for (uint64_t bit = 0; bit < 4; ++bit) {
            const uint64_t index = base + bit;
            if (index < m_segmentCount && m_states[static_cast<size_t>(index)] == State::Complete) {
                nibble |= (1u << bit);
            }
        }
        hex.push_back(kDigits[nibble]);
    }
    return hex;
}

bool SegmentMap::DecodeBitmap(const std::string& hex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (hex.size() != static_cast<size_t>(CeilDiv(m_segmentCount, 4))) {
        return false;
    }
    for (size_t i = 0; i < hex.size(); ++i) {
        const int nibble = HexNibble(hex[i]);
        if (nibble < 0) {
            return false;
        }
        for (uint64_t bit = 0; bit < 4; ++bit) {
            const uint64_t index = static_cast<uint64_t>(i) * 4 + bit;
            if (index >= m_segmentCount) {
                break;
            }
            m_states[static_cast<size_t>(index)] =
                ((nibble >> bit) & 1) ? State::Complete : State::Pending;
        }
    }
    m_nextScanHint = 0;
    return true;
}

} // namespace od::core
