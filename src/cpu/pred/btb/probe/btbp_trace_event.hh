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
    enum EventType : uint32_t
    {
        BtbLookup = 1,
        BtbFill = 2,
        IPrefetchIssue = 3,
        IPrefetchFill = 4,
        IcacheDemandFill = 5,
        DecodeBranch = 6,
    };

    enum BtbLevel : uint32_t
    {
        UBTB = 0,
        AheadBTB = 1,
        MBTB = 2,
    };

    enum FillSource : uint32_t
    {
        DecodeWriteback = 0,
        OracleScan = 1,
        PrefetchScan = 2,
        ExecWriteback = 3,
        CoherenceEvict = 4,
    };

    Tick tick = 0;
    uint32_t eventType = 0;
    ThreadID threadId = 0;
    Addr branchPc = 0;
    uint32_t btbLevel = 0;
    bool hit = false;
    Addr target = 0;
    uint32_t fillSource = 0;
    Addr lineAddr = 0;
    Addr triggerPc = 0;
    bool takenHint = false;
    bool wasInBtbAtLookup = false;
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_PROBE_BTBP_TRACE_EVENT_HH__
