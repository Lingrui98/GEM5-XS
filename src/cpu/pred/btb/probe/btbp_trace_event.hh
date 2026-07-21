#ifndef __CPU_PRED_BTB_PROBE_BTBP_TRACE_EVENT_HH__
#define __CPU_PRED_BTB_PROBE_BTBP_TRACE_EVENT_HH__

#include "base/types.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

struct BtbpTraceEvent
{
    static constexpr uint32_t SchemaVersion = 5;

    enum EventType : uint32_t
    {
        MbtbLookup = 1,
        MbtbFill = 2,
        IPrefetchIssue = 3,
        LineLifecycle = 4,
        L1IDemandAccess = 5,
        DecodeBranch = 6,
        BranchDemand = 7,
        IPrefetchDecisionAccepted = 8,
        IPrefetchTerminal = 9,
        L1IMshrOccupancy = 10,
        L1ILineEvict = 11,
        RoiBegin = 12,
        RoiEnd = 13,
    };

    enum class TerminalReason : uint32_t
    {
        Unknown = 0,
        Completed = 1,
        TranslationFault = 2,
        Uncacheable = 3,
        CacheOrMergeCompletion = 4,
        QueueFullCompletion = 5,
        ResetCanceled = 6,
        PolicyFiltered = 7,
    };

    enum class LifecycleKind : uint32_t
    {
        Unknown = 0,
        PrefetchFill = 1,
        DemandFill = 2,
        DemandUse = 3,
        Evict = 4,
        RoiDrainClose = 5,
    };

    enum FillSource : uint32_t
    {
        ExecWriteback = 1,
        InternalVictimMove = 3,
    };

    enum RequestKind : uint32_t
    {
        UnknownRequest = 0,
        DemandRequest = 1,
        PrefetchRequest = 2,
    };

    enum BranchKind : uint32_t
    {
        UnknownBranch = 0,
        DirectBranch = 1,
        IndirectBranch = 2,
        ReturnBranch = 3,
    };

    Tick tick = 0;
    uint32_t eventType = 0;
    ThreadID threadId = 0;

    Addr branchPc = 0;
    bool branchPcValid = false;
    bool hit = false;
    bool hitValid = false;
    Addr target = 0;
    bool targetValid = false;
    uint32_t fillSource = 0;
    bool fillSourceValid = false;
    Addr lineAddr = 0;
    bool lineAddrValid = false;
    Addr virtualLineAddr = 0;
    bool virtualLineAddrValid = false;
    Addr triggerPc = 0;
    bool triggerPcValid = false;
    bool takenHint = false;
    bool takenHintValid = false;
    uint32_t lineSize = 0;
    bool lineSizeValid = false;
    uint64_t addressSpaceId = 0;
    bool addressSpaceIdValid = false;
    uint32_t asidHash = 0;
    bool asidHashValid = false;
    uint64_t ftqId = 0;
    bool ftqIdValid = false;
    uint64_t fdipEpoch = 0;
    bool fdipEpochValid = false;
    Tick lookupTick = 0;
    bool lookupTickValid = false;
    Tick fillBytesTick = 0;
    bool fillBytesTickValid = false;
    Tick l1iReadyTick = 0;
    bool l1iReadyTickValid = false;
    Tick scanCompleteTick = 0;
    bool scanCompleteTickValid = false;
    uint32_t instBytes = 0;
    bool instBytesValid = false;
    Addr streamStartPc = 0;
    bool streamStartPcValid = false;
    bool structuralMiss = false;
    bool structuralMissValid = false;
    uint32_t requestKind = UnknownRequest;
    bool requestKindValid = false;
    uint32_t branchKind = UnknownBranch;
    bool branchKindValid = false;
    uint64_t demandUid = 0;
    bool demandUidValid = false;
    uint64_t prefetchDecisionId = 0;
    bool prefetchDecisionIdValid = false;
    uint32_t prefetchSource = 0;
    bool prefetchSourceValid = false;
    uint32_t terminalReason =
        static_cast<uint32_t>(TerminalReason::Unknown);
    bool terminalReasonValid = false;
    uint32_t mshrDemandOwned = 0;
    bool mshrDemandOwnedValid = false;
    uint32_t mshrInstPrefetchOwned = 0;
    bool mshrInstPrefetchOwnedValid = false;
    uint32_t mshrTotal = 0;
    bool mshrTotalValid = false;
    uint32_t lifecycleKind =
        static_cast<uint32_t>(LifecycleKind::Unknown);
    bool lifecycleKindValid = false;
    uint64_t residencyId = 0;
    bool residencyIdValid = false;
    uint64_t coreCycle = 0;
    bool coreCycleValid = false;
    uint64_t committedInsts = 0;
    bool committedInstsValid = false;
    uint64_t roiInsts = 0;
    bool roiInstsValid = false;
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_PROBE_BTBP_TRACE_EVENT_HH__
