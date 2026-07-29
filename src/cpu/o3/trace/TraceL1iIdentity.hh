#ifndef __CPU_O3_TRACE_TRACE_L1I_IDENTITY_HH__
#define __CPU_O3_TRACE_TRACE_L1I_IDENTITY_HH__

#include <array>
#include <cstdint>
#include <stdexcept>

#include "base/types.hh"
#include "cpu/o3/limits.hh"

namespace gem5
{
namespace o3
{

struct TraceL1iLookupIdentity
{
    uint64_t lookupUid = 0;
    uint64_t fetchEpoch = 0;
};

struct TraceL1iRequestIdentity
{
    uint64_t requestUid = 0;
    uint64_t fetchEpoch = 0;
};

struct TraceL1iFetchIdentity
{
    uint64_t requestUid = 0;
    uint64_t lookupUid = 0;
    uint64_t fetchEpoch = 0;
};

/**
 * Allocate dynamic trace-fetch identities independently from the diagnostic
 * FTQ number. A squash advances the thread epoch; the next prediction always
 * receives fresh request and lookup UIDs even if the FTQ number is reused.
 */
class TraceL1iIdentityGenerator
{
  private:
    uint64_t nextRequestUid = 1;
    uint64_t nextLookupUid = 1;
    std::array<uint64_t, MaxThreads> fetchEpoch{};

  public:
    TraceL1iIdentityGenerator()
    {
        fetchEpoch.fill(1);
    }

    TraceL1iLookupIdentity allocateLookup(ThreadID tid)
    {
        if (tid >= MaxThreads) {
            throw std::out_of_range("trace L1I identity thread out of range");
        }
        return {nextLookupUid++, fetchEpoch[tid]};
    }

    TraceL1iRequestIdentity allocateRequest(ThreadID tid)
    {
        if (tid >= MaxThreads) {
            throw std::out_of_range("trace L1I identity thread out of range");
        }
        return {nextRequestUid++, fetchEpoch[tid]};
    }

    uint64_t advanceEpoch(ThreadID tid)
    {
        if (tid >= MaxThreads) {
            throw std::out_of_range("trace L1I identity thread out of range");
        }
        return ++fetchEpoch[tid];
    }

    uint64_t currentEpoch(ThreadID tid) const
    {
        if (tid >= MaxThreads) {
            throw std::out_of_range("trace L1I identity thread out of range");
        }
        return fetchEpoch[tid];
    }
};

/**
 * Minimal logical-demand state shared by production helpers and unit tests.
 * Retry attempts advance an ordinal without changing either logical UID.
 */
struct TraceL1iDemandIdentity
{
    uint64_t requestUid = 0;
    uint64_t demandUid = 0;
    uint32_t attempts = 0;
    bool terminal = false;

    uint32_t beginAttempt()
    {
        if (terminal) {
            throw std::logic_error(
                "trace L1I demand attempt after terminal");
        }
        return attempts++;
    }

    bool close()
    {
        if (terminal) {
            return false;
        }
        terminal = true;
        return true;
    }
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_TRACE_L1I_IDENTITY_HH__
