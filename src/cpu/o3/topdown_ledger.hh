/*
 * Copyright (c) 2026 The Regents of the University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_TOPDOWN_LEDGER_HH__
#define __CPU_O3_TOPDOWN_LEDGER_HH__

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/statistics.hh"
#include "cpu/o3/comm.hh"

namespace gem5
{

namespace o3
{

/**
 * @file topdown_ledger.hh
 *
 * D4-B dispatch-slot conservation ledger (statistical group "topdownV2").
 *
 * Design contract (v4, frozen):
 *   btb_prefetch_paper/.local/btb-morphology-spike-20260910/
 *   d4b-design/D4B_DESIGN_CONTRACT_DRAFT.md
 *
 * The ledger is a pure accounting device: none of its outputs feed any
 * pipeline decision.  The cut point is C4 (IEW dispatch event,
 * iew.cc dispatchedInsts registration point): every DynInst that crosses
 * C4 is registered exactly once, and every settlement (retirement or
 * first squash observation) is routed by the registration epoch stamp.
 *
 * Semantics summary (contract sections in parentheses):
 *   - Identity I  (per cycle):  budget = crossings + unused slots, and the
 *     unused slots are partitioned into FE/BE/Recovery/Drain/Unknown by
 *     classifyDispatchCycle (policy P1..P6, section 1.5b).
 *   - Identity II (cohort):    allocated = retiredCohort + squashedCohort
 *     + liveAtClose at snapshot time (Option B end-of-tick anchor).
 *   - Conservation is a bookkeeping property, NOT a causal proof
 *     (section 1.0): misclassification can coexist with a closed ledger.
 *
 * Engineering note (reported deviation from the contract's sketch
 * signature): the hooks operate on a stats-only LedgerTag attached to
 * DynInst plus the instruction pointer as identity, instead of taking
 * DynInstPtr.  This keeps the ledger free of DynInst/CPU dependencies so
 * that the production functions are directly unit-testable (contract
 * section 5a requires direct-linked GTests of the production code, not
 * duplicated inline models).  The CPU-facing wrappers live in cpu.cc.
 */

/** Slot classification for unused dispatch slots (contract section 1.3). */
enum class SlotClass : uint8_t
{
    Frontend = 0,
    Backend,
    Recovery,
    Drain,
    Unknown,
    NumSlotClasses  // sentinel count; NOT a class
};

/** Per-DynInst ledger state (stats-only; contract section 1.4). */
enum class LedgerState : uint8_t
{
    Unallocated = 0,
    Live,
    Retired,
    Squashed
};

/** Whole-ledger run state (contract section 1.6). */
enum class LedgerRunState : uint8_t
{
    Valid = 0,
    Unsupported,
    Invalid
};

/** Observation point a settlement was seen at (contract section 1.4). */
enum class SettleOrigin : uint8_t
{
    Retire = 0,     // point 1: ROB::retireHead (commitHead success)
    CommitBelt,     // point 2: ROB::drainSquashedHead (commit-side belt)
    PreCutBelt,     // commit.cc moveInstsToBuffer skip branch (pre-C4 belt)
    RobWalk,        // point 8: ROB::doSquash walk erase
    IQ,             // IssueQue::doSquash erase
    LSQ,            // LSQUnit squash pops (load/store queue)
    ExecSkip,       // IEW::executeInsts squashed skip
    DispQue,        // IEW::squash dispQue marking (dead path when
                    // enableDispatchStage=false, hooked anyway)
    BufferSquash    // IEW::moveInstsToBuffer squash marking (pre-C4 belt)
};

/**
 * Stats-only tag embedded in DynInst.  Never read by any timing or
 * arbitration decision; the ledger is its only producer/consumer.
 */
struct LedgerTag
{
    LedgerState state = LedgerState::Unallocated;
    /** Epoch stamp taken at registration (beginEpoch increments it). */
    uint32_t epoch = 0;
};

/**
 * Evidence layer: the complete same-cycle raw signal tuple (contract
 * section 1.5a).  Every field is measured at the classification point
 * (IEW phase, after the dispatch loop).  This exact tuple is what the
 * lossless audit table stores, so it doubles as the replay key.
 */
struct DispatchCycleInputs
{
    /** W_C4 slots budgeted for this cycle (renameWidth). */
    unsigned budget = 0;
    /** Dispatch-buffer residue at cycle start (break leftovers). */
    unsigned residueAtStart = 0;
    /** Instructions that arrived from rename this cycle (wire size). */
    unsigned arrivals = 0;
    /** Instructions that crossed C4 this cycle (dispatchedInsts delta). */
    unsigned crossings = 0;
    /** Pre-cut squashed pops from the dispatch buffer this cycle. */
    unsigned poppedSquashed = 0;

    /** ROI drain mode active (fetch-side, same cycle). */
    bool drainMode = false;
    /** Direct cause of a dispatch-loop break (NoStall = no break). */
    StallReason breakReason = StallReason::NoStall;

    /** Commit-side admission block (allocator stalls; same cycle). */
    bool blockIEW = false;
    StallReason iewBlockReason = StallReason::NoStall;
    /** Rename-path admission block set by IEW this cycle. */
    bool blockRename = false;
    StallReason renameBlockReason = StallReason::NoStall;

    /**
     * Lagged upstream chain hop reasons (NoStall = silent hop;
     * kChainMixedStall = same-hop heterogeneous vector, see
     * foldChainHop).  Admission-side fields (above) must never carry
     * kChainMixedStall: it is a chain-role-only sentinel.
     */
    StallReason renameUp = StallReason::NoStall;
    StallReason decodeUp = StallReason::NoStall;
    StallReason fetchUp = StallReason::NoStall;

    /**
     * Per-hop distinct non-NoStall reason bit sets (bit r set iff
     * StallReason value r is present in that hop's per-slot vector).
     * Lossless audit encoding of the chain evidence: the frozen policy
     * consumes only the distinct-reason set, never multiplicity or
     * order.  Kept in the replay key alongside the representative
     * single values above.
     */
    uint64_t renameUpReasons = 0;
    uint64_t decodeUpReasons = 0;
    uint64_t fetchUpReasons = 0;

    /** Lagged commit-side squash wires (informational only). */
    bool commitSquashWire = false;
    bool robSquashingWire = false;

    /** Total order for use as a sparse-map key (lossless audit table). */
    bool operator<(const DispatchCycleInputs &o) const;
};

/** Which policy rule classified each additive term (contract section 1.5b). */
enum class TermRule : uint8_t
{
    None = 0,
    BreakReasonMap,      // break term via section 1.5d table
    BreakUnsignaled,     // break term with NoStall reason -> Unknown
    PoppedBEAdmission,   // popped term: admission Backend family
    PoppedSquashEvidence,// popped term: squash scene evidence
    PoppedUnknown,       // popped term: neither
    P1Drain,             // upstream term: drain mode
    P2AdmissionBE,       // upstream term: admission Backend family
    P3AdmissionBS,       // upstream term: admission BS family
    P4UniqueChain,       // upstream term: unique lagged-chain cause
    P4MixedHop,          // upstream term: single non-silent hop but that
                         // hop is same-hop heterogeneous (mixed) -> Unknown
    AdmissionUnclassified,// upstream term: admission block present but the
                         // reason is neither Backend- nor BS-family
                         // (unclassified evidence) -> Unknown; the consumer
                         // provably refused, so the silent-chain-ready
                         // premise of P5 does NOT hold
    P5SilentFE,          // upstream term: silent chain + ready admission
    P6Ambiguous          // upstream term: ambiguous/contradictory chain
};

/** Result of classifyDispatchCycle: the W_C4-slot partition of one cycle. */
struct SlotSplit
{
    /** Unused slots per class (crossings are tracked separately). */
    unsigned counts[static_cast<size_t>(SlotClass::NumSlotClasses)] = {};
    /** Slots that crossed C4 (Allocated). */
    unsigned crossings = 0;
    /** Additive term sizes (contract section 1.5 identity). */
    unsigned breakBlockedSlots = 0;
    unsigned poppedSquashedSlots = 0;
    unsigned upstreamShortSlots = 0;
    /** Policy rule that classified each term. */
    TermRule breakRule = TermRule::None;
    TermRule poppedRule = TermRule::None;
    TermRule upstreamRule = TermRule::None;
    /** INV-0/0b input guards failed (delivered > budget etc.). */
    bool budgetViolation = false;

    unsigned unusedTotal() const;
    /**
     * INV-4 bookkeeping assertion: the classes plus crossings partition
     * the budget exactly and every count is non-negative.
     */
    bool partitions(unsigned budget) const;
};

/**
 * Chain-hop mixed sentinel (F4 contract pin).  A same-hop vector whose
 * non-NoStall entries carry two or more distinct reasons is folded to
 * this out-of-table value, NOT to its first non-NoStall entry: the
 * frozen ambiguity policy (P6) applies to same-hop heterogeneity too,
 * and slot-order must never select FE/BE.  The value deliberately sits
 * outside the legal StallReason range (comm.hh 0..NumStallReasons-1)
 * and is never injected into admission-side fields (iewBlockReason /
 * renameBlockReason / breakReason producers must never emit it; the
 * only producer is foldChainHop on the three chain-hop fields).
 */
constexpr StallReason kChainMixedStall = static_cast<StallReason>(200);
static_assert(static_cast<unsigned>(kChainMixedStall) >=
              static_cast<unsigned>(StallReason::NumStallReasons),
              "mixed sentinel must stay outside the legal enum range");

/**
 * Fold one chain hop's per-slot reason vector (rename.cc:433-440
 * per-slot propagation keeps delivered NoStall slots and passes the
 * downstream per-slot reasons up) into the cycle-level evidence the
 * policy layer consumes.  Pure production helper (directly tested):
 *   - 0 distinct non-NoStall reasons  -> representative NoStall (silent hop)
 *   - exactly 1 distinct reason        -> that reason (repeats collapse)
 *   - >= 2 distinct reasons (mixed)    -> kChainMixedStall sentinel
 * `reasonsMask` keeps the distinct-reason bit set (bit r set iff reason
 * value r was present) as the lossless per-hop audit encoding: the
 * frozen policy depends only on the distinct-reason set, never on
 * multiplicity or slot order.
 */
struct ChainHopEvidence
{
    StallReason representative = StallReason::NoStall;
    uint64_t reasonsMask = 0;
};
ChainHopEvidence foldChainHop(const std::vector<StallReason> &hop);

/**
 * Section 1.5d policy table (owner-frozen D-4).  Maps a StallReason to
 * its slot class.  The same value can map differently by role:
 *   - upstreamRole=false: admission/break-side role.
 *   - upstreamRole=true:  lagged-chain role.
 * NoStall returns the NumSlotClasses sentinel ("not a classification"):
 * on the admission side NoStall means "no block" (does not participate);
 * on the chain side P4/P5 count NoStall hops separately (a silent hop is
 * never itself classified).
 *
 * Deliberate deviations registered in the contract:
 *   - SerializeStall maps to Backend (barrier occupies dispatch/backend
 *     resources; the comm.hh "F" comment anchor is overridden).
 *   - BpStall maps to Recovery ONLY as chain-role evidence (R-16: it was
 *     removed from the unconditional Recovery row).  In the admission
 *     role it maps to Unknown: an admission-side BpStall is unclassified
 *     evidence, never a P3 trigger (CORE_REVIEW F2).
 *   - OtherFragStall maps to Backend (SMT freeze is out of the supported
 *     domain; DispBW residue is backend backpressure).
 */
SlotClass classifyStallReason(StallReason reason, bool upstreamRole);

/**
 * Policy layer P1..P6 (contract section 1.5b, owner-frozen D-4):
 * classifies one dispatch cycle's unused slots.  Pure function of the
 * evidence tuple; the priority order is
 *   P1 drain > P2 admission-Backend > P3 admission-BS > P4 unique chain
 *   > P5 silent-chain-ready-admission > P6 ambiguous,
 * with the break and popped terms classified by their own sub-rules.
 */
SlotSplit classifyDispatchCycle(const DispatchCycleInputs &inputs);

/**
 * Summary projection of one cycle for the <=64-bin display histogram
 * slotPatternDist (contract section 1.5c: the histogram is a summary,
 * NOT the replay input; the lossless table is).
 * x = policy/rule mode (8 bins), y = dominant attributed class (6 bins,
 * 2 reserved).
 */
struct PatternBin
{
    unsigned x = 0;
    unsigned y = 0;
};
PatternBin patternBin(const DispatchCycleInputs &inputs, const SlotSplit &split);

/**
 * Explicit validity guard for derived ratios (contract section 1.6):
 * returns quiet NaN whenever the run state is not Valid or the
 * denominator is zero.  Production implementation of every topdownV2
 * ratio; directly unit-testable.
 */
double guardedRatio(double numerator, double denominator,
                    LedgerRunState state);

/** One aggregated row of the lossless audit table. */
struct PatternRecord
{
    uint64_t cycles = 0;
    uint64_t crossings = 0;
    uint64_t breakBlockedSlots = 0;
    uint64_t poppedSquashedSlots = 0;
    uint64_t upstreamShortSlots = 0;
    uint64_t classSlots[static_cast<size_t>(SlotClass::NumSlotClasses)] = {};
};

/**
 * The stateful ledger.  The CPU owns one instance; all pipeline hooks
 * route through CPU wrappers (cpu.cc).  Direct unit tests construct it
 * standalone.
 */
class TopdownLedger : public statistics::Group
{
  public:
    /** Explicit resource cap for the lossless tuple table. */
    static constexpr size_t kPatternTableCap = 16384;

    struct Config
    {
        /** W_C4 = renameWidth (min(decodeWidth, renameWidth) today). */
        unsigned slotWidth = 0;
        unsigned numThreads = 1;
        bool enableDispatchStage = false;
        /** Configured ROB capacity (params.numROBEntries). */
        unsigned robCapacity = 0;
        /** Supported domain requires decodeWidth == renameWidth. */
        bool upstreamWidthMatches = true;
    };

    TopdownLedger(statistics::Group *parent, const Config &cfg, bool enabled);
    ~TopdownLedger() = default;

    /** Whether accounting is on (enable_d4b_topdown_ledger). */
    bool accounting() const { return enabled_; }

    LedgerRunState runState() const;

    // ------------------------------------------------------------------
    // Core hooks (DynInst-free; the CPU wrappers pass the tag + identity).
    // ------------------------------------------------------------------

    /**
     * C4 registration (iew.cc dispatchedInsts point).  INV-1: at most one
     * registration per instruction (double registration or pointer
     * recycling is counted in allocatedIdentityViolations).  INV-6:
     * registered instructions must already be in the ROB (same-tick
     * commit-side insert precedes IEW dispatch); violations count in
     * registeredNotInROB and invalidate the ledger.  After the snapshot
     * (frozen) this only counts postSnapshotRegistrations.
     */
    void registerAlloc(LedgerTag &tag, bool inROB, const void *instId,
                       uint64_t whenTick);

    /**
     * Observation point 1: successful retirement (ROB::retireHead).
     * Settlement is idempotent: only a Live instruction settles, exactly
     * once (INV-2).  Routing by epoch stamp (contract section 1.2b(d)).
     */
    void settleRetired(LedgerTag &tag, const void *instId);

    /**
     * Observation points 2-8 (squash terminations).  Same idempotence and
     * epoch routing as settleRetired; origin feeds the squashedInIQ /
     * squashedAtCommit L2 split.
     */
    void settleSquashed(LedgerTag &tag, const void *instId,
                        SettleOrigin origin);

    /**
     * Per-cycle identity-I aggregation and INV-0/INV-4 guard execution
     * (IEW phase, after the dispatch loop; contract section 1.2b(a)).
     * No-op once frozen.
     */
    void accountCycle(const DispatchCycleInputs &inputs);

    /**
     * ROI begin / any statistics reset: epoch++, frozen cleared, working
     * state reset.  Called from resetStats() so the framework-driven
     * dump+reset at the ROI boundary opens the new segment; the stat
     * storages themselves are reset by the default Group::resetStats().
     */
    void beginEpoch();

    /**
     * ROI end, Option B end-of-tick anchor (cpu.cc ROI-end trace-event
     * block, curTick() == btbpRoiEndTick, exactly once).  Freezes the
     * live group; late settlements only feed the dedicated diagnostics.
     */
    void snapshot(uint64_t whenTick);

    /** Statistics-reset hook: drives beginEpoch. */
    void resetStats() override;

    /**
     * Persistent lossless-tuple audit export (contract section 1.5c,
     * validation INTERFACE.md section 4, format d4b-audit-export-v1).
     * When a non-empty path is configured on a COLLECTING (enabled)
     * ledger, every statistics dump rewrites the file atomically
     * (tmp + rename, no per-cycle I/O) with: header (schema_version /
     * impl_schema_version / policy_version / replay_complete /
     * dropped_patterns / resource cap), one entry per exported epoch,
     * and per epoch the full distinct-tuple table with 17-signal inputs
     * + per-class slot weights + crossings.  I/O failure is fatal
     * (fail-fast: a run whose audit cannot be produced must not exit
     * looking successful).  An empty path (default) exports nothing.
     */
    void setAuditExportPath(const std::string &path);
    bool exportAudit() const;

    /** Exported schema/policy identity (F6: version metadata). */
    static constexpr const char *kAuditSchemaVersion =
        "d4b-audit-export-v1";
    static constexpr const char *kAuditImplSchemaVersion =
        "d4b-audit-export-v1.1-hopmultiset";
    static constexpr const char *kAuditPolicyVersion = "d4b-policy-v4";

    /** Read-only access for replay validation (contract section 1.5c). */
    const std::map<DispatchCycleInputs, PatternRecord> &
    patternTable() const { return patternTable_; }
    bool replayComplete() const { return replayComplete_; }

    // Current epoch stamp (for diagnostics/tests).
    uint32_t currentEpoch() const { return currentEpoch_; }
    bool frozen() const { return frozen_; }

    /** Frozen snapshot group (public: d4bDiag bridge formulas and the
     * direct-linked tests read it). */
    struct SnapshotStats : public statistics::Group
    {
        SnapshotStats(statistics::Group *parent);

        statistics::Scalar epoch;
        statistics::Scalar allocated;
        statistics::Scalar retiredCohort;
        statistics::Scalar squashedCohort;
        statistics::Scalar liveAtClose;
        statistics::Scalar liveCapacityBound;
        statistics::Scalar slotBudget;
        statistics::Scalar feSlots;
        statistics::Scalar beSlots;
        statistics::Scalar recoverySlots;
        statistics::Scalar drainSlots;
        statistics::Scalar unknownSlots;
    } snapshotStats;

  private:
    /** Common settlement routing; returns true if a Live member settled. */
    bool settle(LedgerTag &tag, const void *instId, bool retired,
                SettleOrigin origin);

    /** Recompute the guarded ratios + sumCheck from the current counters.
     *  Called at every accountCycle, at snapshot, at resetStats and on
     *  state flips: all six read the same counter snapshot, so sumCheck
     *  is exactly 1 whenever identities I+II hold at that snapshot. */
    void refreshRatios();

    /** One-shot advisory warnings (dump callback / snapshot). */
    void checkAdvisories();
    bool warnedSumCheck_ = false;
    bool warnedUnknown_ = false;

    /** Aggregate one classified cycle into stats + audit table. */
    void recordCycle(const DispatchCycleInputs &inputs, const SlotSplit &split);

    /** Re-derive run state after a guard violation. */
    void invalidate();

    void setLedgerStateStat();

    // -- configuration --------------------------------------------------
    const Config cfg_;
    const bool enabled_;

    // -- run state ------------------------------------------------------
    bool unsupportedConfig_ = false;
    bool invalidated_ = false;
    uint32_t currentEpoch_ = 0;

    // -- segment state --------------------------------------------------
    bool frozen_ = false;
    uint32_t snapshotEpoch_ = 0;
    /** Live members: instruction pointer -> (epoch, registration tick). */
    std::map<const void *, std::pair<uint32_t, uint64_t>> liveMap_;
    /** Registrations since the last accountCycle (cross-check). */
    unsigned registrationsSinceAccount_ = 0;

    // -- lossless audit table --------------------------------------------
    std::map<DispatchCycleInputs, PatternRecord> patternTable_;
    bool replayComplete_ = true;

    // -- persistent audit export (d4b-audit-export-v1) --------------------
    /** Configured output path ("" = no export; default). */
    std::string auditExportPath_;
    /** Serialized per-epoch pattern arrays, frozen at each dump so the
     *  begin-epoch reset cannot lose already-dumped segments.  Mutable
     *  export cache: exportAudit() runs from the (const) dump callback. */
    mutable std::map<uint32_t, std::string> exportedEpochs_;
    /** Patterns dropped after the resource cap, per epoch (replay
     *  completeness must survive the segment reset too). */
    mutable std::map<uint32_t, uint64_t> exportedEpochDropped_;
    /** One-shot fatal guard: the first export failure aborts the run
     *  (fail-fast), so no repeat-warning noise is needed. */
    mutable bool exportFailed_ = false;

  protected:
    // (stat storages are public: the CPU-side d4bDiag bridge formulas in
    // cpu.cc reference them cross-group, mirroring the cpu.cc:553-577
    // precedent of referencing iew/fetch/commit stats)
  public:
    /** Live-group counters (all reset by the framework at beginEpoch). */
    statistics::Scalar statEnabled;
    statistics::Scalar ledgerState;
    statistics::Scalar slotWidth;
    statistics::Scalar slotBudget;
    statistics::Scalar allocated;
    statistics::Scalar retiredCohort;
    statistics::Scalar squashedCohort;
    statistics::Scalar liveAtClose;
    statistics::Scalar squashedInIQ;
    statistics::Scalar squashedAtCommit;
    statistics::Scalar feSlots;
    statistics::Scalar beSlots;
    statistics::Scalar recoverySlots;
    statistics::Scalar drainSlots;
    statistics::Scalar unknownSlots;
    statistics::Vector2d slotPatternDist;

    statistics::Scalar settledAfterCloseRetired;
    statistics::Scalar settledAfterCloseSquashed;
    statistics::Scalar settledOutOfEpochRetired;
    statistics::Scalar settledOutOfEpochSquashed;
    statistics::Scalar postSnapshotRegistrations;

    statistics::Scalar budgetOverrun;
    statistics::Scalar budgetOverrunSlots;
    statistics::Scalar registeredNotInROB;
    statistics::Scalar liveOverCapacity;
    statistics::Scalar identityViolations;
    statistics::Scalar allocatedIdentityViolations;
    statistics::Scalar configUnsupportedSMT;
    statistics::Scalar configUnsupportedDispatchStage;
    statistics::Scalar configUnsupportedWidth;
    statistics::Scalar liveAgeMax;

    statistics::Scalar replayCompleteStat;
    statistics::Scalar replayDistinctTuples;
    statistics::Scalar replayTableOverflowTuples;

    // ---- derived L1 ratios (contract section 1.3; explicit validity
    // guard: unsupported / zero-budget -> quiet NaN, never 0 or inf) ----
    statistics::Scalar retiring;
    statistics::Scalar badSpec;
    statistics::Scalar frontend;
    statistics::Scalar backend;
    statistics::Scalar boundary;
    statistics::Scalar unknown;
    /** Representation invariant: sum of the six ratios (1 when valid;
    NOT a scientific acceptance criterion — contract section 1.0). */
    statistics::Scalar sumCheck;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TOPDOWN_LEDGER_HH__
