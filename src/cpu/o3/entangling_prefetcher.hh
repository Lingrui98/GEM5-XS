#ifndef __CPU_O3_ENTANGLING_PREFETCHER_HH__
#define __CPU_O3_ENTANGLING_PREFETCHER_HH__

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace gem5
{

namespace o3
{

class EntanglingPrefetcherTestPeer;

/**
 * Functional port of the official ISCA 2021 and TC 2024 Entangling
 * Instruction Prefetcher bundles.
 *
 * All virtualLine and physicalLine fields are cache-line numbers: callers
 * must shift byte addresses right by six before invoking this interface.
 * The algorithm is keyed by virtual line, as in the official source. Real
 * GEM5 fill and eviction events provide physical-line residency instead of
 * the source model's approximate timing-cache geometry.
 */
class EntanglingPrefetcher
{
  public:
    enum class AlgorithmVersion : std::uint8_t
    {
        Tc2024,
        Isca2021,
    };

    enum class CandidateKind : std::uint8_t
    {
        BasicBlock,
        Entangled,
        EntangledBasicBlock,
    };

    static constexpr std::uint32_t NoSourceWay = 16;

    struct Candidate
    {
        std::uint64_t virtualLine = 0;
        std::uint64_t triggerVirtualLine = 0;
        std::uint64_t entangledRootVirtualLine = 0;
        std::uint32_t sourceSet = 0;
        std::uint32_t sourceWay = NoSourceWay;
        CandidateKind kind = CandidateKind::BasicBlock;
    };

    using AcceptCallback = std::function<bool(const Candidate &)>;

    struct DemandAccess
    {
        std::uint64_t virtualLine = 0;
        std::uint64_t physicalLine = 0;
        std::uint64_t cycle = 0;
        std::uint64_t instructionId = 0;
        bool cacheHit = false;
        bool prefetchHit = false;
        // TC 2024 uses this only to classify retained wrong-path history.
        bool wrongPath = false;
    };

    struct FillEvent
    {
        std::uint64_t virtualLine = 0;
        std::uint64_t physicalLine = 0;
        std::uint64_t cycle = 0;
    };

    struct EvictEvent
    {
        std::uint64_t physicalLine = 0;
        std::uint64_t cycle = 0;
    };

    struct SquashEvent
    {
        std::uint64_t virtualLine = 0;
        std::uint64_t instructionId = 0;
        std::uint64_t cycle = 0;
    };

    struct DemandResult
    {
        std::uint32_t candidatesOffered = 0;
        std::uint32_t candidatesAccepted = 0;
        bool repeatedCurrentLine = false;
    };

    struct BundleInfo
    {
        std::uint32_t historyEntries = 0;
        std::uint32_t historySourceSearches = 0;
        std::uint32_t basicBlockMergeEntries = 0;
        bool supportsWrongPathSquash = false;
        bool delaysTableInsertion = false;
        bool decrementsRedundantDestinations = false;
    };

    struct Stats
    {
        std::uint64_t demandAccesses = 0;
        std::uint64_t demandMisses = 0;
        std::uint64_t prefetchHits = 0;
        std::uint64_t latePrefetches = 0;
        std::uint64_t wrongPrefetches = 0;
        std::uint64_t candidatesOffered = 0;
        std::uint64_t candidatesAccepted = 0;
        std::uint64_t candidatesRejected = 0;
        std::uint64_t acceptedRequestsCanceled = 0;
        std::uint64_t untrackedDemandHits = 0;
        std::uint64_t untrackedEvictions = 0;
        std::uint64_t overwrittenResidencies = 0;
    };

    explicit EntanglingPrefetcher(
        AlgorithmVersion version, std::uint64_t initialCycle = 0);
    ~EntanglingPrefetcher();

    EntanglingPrefetcher(const EntanglingPrefetcher &) = delete;
    EntanglingPrefetcher &operator=(const EntanglingPrefetcher &) = delete;
    EntanglingPrefetcher(EntanglingPrefetcher &&) noexcept;
    EntanglingPrefetcher &operator=(EntanglingPrefetcher &&) noexcept;

    DemandResult observeDemand(
        const DemandAccess &access, const AcceptCallback &accept);
    void observeFill(const FillEvent &event);
    bool observeEvict(const EvictEvent &event);
    void observeSquash(const SquashEvent &event);

    /**
     * Remove an accepted prefetch which cannot reach the cache hierarchy.
     * This is an adapter-only lifecycle action and performs no learning.
     */
    bool cancelAcceptedRequest(std::uint64_t virtualLine);

    AlgorithmVersion version() const;
    BundleInfo bundleInfo() const;
    const Stats &stats() const;
    std::size_t inflightRequests() const;
    std::size_t residentLines() const;
    bool hasResidentVirtualLine(std::uint64_t virtualLine) const;

    /** Return a deterministic digest of algorithm and lifecycle state. */
    std::uint64_t stateDigest() const;

    /** Return the shared paper-algorithm state used by the source harness. */
    std::uint64_t algorithmStateDigest() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    static std::uint32_t debugFormat(
        std::uint64_t source, std::uint64_t destination);
    static std::uint64_t debugCompress(
        std::uint64_t destination, std::uint32_t format);
    static std::uint64_t debugExtend(
        std::uint64_t source, std::uint64_t destination,
        std::uint32_t format);
    static std::uint64_t debugHash(std::uint64_t line);
    void debugAddEntangled(
        std::uint64_t source, std::uint64_t destination);
    void debugAddBasicBlockSize(std::uint64_t line, std::uint32_t size);
    std::uint32_t debugConfidence(
        std::uint64_t source, std::uint64_t destination) const;
    std::uint32_t debugBasicBlockSize(std::uint64_t line) const;
    std::uint32_t debugFindHistory(std::uint64_t line) const;
    std::uint32_t debugHistoryHead() const;
    std::uint64_t debugHistoryTag(std::uint32_t index) const;
    std::uint64_t debugHistoryTimeDiff(std::uint32_t index) const;
    std::uint64_t debugHistoryInstructionId(std::uint32_t index) const;
    std::uint64_t debugHistoryEntangledSource(std::uint32_t index) const;
    std::uint32_t debugHistoryBasicBlockSize(std::uint32_t index) const;
    bool debugTimingAccessed(std::uint64_t line) const;
    std::uint32_t debugTimingSourceWay(std::uint64_t line) const;

    friend class EntanglingPrefetcherTestPeer;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_ENTANGLING_PREFETCHER_HH__
