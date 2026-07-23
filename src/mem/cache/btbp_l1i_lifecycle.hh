#ifndef __MEM_CACHE_BTBP_L1I_LIFECYCLE_HH__
#define __MEM_CACHE_BTBP_L1I_LIFECYCLE_HH__

#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/logging.hh"
#include "base/types.hh"
#include "mem/cache/cache_blk.hh"

namespace gem5
{

class BtbpL1iLifecycleLedger
{
  public:
    struct LineKey
    {
        Addr lineAddr = 0;
        bool secure = false;

        bool operator==(const LineKey &other) const
        {
            return lineAddr == other.lineAddr && secure == other.secure;
        }
    };

    struct Residency
    {
        uint64_t id = 0;
        bool prefetchSource = false;
        bool roiOrigin = false;
        bool demandUsed = false;
    };

  private:
    struct LineKeyHash
    {
        std::size_t operator()(const LineKey &key) const
        {
            return std::hash<Addr>{}(key.lineAddr) ^
                (std::hash<bool>{}(key.secure) << 1);
        }
    };

    std::unordered_map<LineKey, Residency, LineKeyHash> activeByLine;
    std::unordered_map<uint64_t, LineKey> lineByResidency;
    const std::size_t maxActiveLines;

  public:
    explicit BtbpL1iLifecycleLedger(
        std::size_t max_active_lines =
            std::numeric_limits<std::size_t>::max())
        : maxActiveLines(max_active_lines)
    {
        panic_if(maxActiveLines == 0,
                 "BTBP lifecycle capacity must be nonzero");
    }

    std::optional<Residency> install(
        const LineKey &key, const Residency &residency)
    {
        panic_if(residency.id == 0, "BTBP residency id must be nonzero");
        panic_if(lineByResidency.find(residency.id) !=
                     lineByResidency.end(),
                 "BTBP residency id %llu is not unique", residency.id);

        if (const auto current = activeByLine.find(key);
            current != activeByLine.end()) {
            return current->second;
        }
        panic_if(activeByLine.size() >= maxActiveLines,
                 "BTBP active L1I residencies exceed capacity %llu",
                 static_cast<unsigned long long>(maxActiveLines));
        activeByLine.emplace(key, residency);
        lineByResidency.emplace(residency.id, key);
        return std::nullopt;
    }

    bool close(const LineKey &key, uint64_t residency_id)
    {
        const auto current = activeByLine.find(key);
        if (current == activeByLine.end() ||
            current->second.id != residency_id) {
            return false;
        }
        activeByLine.erase(current);
        return lineByResidency.erase(residency_id) == 1;
    }

    bool markDemandUse(const LineKey &key, uint64_t residency_id)
    {
        const auto current = activeByLine.find(key);
        if (current == activeByLine.end() ||
            current->second.id != residency_id) {
            return false;
        }
        current->second.demandUsed = true;
        return true;
    }

    bool isCurrent(const LineKey &key, uint64_t residency_id) const
    {
        const auto current = activeByLine.find(key);
        const auto reverse = lineByResidency.find(residency_id);
        return current != activeByLine.end() &&
            current->second.id == residency_id &&
            reverse != lineByResidency.end() && reverse->second == key;
    }

    std::vector<std::pair<LineKey, Residency>> drainRoiPrefetch()
    {
        std::vector<std::pair<LineKey, Residency>> drained;
        for (auto current = activeByLine.begin();
             current != activeByLine.end();) {
            if (!current->second.prefetchSource ||
                !current->second.roiOrigin) {
                ++current;
                continue;
            }
            drained.emplace_back(current->first, current->second);
            lineByResidency.erase(current->second.id);
            current = activeByLine.erase(current);
        }
        return drained;
    }

    std::size_t size() const
    {
        return activeByLine.size();
    }
};

inline bool
closeBtbpL1iBlockResidency(
    BtbpL1iLifecycleLedger &ledger,
    const BtbpL1iLifecycleLedger::LineKey &key,
    const CacheBlk &block)
{
    const uint64_t residency_id =
        block.getXsMetadata().l1iResidencyId;
    return residency_id != 0 && ledger.close(key, residency_id);
}

inline bool
markBtbpL1iDemandUse(
    BtbpL1iLifecycleLedger &ledger,
    const BtbpL1iLifecycleLedger::LineKey &key,
    const CacheBlk &block)
{
    const uint64_t residency_id =
        block.getXsMetadata().l1iResidencyId;
    return residency_id != 0 &&
        ledger.markDemandUse(key, residency_id);
}

template <class CloseCallback>
void
transitionBtbpL1iFillMetadata(
    BtbpL1iLifecycleLedger &ledger,
    const BtbpL1iLifecycleLedger::LineKey &key,
    CacheBlk &block,
    const Request::XsMetadata &new_metadata,
    bool has_old_data,
    bool prefetch_source,
    CloseCallback &&on_close)
{
    const Request::XsMetadata old_metadata = block.getXsMetadata();
    if (has_old_data && old_metadata.l1iResidencyId != 0) {
        panic_if(!closeBtbpL1iBlockResidency(ledger, key, block),
                 "BTBP same-line refill could not close residency %llu",
                 old_metadata.l1iResidencyId);
        on_close(old_metadata);
    }
    if (new_metadata.l1iResidencyId != 0) {
        panic_if(ledger.install(
                     key,
                     {new_metadata.l1iResidencyId,
                      prefetch_source,
                      new_metadata.btbpRoiOrigin,
                      false}),
                 "BTBP L1I line %#llx (%s) remained active after close",
                 key.lineAddr, key.secure ? "secure" : "non-secure");
    }
    block.setXsMetadata(new_metadata);
}

inline void
completeBtbpL1iPrefetch(
    BtbpL1iLifecycleLedger &ledger,
    const BtbpL1iLifecycleLedger::LineKey &key,
    CacheBlk &block,
    bool mark_prefetched)
{
    panic_if(!block.isValid(),
             "BTBP prefetch completion lacks a tracked valid L1I block");
    if (mark_prefetched) {
        block.setPrefetched();
    }
    const auto metadata = block.getXsMetadata();
    panic_if(metadata.l1iResidencyId == 0,
             "BTBP prefetch completion cleared its block residency");
    panic_if(!ledger.isCurrent(key, metadata.l1iResidencyId),
             "BTBP prefetch completion block/ledger residency mismatch: "
             "line %#llx (%s), id %llu",
             key.lineAddr, key.secure ? "secure" : "non-secure",
             metadata.l1iResidencyId);
}

} // namespace gem5

#endif // __MEM_CACHE_BTBP_L1I_LIFECYCLE_HH__
