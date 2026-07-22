#ifndef __MEM_CACHE_BTBP_L1I_LIFECYCLE_HH__
#define __MEM_CACHE_BTBP_L1I_LIFECYCLE_HH__

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/logging.hh"
#include "base/types.hh"

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

  public:
    std::optional<Residency> install(
        const LineKey &key, const Residency &residency)
    {
        panic_if(residency.id == 0, "BTBP residency id must be nonzero");
        panic_if(lineByResidency.find(residency.id) !=
                     lineByResidency.end(),
                 "BTBP residency id %llu is not unique", residency.id);

        std::optional<Residency> previous;
        if (const auto current = activeByLine.find(key);
            current != activeByLine.end()) {
            previous = current->second;
            lineByResidency.erase(current->second.id);
            current->second = residency;
        } else {
            activeByLine.emplace(key, residency);
        }
        lineByResidency.emplace(residency.id, key);
        return previous;
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

} // namespace gem5

#endif // __MEM_CACHE_BTBP_L1I_LIFECYCLE_HH__
