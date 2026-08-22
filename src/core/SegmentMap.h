#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace od::core {

/// One contiguous byte range of the resource, the unit of work and of resume.
struct Segment {
    uint64_t index{0};
    uint64_t firstByte{0};
    uint64_t lastByte{0};   ///< inclusive, matching HTTP Range semantics

    [[nodiscard]] uint64_t Length() const noexcept { return lastByte - firstByte + 1; }
};

/**
 * @brief Segmentation plan plus completion state, shared by all download workers.
 *
 * SIZING RATIONALE:
 * A naive "one range per worker" split is wrong over Tor, where circuit speeds
 * differ by an order of magnitude: the slowest worker dictates the finish time
 * while every other circuit sits idle. Instead the file is cut into
 * significantly more segments than there are workers and handed out on demand,
 * so a fast circuit simply takes more work. Segments must still be large enough
 * to amortise the round trip of a rendezvous circuit, hence the clamp:
 *
 *     target   = workers * kSegmentsPerWorker
 *     rawSize  = ceil(total / target)
 *     size     = clamp(roundUpToMiB(rawSize), kMinSegment, kMaxSegment)
 *     count    = ceil(total / size)
 *
 * The resulting size is recorded in the resume metadata; a plan whose
 * parameters differ from the stored ones is rejected rather than merged, which
 * makes partial files impossible to corrupt by changing the worker count.
 */
class SegmentMap {
public:
    static constexpr uint64_t kMinSegmentBytes = 2ull * 1024 * 1024;
    static constexpr uint64_t kMaxSegmentBytes = 16ull * 1024 * 1024;
    static constexpr uint64_t kSegmentsPerWorker = 8;

    /// Compute the segment size for a resource of `totalBytes` split across `workers`.
    [[nodiscard]] static uint64_t ChooseSegmentSize(uint64_t totalBytes, uint32_t workers);

    SegmentMap() = default;
    SegmentMap(uint64_t totalBytes, uint64_t segmentSize);

    SegmentMap(const SegmentMap&) = delete;
    SegmentMap& operator=(const SegmentMap&) = delete;

    /// Claim the next segment that is neither complete nor in flight.
    [[nodiscard]] std::optional<Segment> ClaimNext();

    /// Mark a claimed segment finished; its bytes are on disk.
    void MarkComplete(uint64_t index);

    /// Return a claimed segment to the pool after a failure.
    void Release(uint64_t index);

    [[nodiscard]] bool IsComplete() const;
    [[nodiscard]] uint64_t CompletedBytes() const;
    [[nodiscard]] uint64_t TotalBytes() const noexcept { return m_totalBytes; }
    [[nodiscard]] uint64_t SegmentSize() const noexcept { return m_segmentSize; }
    [[nodiscard]] uint64_t SegmentCount() const noexcept { return m_segmentCount; }

    /// Serialise the completion bitmap (hex, one nibble per four segments).
    [[nodiscard]] std::string EncodeBitmap() const;

    /// Restore a completion bitmap produced by EncodeBitmap().
    bool DecodeBitmap(const std::string& hex);

private:
    enum class State : uint8_t { Pending, InFlight, Complete };

    [[nodiscard]] Segment SegmentAt(uint64_t index) const;

    mutable std::mutex m_mutex;
    std::vector<State> m_states;
    uint64_t m_totalBytes{0};
    uint64_t m_segmentSize{0};
    uint64_t m_segmentCount{0};
    uint64_t m_nextScanHint{0};
};

} // namespace od::core
