// D4-B topdown ledger unit tests (direct-linked production code).
//
// Build (NULL ISA):
//   scons build/NULL/cpu/o3/topdown_ledger.test.opt --unit-test
// Run:
//   ./build/NULL/cpu/o3/topdown_ledger.test.opt
//
// Design contract: btb_prefetch_paper/.local/
//   btb-morphology-spike-20260910/d4b-design/D4B_DESIGN_CONTRACT_DRAFT.md (v4)
//
// These tests link the PRODUCTION translation unit (topdown_ledger.cc) —
// there is no inline duplicated model (contract section 5a explicitly
// rejects duplicated-logic tests as correctness evidence).  Every
// expectation below cites the contract section it pins.
//
// NOTE on the P1-P6 classification policy: the priorities are OWNER-FROZEN
// policy (D-4), not causal truth; these tests pin "classification follows
// the frozen policy", nothing more (contract section 1.0).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "cpu/o3/comm.hh"
#include "cpu/o3/topdown_ledger.hh"

using gem5::o3::classifyDispatchCycle;
using gem5::o3::classifyStallReason;
using gem5::o3::DispatchCycleInputs;
using gem5::o3::foldChainHop;
using gem5::o3::guardedRatio;
using gem5::o3::LedgerRunState;
using gem5::o3::LedgerState;
using gem5::o3::LedgerTag;
using gem5::o3::patternBin;
using gem5::o3::PatternBin;
using gem5::o3::SettleOrigin;
using gem5::o3::SlotClass;
using gem5::o3::SlotSplit;
using gem5::o3::StallReason;
using gem5::o3::TermRule;
using gem5::o3::TopdownLedger;

namespace
{

constexpr unsigned kW = 8;

DispatchCycleInputs
baseInputs()
{
    // A fully-utilised cycle: 8 arrive, 8 cross, nothing unused.
    DispatchCycleInputs in;
    in.budget = kW;
    in.residueAtStart = 0;
    in.arrivals = kW;
    in.crossings = kW;
    in.poppedSquashed = 0;
    return in;
}

TopdownLedger::Config
baseConfig()
{
    TopdownLedger::Config cfg;
    cfg.slotWidth = kW;
    cfg.numThreads = 1;
    cfg.enableDispatchStage = false;
    cfg.robCapacity = 352;
    cfg.upstreamWidthMatches = true;
    return cfg;
}

// Convenience: total slots attributed to a class by a split.
unsigned
cls(const SlotSplit &s, SlotClass c)
{
    return s.counts[static_cast<size_t>(c)];
}

} // anonymous namespace

// ===========================================================================
// Section 1.5d mapping table (owner-frozen D-4).  Every StallReason value
// is pinned, including the deliberate deviations (SerializeStall=Backend,
// OtherFragStall=Backend, BpStall not unconditional).
// ===========================================================================

TEST(ClassifyStallReason, FrontendRow)
{
    EXPECT_EQ(classifyStallReason(StallReason::IcacheStall, false),
              SlotClass::Frontend);
    EXPECT_EQ(classifyStallReason(StallReason::ITlbStall, true),
              SlotClass::Frontend);
    EXPECT_EQ(classifyStallReason(StallReason::IntStall, false),
              SlotClass::Frontend);
    EXPECT_EQ(classifyStallReason(StallReason::TrapStall, true),
              SlotClass::Frontend);
    EXPECT_EQ(classifyStallReason(StallReason::FTQBubble, false),
              SlotClass::Frontend);
    EXPECT_EQ(classifyStallReason(StallReason::FetchFragStall, true),
              SlotClass::Frontend);
    EXPECT_EQ(classifyStallReason(StallReason::OtherFetchStall, false),
              SlotClass::Frontend);
}

TEST(ClassifyStallReason, BackendRowIncludingDeliberateDeviations)
{
    const StallReason backend[] = {
        StallReason::DTlbStall,
        StallReason::SerializeStall,      // deliberate: Backend, not "F"
        StallReason::ScalarLongExecute,
        StallReason::VectorLongExecute,
        StallReason::InstNotReady,
        StallReason::LoadL1Bound,
        StallReason::LoadL2Bound,
        StallReason::LoadL3Bound,
        StallReason::LoadMemBound,
        StallReason::StoreL1Bound,
        StallReason::StoreL2Bound,
        StallReason::StoreL3Bound,
        StallReason::StoreMemBound,
        StallReason::MemNotReady,
        StallReason::MemCommitRateLimit,
        StallReason::Atomic,
        StallReason::OtherMemStall,
        StallReason::MemDQBandwidth,
        StallReason::IntDQBandwidth,
        StallReason::FVDQBandwidth,
        StallReason::VectorReadyButNotIssued,
        StallReason::ScalarReadyButNotIssued,
        StallReason::ResumeUnblock,
        StallReason::ROBFull,
        StallReason::RegFull,
        StallReason::OtherStall,
        StallReason::OtherFragStall,      // deliberate: Backend (DispBW residue)
    };
    for (StallReason r : backend) {
        EXPECT_EQ(classifyStallReason(r, false), SlotClass::Backend)
            << "reason " << static_cast<int>(r);
        EXPECT_EQ(classifyStallReason(r, true), SlotClass::Backend)
            << "reason " << static_cast<int>(r);
    }
}

TEST(ClassifyStallReason, RecoveryRow)
{
    // BpStall is chain-role ONLY (R-16): admission role maps to Unknown.
    const StallReason recovery[] = {
        StallReason::SquashStall,
        StallReason::InstMisPred,
        StallReason::InstSquashed,
        StallReason::CommitSquash,
    };
    for (StallReason r : recovery) {
        EXPECT_EQ(classifyStallReason(r, true), SlotClass::Recovery);
        EXPECT_EQ(classifyStallReason(r, false), SlotClass::Recovery);
    }
    EXPECT_EQ(classifyStallReason(StallReason::BpStall, true),
              SlotClass::Recovery);   // chain-role evidence
    EXPECT_EQ(classifyStallReason(StallReason::BpStall, false),
              SlotClass::Unknown);     // admission role: unclassified (F2)
}

TEST(ClassifyStallReason, UnknownRowAndNoStallSentinel)
{
    EXPECT_EQ(classifyStallReason(StallReason::FetchBufferInvalid, true),
              SlotClass::Unknown);
    EXPECT_EQ(classifyStallReason(StallReason::MemSquashed, false),
              SlotClass::Unknown);
    // Out-of-table future value falls to Unknown via the default arm.
    EXPECT_EQ(classifyStallReason(static_cast<StallReason>(200), true),
              SlotClass::Unknown);
    // NoStall is "not a classification" in both roles (section 1.5d
    // role-special-case); callers handle it before mapping.
    EXPECT_EQ(classifyStallReason(StallReason::NoStall, false),
              SlotClass::NumSlotClasses);
    EXPECT_EQ(classifyStallReason(StallReason::NoStall, true),
              SlotClass::NumSlotClasses);
}

// ===========================================================================
// classifyDispatchCycle: additive identity + INV-4 partition + P1..P6
// ===========================================================================

TEST(ClassifyDispatch, FullWidthCyclePartitionsExactly)
{
    const DispatchCycleInputs in = baseInputs();
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_FALSE(s.budgetViolation);
    EXPECT_TRUE(s.partitions(kW));
    EXPECT_EQ(s.unusedTotal(), 0u);
    EXPECT_EQ(s.crossings, kW);
    EXPECT_EQ(cls(s, SlotClass::Frontend), 0u);
    EXPECT_EQ(cls(s, SlotClass::Backend), 0u);
    EXPECT_EQ(cls(s, SlotClass::Recovery), 0u);
    EXPECT_EQ(cls(s, SlotClass::Drain), 0u);
    EXPECT_EQ(cls(s, SlotClass::Unknown), 0u);
    EXPECT_EQ(s.breakRule, TermRule::None);
    EXPECT_EQ(s.upstreamRule, TermRule::None);
}

TEST(ClassifyDispatch, AdditiveTermIdentity)
{
    // residue 2 + arrivals 5 = delivered 7; crossings 4, popped 1 ->
    // breakBlocked 2, upstreamShort 1.  (Partial delivery with a break.)
    DispatchCycleInputs in = baseInputs();
    in.residueAtStart = 2;
    in.arrivals = 5;
    in.crossings = 4;
    in.poppedSquashed = 1;
    in.breakReason = StallReason::ROBFull;
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_FALSE(s.budgetViolation);
    EXPECT_TRUE(s.partitions(kW));
    EXPECT_EQ(s.breakBlockedSlots, 2u);
    EXPECT_EQ(s.poppedSquashedSlots, 1u);
    EXPECT_EQ(s.upstreamShortSlots, 1u);
    EXPECT_EQ(s.unusedTotal(), 4u);
    EXPECT_EQ(s.crossings, 4u);
}

TEST(ClassifyDispatch, BudgetGuardsFlagViolations)
{
    // delivered (residue+arrivals) exceeds the budget -> INV-0 violation.
    DispatchCycleInputs in = baseInputs();
    in.residueAtStart = 4;
    in.arrivals = 6;      // delivered 10 > 8
    in.crossings = 6;
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_TRUE(s.budgetViolation);

    // crossings exceed delivered -> INV-0b violation.
    DispatchCycleInputs in2 = baseInputs();
    in2.arrivals = 3;
    in2.crossings = 4;
    const SlotSplit s2 = classifyDispatchCycle(in2);
    EXPECT_TRUE(s2.budgetViolation);
}

// --- S19 (contract section 1.5e): FE starvation + BE admission overlap ---
// W=8, residue=0, arrivals=3, crossings=2, popped=0, break@IQFull with
// residue 1 left; blockRename=true/OtherFragStall (residue backpressure),
// blockIEW=false; chain fetchUp=IcacheStall, robSquashingWire=1.
// Frozen expectation: BE=6, FE=0, Recovery=0, allocated=2.
TEST(ClassifyDispatch, S19FEStarvationWithBEAdmissionOverlap)
{
    DispatchCycleInputs in = baseInputs();
    in.residueAtStart = 0;
    in.arrivals = 3;
    in.crossings = 2;
    in.poppedSquashed = 0;
    in.breakReason = StallReason::InstNotReady;   // IQ-full break, Backend
    in.blockRename = true;
    in.renameBlockReason = StallReason::OtherFragStall;
    in.blockIEW = false;
    in.iewBlockReason = StallReason::NoStall;
    in.renameUp = StallReason::NoStall;
    in.decodeUp = StallReason::NoStall;
    in.fetchUp = StallReason::IcacheStall;
    in.commitSquashWire = false;
    in.robSquashingWire = true;

    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_FALSE(s.budgetViolation);
    EXPECT_TRUE(s.partitions(kW));
    // breakBlocked = 3-2-0 = 1 -> Backend (section 1.5d of the break
    // reason); upstreamShort = 8-3 = 5 -> P2 (admission Backend family:
    // blockRename with a non-BS reason) -> Backend.
    EXPECT_EQ(cls(s, SlotClass::Backend), 6u);
    EXPECT_EQ(cls(s, SlotClass::Frontend), 0u);
    EXPECT_EQ(cls(s, SlotClass::Recovery), 0u);
    EXPECT_EQ(cls(s, SlotClass::Unknown), 0u);
    EXPECT_EQ(s.crossings, 2u);
    EXPECT_EQ(s.upstreamRule, TermRule::P2AdmissionBE);
    EXPECT_EQ(s.breakRule, TermRule::BreakReasonMap);
}

// --- S20: pure recovery + supply suppression --------------------------
// ROBSquashing walk: blockIEW=true/CommitSquash, arrivals=0, delivered=0,
// chain renameUp=BpStall -> Recovery += upstreamShort=8 via P3 (the chain
// BpStall is corroborating evidence only, not the basis).
TEST(ClassifyDispatch, S20PureRecoverySupplySuppression)
{
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 0;
    in.crossings = 0;
    in.blockIEW = true;
    in.iewBlockReason = StallReason::CommitSquash;
    in.blockRename = true;
    in.renameBlockReason = StallReason::CommitSquash;
    in.renameUp = StallReason::BpStall;
    in.decodeUp = StallReason::NoStall;
    in.fetchUp = StallReason::NoStall;

    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_TRUE(s.partitions(kW));
    EXPECT_EQ(cls(s, SlotClass::Recovery), 8u);
    EXPECT_EQ(cls(s, SlotClass::Backend), 0u);
    EXPECT_EQ(cls(s, SlotClass::Frontend), 0u);
    EXPECT_EQ(s.upstreamRule, TermRule::P3AdmissionBS);
}

// --- S20b: recovery scene + Backend admission block (P2 outranks P3) ---
TEST(ClassifyDispatch, S20bBackendAdmissionOutranksRecovery)
{
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 0;
    in.crossings = 0;
    // Squash scene is present, but the admission block itself is Backend
    // family (blockRename from LQ/SQ full, blockIEW false).
    in.blockIEW = false;
    in.iewBlockReason = StallReason::NoStall;
    in.blockRename = true;
    in.renameBlockReason = StallReason::ROBFull;
    in.robSquashingWire = true;    // squash scene evidence present
    in.renameUp = StallReason::BpStall;

    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_TRUE(s.partitions(kW));
    EXPECT_EQ(cls(s, SlotClass::Backend), 8u);
    EXPECT_EQ(cls(s, SlotClass::Recovery), 0u);
    EXPECT_EQ(s.upstreamRule, TermRule::P2AdmissionBE);
}

// --- S21: lagged-chain ambiguity -> Unknown (P6) -----------------------
TEST(ClassifyDispatch, S21AmbiguousChainGoesUnknown)
{
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 0;
    in.crossings = 0;
    in.renameUp = StallReason::ROBFull;        // B
    in.decodeUp = StallReason::NoStall;
    in.fetchUp = StallReason::IcacheStall;     // F  (multi-hop mixed)
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
    EXPECT_EQ(s.upstreamRule, TermRule::P6Ambiguous);
    // F7 (CORE_REVIEW): S21 must also pin the slotPatternDist bucket
    // (contract section 1.5e: "Unknown += 8 (P6) + bucket chain=mixed").
    const PatternBin bin = patternBin(in, s);
    EXPECT_EQ(bin.x, 6u);
    EXPECT_EQ(bin.y, static_cast<unsigned>(SlotClass::Unknown));
}

// --- S21b: silent chain + ready admission -> Frontend (P5, frozen) -----
TEST(ClassifyDispatch, S21bSilentChainReadyAdmissionIsFrontend)
{
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 0;
    in.crossings = 0;
    in.blockIEW = false;
    in.blockRename = false;
    in.renameUp = StallReason::NoStall;
    in.decodeUp = StallReason::NoStall;
    in.fetchUp = StallReason::NoStall;
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_EQ(cls(s, SlotClass::Frontend), 8u);
    EXPECT_EQ(s.upstreamRule, TermRule::P5SilentFE);
}

// --- P1 drain outranks everything ---------------------------------------
TEST(ClassifyDispatch, P1DrainOutranksAdmissionAndChain)
{
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 0;
    in.crossings = 0;
    in.drainMode = true;
    in.blockIEW = true;
    in.iewBlockReason = StallReason::CommitSquash;   // would be P3
    in.fetchUp = StallReason::IcacheStall;           // would be P4
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_EQ(cls(s, SlotClass::Drain), 8u);
    EXPECT_EQ(s.upstreamRule, TermRule::P1Drain);
}

// --- P4 unique chain mapping (F/B/BS/Unknown per table) -----------------
TEST(ClassifyDispatch, P4UniqueChainMapsPerTable)
{
    // Unique frontend cause.
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.fetchUp = StallReason::IcacheStall;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Frontend), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::P4UniqueChain);
    }
    // Unique backend cause.
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.renameUp = StallReason::ROBFull;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Backend), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::P4UniqueChain);
    }
    // Unique bad-speculation cause (BpStall chain evidence -> Recovery).
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.fetchUp = StallReason::BpStall;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Recovery), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::P4UniqueChain);
    }
    // Unique out-of-table cause -> Unknown through P4.
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.decodeUp = StallReason::FetchBufferInvalid;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::P4UniqueChain);
    }
}

// --- BpStall is NOT unconditional Recovery (v3 change) ------------------
TEST(ClassifyDispatch, BpStallNotUnconditionallyRecovery)
{
    // Two non-silent hops, one of them BpStall: ambiguity -> Unknown.
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 0;
    in.crossings = 0;
    in.fetchUp = StallReason::BpStall;
    in.decodeUp = StallReason::IcacheStall;
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
    EXPECT_EQ(s.upstreamRule, TermRule::P6Ambiguous);
}

// ===========================================================================
// F2 + F4 linkage (CORE_REVIEW F2; main-session directive):
//   admission-role BpStall / out-of-table sentinels / unsampled NoStall
//   reasons are UNCLASSIFIED admission evidence -> Unknown (never P2 via
//   a != Recovery predicate, never P3, never P5), while legitimate
//   Backend-family and CommitSquash admission blocks keep P2/P3 and the
//   P2-over-P3 priority is untouched.
// ===========================================================================

TEST(ClassifyDispatch, AdmissionBpStallIsUnclassifiedUnknown)
{
    // blockIEW + BpStall: NOT P3 (admission-role BpStall is not BS-family
    // evidence per R-16) and NOT P2 -- unclassified admission -> Unknown.
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.blockIEW = true;
        in.iewBlockReason = StallReason::BpStall;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
        EXPECT_EQ(cls(s, SlotClass::Recovery), 0u);
        EXPECT_EQ(cls(s, SlotClass::Backend), 0u);
        EXPECT_EQ(s.upstreamRule, TermRule::AdmissionUnclassified);
    }
    // blockRename + BpStall: the OLD != Recovery predicate would have
    // sent this to P2 Backend after the BpStall role split; the tightened
    // == Backend predicate must not (order-independence of the linkage).
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.blockRename = true;
        in.renameBlockReason = StallReason::BpStall;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
        EXPECT_EQ(cls(s, SlotClass::Backend), 0u);
        EXPECT_EQ(s.upstreamRule, TermRule::AdmissionUnclassified);
    }
}

TEST(ClassifyDispatch, AdmissionUnknownSentinelAndUnsampledReasons)
{
    // Out-of-table sentinel in an admission field: unclassified -> Unknown
    // (the F4 sentinel-forbidden-in-admission-fields discipline makes this
    // unreachable from production wiring; the policy must still be safe).
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.blockIEW = true;
        in.iewBlockReason = static_cast<StallReason>(200);
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::AdmissionUnclassified);
    }
    // Block present but the reason was not sampled (NoStall): the old
    // predicate read NoStall's sentinel as "!= Recovery" -> P2 Backend.
    // The consumer provably refused, but there is no Backend-family
    // evidence: Unknown, and P5 silent-ready must NOT fire either.
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.blockIEW = true;
        in.iewBlockReason = StallReason::NoStall;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
        EXPECT_EQ(cls(s, SlotClass::Frontend), 0u);
        EXPECT_EQ(s.upstreamRule, TermRule::AdmissionUnclassified);
    }
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.blockRename = true;
        in.renameBlockReason = StallReason::NoStall;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::AdmissionUnclassified);
    }
}

TEST(ClassifyDispatch, LegitimateAdmissionFamiliesKeepP2P3Priority)
{
    // P2 preserved: Backend-family admission block (both arms).
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.blockIEW = true;
        in.iewBlockReason = StallReason::ROBFull;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Backend), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::P2AdmissionBE);
    }
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.blockRename = true;
        in.renameBlockReason = StallReason::OtherFragStall;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Backend), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::P2AdmissionBE);
    }
    // P3 preserved: CommitSquash admission block.
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.blockIEW = true;
        in.iewBlockReason = StallReason::CommitSquash;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Recovery), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::P3AdmissionBS);
    }
    // P2 outranks P3 even when squash-scene evidence is present (S20b).
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.blockIEW = true;
        in.iewBlockReason = StallReason::ROBFull;
        in.robSquashingWire = true;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Backend), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::P2AdmissionBE);
    }
}

// ===========================================================================
// F4 chain-hop folding (foldChainHop): per-slot vectors keep delivered
// NoStall slots and propagate per-slot causes (rename.cc:433-440).  A
// same-hop heterogeneous vector folds to the kChainMixedStall sentinel —
// slot order never selects the cause — and single-reason repeats collapse
// to that reason (F3 stays frozen: multi-hop SAME-reason is still P6
// ambiguity, only same-hop mixing is new evidence).
// ===========================================================================

TEST(FoldChainHop, OrderInvarianceAndMixedSentinel)
{
    using gem5::o3::kChainMixedStall;
    const std::vector<StallReason> fwd = {
        StallReason::IcacheStall, StallReason::ROBFull};
    const std::vector<StallReason> rev = {
        StallReason::ROBFull, StallReason::IcacheStall};
    const auto f = foldChainHop(fwd);
    const auto r = foldChainHop(rev);
    EXPECT_EQ(f.representative, kChainMixedStall);
    EXPECT_EQ(r.representative, kChainMixedStall);
    EXPECT_EQ(f.reasonsMask, r.reasonsMask);
    // [Icache, Icache] collapses to the sole reason (repeat = same reason,
    // not ambiguity).
    const auto rep = foldChainHop({StallReason::IcacheStall,
                                   StallReason::IcacheStall});
    EXPECT_EQ(rep.representative, StallReason::IcacheStall);
    EXPECT_EQ(rep.reasonsMask, 1ULL << static_cast<unsigned>(
                                   StallReason::IcacheStall));
    // Silent and empty hops.
    EXPECT_EQ(foldChainHop({}).representative, StallReason::NoStall);
    EXPECT_EQ(foldChainHop({StallReason::NoStall,
                            StallReason::NoStall}).representative,
              StallReason::NoStall);
    EXPECT_EQ(foldChainHop({}).reasonsMask, 0u);
}

TEST(FoldChainHop, DeliveredNoStallSlotsAreNotEvidence)
{
    // rename.cc:433-440 shape: delivered slots carry NoStall, the tail
    // keeps the upstream per-slot reasons.  Only distinct causes matter.
    const std::vector<StallReason> hop = {
        StallReason::NoStall, StallReason::NoStall,
        StallReason::IcacheStall, StallReason::NoStall,
        StallReason::IcacheStall};
    const auto e = foldChainHop(hop);
    EXPECT_EQ(e.representative, StallReason::IcacheStall);
    // One NoStall + one non-NoStall = sole cause (a NoStall slot is
    // delivered-slot padding, not a second reason).
    const auto pad = foldChainHop({StallReason::NoStall,
                                   StallReason::ROBFull});
    EXPECT_EQ(pad.representative, StallReason::ROBFull);
}

TEST(ClassifyDispatch, MixedHopFoldsToUnknownEvenAsSoleHop)
{
    using gem5::o3::kChainMixedStall;
    // Same-hop heterogeneous evidence with admission provably ready and
    // every other hop silent: the frozen ambiguity policy applies — the
    // first non-NoStall entry (IcacheStall = Frontend) must NOT be picked.
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 0;
    in.crossings = 0;
    in.fetchUp = kChainMixedStall;
    in.fetchUpReasons = (1ULL << static_cast<unsigned>(
                             StallReason::IcacheStall)) |
                        (1ULL << static_cast<unsigned>(StallReason::ROBFull));
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
    EXPECT_EQ(cls(s, SlotClass::Frontend), 0u);
    EXPECT_EQ(cls(s, SlotClass::Backend), 0u);
    EXPECT_EQ(s.upstreamRule, TermRule::P4MixedHop);
    const PatternBin bin = patternBin(in, s);
    EXPECT_EQ(bin.x, 6u);
}

TEST(ClassifyDispatch, MixedHopDominatesOtherHopReasons)
{
    using gem5::o3::kChainMixedStall;
    // A mixed hop plus another non-silent hop is still ambiguity; the
    // mixed sentinel is detected first so the recorded rule names the
    // same-hop heterogeneity precisely (destination is Unknown either way).
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 0;
    in.crossings = 0;
    in.fetchUp = kChainMixedStall;
    in.renameUp = StallReason::ROBFull;
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
    EXPECT_EQ(s.upstreamRule, TermRule::P4MixedHop);
}

TEST(ClassifyDispatch, ChainPathOutOfTableValueIsUnknown)
{
    // F4 contract pin: an out-of-table value (NOT the kChainMixedStall
    // sentinel — a generic unclassified value, e.g. 150) in a CHAIN field
    // falls to Unknown through both the sole-hop (P4) and multi-hop (P6)
    // routes — the default arm is an explicit contract, not an accident.
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.fetchUp = static_cast<StallReason>(150);
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::P4UniqueChain);
    }
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.fetchUp = static_cast<StallReason>(150);
        in.renameUp = StallReason::IcacheStall;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Unknown), 8u);
        EXPECT_EQ(s.upstreamRule, TermRule::P6Ambiguous);
    }
}

// --- poppedSquashed term rules ------------------------------------------
TEST(ClassifyDispatch, PoppedTermAdmissionAndEvidenceRules)
{
    // Admission Backend family -> Backend.
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 2;
        in.crossings = 0;
        in.poppedSquashed = 2;
        in.blockRename = true;
        in.renameBlockReason = StallReason::ROBFull;
        const SlotSplit s = classifyDispatchCycle(in);
        // breakBlocked = 2-0-2 = 0; popped 2 -> Backend; upstreamShort 6 ->
        // P2 Backend.
        EXPECT_EQ(cls(s, SlotClass::Backend), 8u);
        EXPECT_EQ(s.poppedRule, TermRule::PoppedBEAdmission);
    }
    // Squash scene evidence (wire only) -> Recovery.
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 2;
        in.crossings = 0;
        in.poppedSquashed = 2;
        in.robSquashingWire = true;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Recovery), 2u);
        EXPECT_EQ(s.poppedRule, TermRule::PoppedSquashEvidence);
        EXPECT_EQ(s.upstreamRule, TermRule::P5SilentFE);
        EXPECT_EQ(cls(s, SlotClass::Frontend), 6u);
    }
    // Neither -> Unknown.
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 2;
        in.crossings = 0;
        in.poppedSquashed = 2;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_EQ(cls(s, SlotClass::Unknown), 2u);
        EXPECT_EQ(s.poppedRule, TermRule::PoppedUnknown);
    }
}

// --- break term with unsignaled reason -> Unknown (defensive) ----------
TEST(ClassifyDispatch, BreakUnsignaledGoesUnknown)
{
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 3;
    in.crossings = 2;   // breakBlocked=1 but breakReason NoStall
    in.breakReason = StallReason::NoStall;
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_EQ(s.breakBlockedSlots, 1u);
    EXPECT_EQ(cls(s, SlotClass::Unknown), 1u);
    EXPECT_EQ(s.breakRule, TermRule::BreakUnsignaled);
}

// --- zero-capacity cycle (S12-family): budget 0 -------------------------
TEST(ClassifyDispatch, ZeroBudgetCyclePartitionsZero)
{
    DispatchCycleInputs in;
    in.budget = 0;
    const SlotSplit s = classifyDispatchCycle(in);
    EXPECT_FALSE(s.budgetViolation);
    EXPECT_TRUE(s.partitions(0));
    EXPECT_EQ(s.unusedTotal(), 0u);
}

// --- width variants (S10-family): partitions hold at other W -----------
TEST(ClassifyDispatch, WidthVariantsPartition)
{
    for (unsigned w : {1u, 2u, 4u, 6u, 16u}) {
        DispatchCycleInputs in;
        in.budget = w;
        in.arrivals = w / 2;
        in.crossings = w / 4;
        in.breakReason = StallReason::RegFull;
        const SlotSplit s = classifyDispatchCycle(in);
        EXPECT_FALSE(s.budgetViolation) << "w=" << w;
        EXPECT_TRUE(s.partitions(w)) << "w=" << w;
    }
}

// ===========================================================================
// Pattern bin summary (section 1.5c: the <=64-bin histogram is a summary
// only; distinct tuples must be distinguishable — see the replay tests).
// ===========================================================================

TEST(PatternBin, FullWidthAndModeBins)
{
    {
        const PatternBin b = patternBin(baseInputs(),
                                        classifyDispatchCycle(baseInputs()));
        EXPECT_EQ(b.x, 0u);
    }
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.drainMode = true;
        EXPECT_EQ(patternBin(in, classifyDispatchCycle(in)).x, 1u);
    }
    {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.blockRename = true;
        in.renameBlockReason = StallReason::ROBFull;
        EXPECT_EQ(patternBin(in, classifyDispatchCycle(in)).x, 2u);
    }
}

// ===========================================================================
// guardedRatio (section 1.6: explicit validity guard, NaN — never 0, never
// inf — for unsupported/zero-budget; S23 pure-function layer)
// ===========================================================================

TEST(GuardedRatio, ValidComputesExact)
{
    EXPECT_EQ(guardedRatio(3.0, 8.0, LedgerRunState::Valid), 0.375);
    EXPECT_EQ(guardedRatio(0.0, 8.0, LedgerRunState::Valid), 0.0);
}

TEST(GuardedRatio, ZeroBudgetIsNaNNotZeroOrInf)
{
    const double r = guardedRatio(7.0, 0.0, LedgerRunState::Valid);
    EXPECT_TRUE(std::isnan(r));
    EXPECT_FALSE(std::isinf(r));
    EXPECT_NE(r, 0.0);
}

TEST(GuardedRatio, UnsupportedAndInvalidAreNaNEvenWithNonzeroDenominator)
{
    for (LedgerRunState st :
         {LedgerRunState::Unsupported, LedgerRunState::Invalid}) {
        const double r = guardedRatio(7.0, 8.0, st);
        EXPECT_TRUE(std::isnan(r));
        EXPECT_NE(r, 0.0);
        // A nonzero numerator must never masquerade as a valid 0 ratio.
        EXPECT_FALSE(r == 0.0);
    }
}

// ===========================================================================
// Counter representation (section 1.6: Counter=double, exact integers to
// 2^53; the executable check of the formal argument).
// ===========================================================================

TEST(CounterPrecision, ExactIntegerBoundaryAt2p53)
{
    const double two53 = std::ldexp(1.0, 53);   // 9007199254740992
    double c = two53;
    c += 1.0;                                   // NOT representable
    EXPECT_EQ(c, two53);                        // rounding swallows the +1
    c = two53 - 2.0;
    c += 1.0;
    EXPECT_EQ(c, two53 - 1.0);                  // exact below the boundary
    // Realistic ledger magnitudes (~3e7) accumulate exactly.
    double acc = 0.0;
    for (int i = 0; i < 30000000; i += 1000000)
        acc += 1000000;
    EXPECT_EQ(acc, 30000000.0);
}

// ===========================================================================
// TopdownLedger state machine (sections 1.4/1.2b(d)/1.6; INV-1/2/3/5/6/7).
// The ledger is constructed standalone (no CPU): the tag-based hook API
// is the production API; cpu.cc only forwards DynInst fields into it.
// ===========================================================================

TEST(TopdownLedger, RegistrationAndINV6InROBGuard)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag tag;
    const void *id = &tag;
    ledger.registerAlloc(tag, true, id, 100);
    EXPECT_EQ(tag.state, LedgerState::Live);
    EXPECT_EQ(tag.epoch, 0u);
    EXPECT_EQ(ledger.allocated.value(), 1.0);
    EXPECT_EQ(ledger.liveAtClose.value(), 1.0);
    EXPECT_EQ(ledger.registeredNotInROB.value(), 0.0);
    EXPECT_EQ(ledger.runState(), LedgerRunState::Valid);

    // INV-6 violation: registration while not in the ROB -> guard + invalid.
    TopdownLedger bad(nullptr, baseConfig(), true);
    LedgerTag tag2;
    bad.registerAlloc(tag2, false, &tag2, 100);
    EXPECT_EQ(bad.registeredNotInROB.value(), 1.0);
    EXPECT_EQ(bad.runState(), LedgerRunState::Invalid);
    // The instruction still registers (debug values preserved).
    EXPECT_EQ(tag2.state, LedgerState::Live);
}

TEST(TopdownLedger, INV1DoubleRegistrationRejected)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag tag;
    const void *id = &tag;
    ledger.registerAlloc(tag, true, id, 100);
    ledger.registerAlloc(tag, true, id, 100);   // state != Unallocated
    EXPECT_EQ(ledger.allocatedIdentityViolations.value(), 1.0);
    EXPECT_EQ(ledger.runState(), LedgerRunState::Invalid);
    EXPECT_EQ(ledger.allocated.value(), 1.0);   // not double counted
}

TEST(TopdownLedger, SettlementIdempotenceAndL2Split)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag a, b, c;
    ledger.registerAlloc(a, true, &a, 100);
    ledger.registerAlloc(b, true, &b, 100);
    ledger.registerAlloc(c, true, &c, 100);

    // Retire one (observation point 1).
    ledger.settleRetired(a, &a);
    EXPECT_EQ(a.state, LedgerState::Retired);
    EXPECT_EQ(ledger.retiredCohort.value(), 1.0);
    EXPECT_EQ(ledger.liveAtClose.value(), 2.0);

    // Squash one from the IQ side.
    ledger.settleSquashed(b, &b, SettleOrigin::IQ);
    EXPECT_EQ(b.state, LedgerState::Squashed);
    EXPECT_EQ(ledger.squashedCohort.value(), 1.0);
    EXPECT_EQ(ledger.squashedInIQ.value(), 1.0);
    EXPECT_EQ(ledger.squashedAtCommit.value(), 0.0);

    // S16: duplicate squash notifications never double settle (the same
    // instruction is observed by several points over its drain).
    ledger.settleSquashed(b, &b, SettleOrigin::RobWalk);
    ledger.settleSquashed(b, &b, SettleOrigin::CommitBelt);
    ledger.settleRetired(b, &b);
    EXPECT_EQ(ledger.squashedCohort.value(), 1.0);
    EXPECT_EQ(ledger.retiredCohort.value(), 1.0);
    EXPECT_EQ(ledger.squashedInIQ.value(), 1.0);
    EXPECT_EQ(ledger.squashedAtCommit.value(), 0.0);

    // Commit-side belt squash for the third.
    ledger.settleSquashed(c, &c, SettleOrigin::CommitBelt);
    EXPECT_EQ(ledger.squashedAtCommit.value(), 1.0);
    EXPECT_EQ(ledger.liveAtClose.value(), 0.0);
    EXPECT_EQ(ledger.runState(), LedgerRunState::Valid);
}

TEST(TopdownLedger, EpochRoutingSettledOutOfEpoch)
{
    // INV-5: pre-segment registrations settle into the diagnostics, not
    // the segment cohort (PreIn material of identity E2 / bridge B2).
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag pre;
    ledger.registerAlloc(pre, true, &pre, 100);

    ledger.beginEpoch();      // segment boundary (resetStats path)
    EXPECT_EQ(ledger.currentEpoch(), 1u);

    LedgerTag inSeg;
    ledger.registerAlloc(inSeg, true, &inSeg, 200);
    EXPECT_EQ(inSeg.epoch, 1u);

    ledger.settleRetired(pre, &pre);
    ledger.settleSquashed(inSeg, &inSeg, SettleOrigin::LSQ);
    EXPECT_EQ(ledger.settledOutOfEpochRetired.value(), 1.0);
    EXPECT_EQ(ledger.retiredCohort.value(), 0.0);
    EXPECT_EQ(ledger.squashedCohort.value(), 1.0);
    EXPECT_EQ(ledger.squashedInIQ.value(), 1.0);
}

TEST(TopdownLedger, SnapshotFreezesAndRoutesLateSettlements)
{
    // Option B end-of-tick snapshot semantics (sections 1.2b(c)/(d)).
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag r, sq, liveA, liveB, older;
    ledger.registerAlloc(r, true, &r, 10);
    ledger.registerAlloc(sq, true, &sq, 12);
    ledger.registerAlloc(liveA, true, &liveA, 14);
    ledger.registerAlloc(liveB, true, &liveB, 16);
    ledger.settleRetired(r, &r);
    ledger.settleSquashed(sq, &sq, SettleOrigin::IQ);
    // liveA/liveB remain in flight at the close.

    ledger.snapshot(1000);
    EXPECT_TRUE(ledger.frozen());
    EXPECT_EQ(ledger.snapshotStats.allocated.value(), 4.0);
    EXPECT_EQ(ledger.snapshotStats.retiredCohort.value(), 1.0);
    EXPECT_EQ(ledger.snapshotStats.squashedCohort.value(), 1.0);
    EXPECT_EQ(ledger.snapshotStats.liveAtClose.value(), 2.0);
    EXPECT_EQ(ledger.snapshotStats.liveCapacityBound.value(), 352.0);
    EXPECT_EQ(ledger.liveAgeMax.value(), 1000.0 - 14.0);

    // Post-snapshot: registration attempts are counted, never registered.
    LedgerTag late;
    ledger.registerAlloc(late, true, &late, 1001);
    EXPECT_EQ(late.state, LedgerState::Unallocated);
    EXPECT_EQ(ledger.postSnapshotRegistrations.value(), 1.0);
    EXPECT_EQ(ledger.allocated.value(), 4.0);       // live group frozen

    // Late settlements of snapshot-Live members.
    ledger.settleRetired(liveA, &liveA);
    ledger.settleSquashed(liveB, &liveB, SettleOrigin::RobWalk);
    EXPECT_EQ(ledger.settledAfterCloseRetired.value(), 1.0);
    EXPECT_EQ(ledger.settledAfterCloseSquashed.value(), 1.0);
    // The live group did not move (frozen).
    EXPECT_EQ(ledger.retiredCohort.value(), 1.0);
    EXPECT_EQ(ledger.squashedCohort.value(), 1.0);
    EXPECT_EQ(ledger.liveAtClose.value(), 2.0);
    // Late registration never settles (Unallocated -> no-op).
    ledger.settleRetired(late, &late);
    EXPECT_EQ(ledger.settledAfterCloseRetired.value(), 1.0);

    // A second snapshot does not refire.
    ledger.snapshot(1010);
    EXPECT_EQ(ledger.snapshotStats.allocated.value(), 4.0);

    // Invariant (1.2b(d)): liveAtClose >= settledAfterClose R + S.
    EXPECT_GE(ledger.snapshotStats.liveAtClose.value(),
              ledger.settledAfterCloseRetired.value() +
              ledger.settledAfterCloseSquashed.value());
}

TEST(TopdownLedger, SnapshotOutOfEpochLateSquashDoesNotOccupyAfterClose)
{
    // Routing-table row 4: pre-snapshot-epoch members settling AFTER the
    // snapshot land in settledOutOfEpoch (epoch stamp decides first).
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag pre;
    ledger.registerAlloc(pre, true, &pre, 10);
    ledger.beginEpoch();                    // pre has epoch 0, now epoch 1
    ledger.snapshot(100);                   // snapshot of epoch 1 (empty)
    ledger.settleSquashed(pre, &pre, SettleOrigin::RobWalk);
    EXPECT_EQ(ledger.settledOutOfEpochSquashed.value(), 1.0);
    EXPECT_EQ(ledger.settledAfterCloseSquashed.value(), 0.0);
}

TEST(TopdownLedger, LiveOverCapacityGuard)
{
    TopdownLedger::Config cfg = baseConfig();
    cfg.robCapacity = 2;
    TopdownLedger ledger(nullptr, cfg, true);
    LedgerTag a, b, c;
    ledger.registerAlloc(a, true, &a, 10);
    ledger.registerAlloc(b, true, &b, 11);
    ledger.registerAlloc(c, true, &c, 12);
    ledger.snapshot(100);
    EXPECT_EQ(ledger.liveOverCapacity.value(), 1.0);
    EXPECT_EQ(ledger.runState(), LedgerRunState::Invalid);
}

TEST(TopdownLedger, AccountCycleAggregatesAndGuards)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);

    // Cycle 1: 3 register, 3 crossings, admission Backend family blocked
    // (P2): upstreamShort 5 -> Backend.
    LedgerTag i1, i2, i3;
    ledger.registerAlloc(i1, true, &i1, 100);
    ledger.registerAlloc(i2, true, &i2, 100);
    ledger.registerAlloc(i3, true, &i3, 100);
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 3;
    in.crossings = 3;
    in.blockRename = true;
    in.renameBlockReason = StallReason::OtherFragStall;
    ledger.accountCycle(in);
    EXPECT_EQ(ledger.slotBudget.value(), 8.0);
    EXPECT_EQ(ledger.beSlots.value(), 5.0);
    EXPECT_EQ(ledger.feSlots.value(), 0.0);
    EXPECT_EQ(ledger.identityViolations.value(), 0.0);
    EXPECT_EQ(ledger.budgetOverrun.value(), 0.0);
    EXPECT_EQ(ledger.runState(), LedgerRunState::Valid);

    // Cycle 2: crossing count disagrees with registrations -> identity
    // violation + invalidation (INV-4 path).  The input itself stays
    // partition-consistent (arrivals=1/crossings=1) so ONLY the mismatch
    // fires — the partition violation has its own case below.
    DispatchCycleInputs in2 = baseInputs();
    in2.arrivals = 1;
    in2.crossings = 1;   // no registration happened this cycle
    ledger.accountCycle(in2);
    EXPECT_EQ(ledger.identityViolations.value(), 1.0);
    EXPECT_EQ(ledger.runState(), LedgerRunState::Invalid);

    // Crossing-beyond-delivered breaks the additive identity itself: in
    // the same cycle BOTH the crossing-mismatch check and the INV-4
    // partition assertion fire (two distinct violations, both legitimate:
    // the crossings claim is impossible for the delivered set).
    TopdownLedger part(nullptr, baseConfig(), true);
    DispatchCycleInputs in4 = baseInputs();
    in4.arrivals = 0;
    in4.crossings = 1;   // crossings > delivered
    part.accountCycle(in4);
    EXPECT_EQ(part.identityViolations.value(), 2.0);
    EXPECT_EQ(part.budgetOverrun.value(), 1.0);
    EXPECT_EQ(part.runState(), LedgerRunState::Invalid);

    // Budget overrun (delivered > budget) is flagged in slot units too.
    TopdownLedger over(nullptr, baseConfig(), true);
    DispatchCycleInputs in3 = baseInputs();
    in3.residueAtStart = 4;
    in3.arrivals = 8;    // delivered 12 > 8
    in3.crossings = 6;
    over.accountCycle(in3);
    EXPECT_EQ(over.budgetOverrun.value(), 1.0);
    EXPECT_GE(over.budgetOverrunSlots.value(), 1.0);
    EXPECT_EQ(over.runState(), LedgerRunState::Invalid);
}

TEST(TopdownLedger, AccountCycleFrozenIsNoOp)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag a;
    ledger.registerAlloc(a, true, &a, 10);
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 0;
    in.crossings = 0;
    ledger.accountCycle(in);
    ledger.snapshot(100);
    const double budget = ledger.slotBudget.value();
    const double fe = ledger.feSlots.value();
    DispatchCycleInputs in2 = baseInputs();
    in2.arrivals = 0;
    in2.crossings = 0;
    ledger.accountCycle(in2);           // frozen: no live-group movement
    EXPECT_EQ(ledger.slotBudget.value(), budget);
    EXPECT_EQ(ledger.feSlots.value(), fe);
}

TEST(TopdownLedger, DisabledLedgerIsQuiescent)
{
    // Layer-1 A/B off-arm semantics: everything stays zero.
    TopdownLedger ledger(nullptr, baseConfig(), false);
    EXPECT_FALSE(ledger.accounting());
    LedgerTag a;
    ledger.registerAlloc(a, true, &a, 10);
    ledger.settleRetired(a, &a);
    ledger.accountCycle(baseInputs());
    ledger.snapshot(100);
    EXPECT_EQ(ledger.allocated.value(), 0.0);
    EXPECT_EQ(ledger.retiredCohort.value(), 0.0);
    EXPECT_EQ(ledger.slotBudget.value(), 0.0);
    EXPECT_EQ(ledger.statEnabled.value(), 0.0);
    EXPECT_EQ(a.state, LedgerState::Unallocated);
    // FAIL-1 (acceptance card): the off arm must stay 0/NaN across the
    // WHOLE topdownV2 group.  replayComplete is a collection-completeness
    // self-report: a disabled ledger has no table, so it reports 0, never
    // 1 — the enabled state is carried by statEnabled alone.
    EXPECT_EQ(ledger.replayCompleteStat.value(), 0.0);
    EXPECT_FALSE(ledger.replayComplete());
    // resetStats must not resurrect it (the ROI-begin dump+reset path is
    // exactly where the acceptance FAIL-1 was observed).
    ledger.resetStats();
    EXPECT_EQ(ledger.replayCompleteStat.value(), 0.0);
    EXPECT_EQ(ledger.statEnabled.value(), 0.0);
    ledger.accountCycle(baseInputs());
    EXPECT_EQ(ledger.replayCompleteStat.value(), 0.0);
}

TEST(TopdownLedger, UnsupportedConfigurationsExplicit)
{
    // SMT.
    {
        TopdownLedger::Config cfg = baseConfig();
        cfg.numThreads = 2;
        TopdownLedger ledger(nullptr, cfg, true);
        EXPECT_EQ(ledger.configUnsupportedSMT.value(), 1.0);
        EXPECT_EQ(ledger.runState(), LedgerRunState::Unsupported);
        EXPECT_EQ(ledger.slotWidth.value(), 8.0);   // reported as configured
    }
    // enableDispatchStage=true.
    {
        TopdownLedger::Config cfg = baseConfig();
        cfg.enableDispatchStage = true;
        TopdownLedger ledger(nullptr, cfg, true);
        EXPECT_EQ(ledger.configUnsupportedDispatchStage.value(), 1.0);
        EXPECT_EQ(ledger.runState(), LedgerRunState::Unsupported);
    }
    // Width mismatch.
    {
        TopdownLedger::Config cfg = baseConfig();
        cfg.upstreamWidthMatches = false;
        TopdownLedger ledger(nullptr, cfg, true);
        EXPECT_EQ(ledger.configUnsupportedWidth.value(), 1.0);
        EXPECT_EQ(ledger.runState(), LedgerRunState::Unsupported);
    }
    // Invalid outranks unsupported once a guard trips.
    {
        TopdownLedger::Config cfg = baseConfig();
        cfg.numThreads = 2;
        TopdownLedger ledger(nullptr, cfg, true);
        LedgerTag a;
        ledger.registerAlloc(a, false, &a, 10);    // INV-6 violation
        EXPECT_EQ(ledger.runState(), LedgerRunState::Invalid);
    }
}

TEST(TopdownLedger, ResetStatsReopensSegmentAndRestampsConfig)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag a;
    ledger.registerAlloc(a, true, &a, 10);
    ledger.settleRetired(a, &a);
    ledger.snapshot(100);

    ledger.resetStats();   // framework-driven ROI-begin reset
    EXPECT_EQ(ledger.currentEpoch(), 1u);
    EXPECT_FALSE(ledger.frozen());
    EXPECT_EQ(ledger.allocated.value(), 0.0);
    EXPECT_EQ(ledger.retiredCohort.value(), 0.0);
    EXPECT_EQ(ledger.slotBudget.value(), 0.0);
    EXPECT_EQ(ledger.statEnabled.value(), 1.0);     // re-stamped
    EXPECT_EQ(ledger.slotWidth.value(), 8.0);       // re-stamped
    EXPECT_EQ(ledger.replayCompleteStat.value(), 1.0);
    // Live map survives (cross-segment settlement remains routable).
    ledger.settleSquashed(a, &a, SettleOrigin::LSQ);   // no-op: already Retired
    EXPECT_EQ(ledger.settledOutOfEpochSquashed.value(), 0.0);
}

// ===========================================================================
// Lossless replay table (section 1.5c): the full tuple table distinguishes
// inputs that the <=64-bin summary collapses, and aggregated slot weights
// reconcile with the ledger totals.
// ===========================================================================

TEST(ReplayTable, DistinctTuplesWithSameSummaryBinAreDistinguishable)
{
    // Two S21b-like cycles (P5 silent-FE, dominant Frontend, same bin)
    // that differ only in residue/budget composition.
    DispatchCycleInputs x = baseInputs();
    x.arrivals = 0;
    x.crossings = 0;
    x.residueAtStart = 0;

    DispatchCycleInputs y = baseInputs();
    y.arrivals = 0;
    y.crossings = 0;
    y.residueAtStart = 1;
    y.breakReason = StallReason::NoStall;   // residue without a signaled
                                            // break: breakBlocked=1 -> U
    // Make them land in the same summary bin by construction check first.
    const SlotSplit sx = classifyDispatchCycle(x);
    const SlotSplit sy = classifyDispatchCycle(y);
    const PatternBin bx = patternBin(x, sx);
    const PatternBin by = patternBin(y, sy);
    ASSERT_EQ(bx.x, by.x);   // same mode...
    ASSERT_EQ(bx.y, by.y);   // ...same dominant class

    TopdownLedger ledger(nullptr, baseConfig(), true);
    ledger.accountCycle(x);
    ledger.accountCycle(y);
    EXPECT_EQ(ledger.patternTable().size(), 2u);   // NOT collapsed
    EXPECT_EQ(ledger.replayDistinctTuples.value(), 2.0);
    EXPECT_EQ(ledger.replayCompleteStat.value(), 1.0);

    // Slot weights reconcile with the live counters (aggregate identity).
    double fe = 0, unknown = 0, budget = 0;
    for (const auto &kv : ledger.patternTable()) {
        fe += kv.second.classSlots[static_cast<size_t>(SlotClass::Frontend)];
        unknown +=
            kv.second.classSlots[static_cast<size_t>(SlotClass::Unknown)];
        budget += kv.first.budget;
    }
    EXPECT_EQ(fe, ledger.feSlots.value());
    EXPECT_EQ(unknown, ledger.unknownSlots.value());
    EXPECT_EQ(budget, ledger.slotBudget.value());
}

TEST(ReplayTable, AlternatePolicyReplayRecoversDifferentResults)
{
    // The acceptance requirement (section 1.5c): a replay that changes the
    // policy verdict (P5 Frontend -> Unknown) must be recoverable from the
    // stored full tuples alone — the summary histogram cannot do this.
    DispatchCycleInputs silentFE = baseInputs();
    silentFE.arrivals = 0;
    silentFE.crossings = 0;
    DispatchCycleInputs drainCycle = baseInputs();
    drainCycle.arrivals = 0;
    drainCycle.crossings = 0;
    drainCycle.drainMode = true;

    TopdownLedger ledger(nullptr, baseConfig(), true);
    ledger.accountCycle(silentFE);
    ledger.accountCycle(drainCycle);
    ASSERT_EQ(ledger.feSlots.value(), 8.0);
    ASSERT_EQ(ledger.drainSlots.value(), 8.0);

    // Offline replay with an alternate P5 policy (Frontend -> Unknown).
    double replayFE = 0, replayUnknown = 0, replayDrain = 0;
    for (const auto &kv : ledger.patternTable()) {
        // A real policy replay would re-run classifyDispatchCycle with the
        // modified policy; here the tuple is sufficient to decide P5 rows.
        const DispatchCycleInputs &t = kv.first;
        const SlotSplit s = classifyDispatchCycle(t);
        const bool wasP5 = (s.upstreamRule == TermRule::P5SilentFE);
        for (size_t c = 0; c < static_cast<size_t>(SlotClass::NumSlotClasses);
             c++) {
            double slots = kv.second.classSlots[c];
            if (wasP5 && c == static_cast<size_t>(SlotClass::Frontend)) {
                replayUnknown += slots;      // policy change applies
            } else if (c == static_cast<size_t>(SlotClass::Frontend)) {
                replayFE += slots;
            } else if (c == static_cast<size_t>(SlotClass::Drain)) {
                replayDrain += slots;
            } else if (c == static_cast<size_t>(SlotClass::Unknown)) {
                replayUnknown += slots;
            }
        }
    }
    // The original ledger said FE=8; the alternate policy says Unknown=8
    // — recoverable only because the full tuple (chain reasons, admission
    // state) was retained, not just the summary bin.
    EXPECT_EQ(replayFE, 0.0);
    EXPECT_EQ(replayUnknown, 8.0);
    EXPECT_EQ(replayDrain, 8.0);
}

TEST(ReplayTable, ResourceCapMarksReplayIncomplete)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);
    // Fill the table with distinct tuples up to the cap.
    for (unsigned i = 0; i < TopdownLedger::kPatternTableCap; i++) {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.residueAtStart = i;   // distinct tuples
        in.breakReason = StallReason::NoStall;
        ledger.accountCycle(in);
    }
    EXPECT_EQ(ledger.replayCompleteStat.value(), 1.0);
    EXPECT_TRUE(ledger.replayComplete());

    // One more distinct tuple: dropped, flagged, counted — never silently.
    DispatchCycleInputs extra = baseInputs();
    extra.arrivals = 0;
    extra.crossings = 0;
    extra.residueAtStart = TopdownLedger::kPatternTableCap + 1;
    ledger.accountCycle(extra);
    EXPECT_EQ(ledger.replayCompleteStat.value(), 0.0);
    EXPECT_FALSE(ledger.replayComplete());
    EXPECT_EQ(ledger.replayTableOverflowTuples.value(), 1.0);
    // Already-retained tuples still update.
    DispatchCycleInputs repeat = baseInputs();
    repeat.arrivals = 0;
    repeat.crossings = 0;
    repeat.residueAtStart = 0;
    repeat.breakReason = StallReason::NoStall;
    ledger.accountCycle(repeat);
    EXPECT_EQ(ledger.replayTableOverflowTuples.value(), 1.0);
    EXPECT_EQ(ledger.patternTable().size(), TopdownLedger::kPatternTableCap);
}

// ===========================================================================
// Cohort identity II at snapshot time (section 1.3): allocated ==
// retiredCohort + squashedCohort + liveAtClose.
// ===========================================================================

TEST(CohortIdentity, IdentityIIAtSnapshot)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);
    std::vector<LedgerTag> tags(10);
    for (int i = 0; i < 10; i++) {
        LedgerTag &t = tags[i];
        ledger.registerAlloc(t, true, &t, 100 + i);
        if (i < 3)
            ledger.settleRetired(t, &t);
        else if (i < 7)
            ledger.settleSquashed(t, &t, SettleOrigin::IQ);
        // 3 remain live
    }
    ledger.snapshot(500);
    EXPECT_EQ(ledger.snapshotStats.allocated.value(), 10.0);
    EXPECT_EQ(ledger.snapshotStats.retiredCohort.value()
              + ledger.snapshotStats.squashedCohort.value()
              + ledger.snapshotStats.liveAtClose.value(), 10.0);
}

// ===========================================================================
// SlotPatternDist aggregation: per (mode, dominant class) cycle counts
// reconcile with the class-slot totals' cycle structure.
// ===========================================================================

TEST(SlotPatternDist, BinsAccumulate)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);
    // Two P5 silent-FE cycles and one full-width cycle.
    for (int i = 0; i < 2; i++) {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        ledger.accountCycle(in);
    }
    ledger.accountCycle(baseInputs());
    // Mode 5 (silentFE) x class 0 (Frontend) = 2 cycles; mode 0 = 1 cycle.
    double total = 0;
    for (unsigned x = 0; x < 8; x++)
        for (unsigned y = 0; y < 8; y++)
            total += ledger.slotPatternDist[x][y].value();
    EXPECT_EQ(total, 3.0);
}

// ===========================================================================
// P4 derived ratios: exact values, explicit NaN guards, sumCheck identity.
// ===========================================================================

TEST(LedgerRatios, ExactFractionsAndSumCheck)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);
    // Cycle 1: 3 cross, 5 P5-frontend unused.
    LedgerTag i1, i2, i3;
    ledger.registerAlloc(i1, true, &i1, 100);
    ledger.registerAlloc(i2, true, &i2, 100);
    ledger.registerAlloc(i3, true, &i3, 100);
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 3;
    in.crossings = 3;
    ledger.accountCycle(in);
    // Cycle 2: five more cross the full-width arrival (consistent input:
    // 5 registrations, 5 crossings; the 3 unused slots go P5 Frontend).
    LedgerTag i4, i5, i6, i7, i8;
    ledger.registerAlloc(i4, true, &i4, 110);
    ledger.registerAlloc(i5, true, &i5, 110);
    ledger.registerAlloc(i6, true, &i6, 110);
    ledger.registerAlloc(i7, true, &i7, 110);
    ledger.registerAlloc(i8, true, &i8, 110);
    DispatchCycleInputs in2 = baseInputs();
    in2.arrivals = 5;
    in2.crossings = 5;
    ledger.accountCycle(in2);

    // Cycle 3: 1 retire (settlement in commit-phase order), 2 squash, 3
    // live; admission-BS blocked empty cycle -> 8 recovery slots.
    ledger.settleRetired(i1, &i1);
    ledger.settleSquashed(i2, &i2, SettleOrigin::IQ);
    ledger.settleSquashed(i3, &i3, SettleOrigin::RobWalk);
    DispatchCycleInputs in3 = baseInputs();
    in3.arrivals = 0;
    in3.crossings = 0;
    in3.blockIEW = true;
    in3.iewBlockReason = StallReason::CommitSquash;
    ledger.accountCycle(in3);

    // Cohort: 8 allocated, 1 retired, 2 squashed, 5 live.
    // Slots: budget 24; crossings 8; fe 8 (5+3); recovery 8.
    EXPECT_EQ(ledger.slotBudget.value(), 24.0);
    EXPECT_EQ(ledger.retiredCohort.value(), 1.0);
    EXPECT_EQ(ledger.squashedCohort.value(), 2.0);
    EXPECT_EQ(ledger.liveAtClose.value(), 5.0);
    EXPECT_EQ(ledger.feSlots.value(), 8.0);
    EXPECT_EQ(ledger.recoverySlots.value(), 8.0);
    EXPECT_EQ(ledger.identityViolations.value(), 0.0);

    EXPECT_DOUBLE_EQ(ledger.retiring.value(), 1.0 / 24.0);
    EXPECT_DOUBLE_EQ(ledger.badSpec.value(), (2.0 + 8.0) / 24.0);
    EXPECT_DOUBLE_EQ(ledger.frontend.value(), 8.0 / 24.0);
    EXPECT_DOUBLE_EQ(ledger.backend.value(), 0.0);
    EXPECT_DOUBLE_EQ(ledger.boundary.value(), 5.0 / 24.0);
    EXPECT_DOUBLE_EQ(ledger.unknown.value(), 0.0);
    // Representation invariant: identities I+II hold -> sum is exactly 1.
    EXPECT_DOUBLE_EQ(ledger.sumCheck.value(), 1.0);
    // Double addition of these exact dyadic fractions is exact.
    EXPECT_EQ(ledger.retiring.value() + ledger.badSpec.value() +
              ledger.frontend.value() + ledger.backend.value() +
              ledger.boundary.value() + ledger.unknown.value(), 1.0);
}

TEST(LedgerRatios, DisabledLedgerRatiosAreNaN)
{
    TopdownLedger ledger(nullptr, baseConfig(), false);
    EXPECT_TRUE(std::isnan(ledger.retiring.value()));
    EXPECT_TRUE(std::isnan(ledger.sumCheck.value()));
    // Even after cycles that would otherwise compute values.
    ledger.accountCycle(baseInputs());
    EXPECT_TRUE(std::isnan(ledger.retiring.value()));
    EXPECT_TRUE(std::isnan(ledger.boundary.value()));
}

TEST(LedgerRatios, UnsupportedConfigRatiosAreNaNEvenWithBudget)
{
    // S23 integration form: non-zero counters, unsupported state -> NaN
    // (never 0, never inf) — the explicit validity guard, not slotWidth
    // trickery.
    TopdownLedger::Config cfg = baseConfig();
    cfg.numThreads = 2;
    TopdownLedger ledger(nullptr, cfg, true);
    LedgerTag a;
    ledger.registerAlloc(a, true, &a, 100);
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 1;
    in.crossings = 1;
    ledger.accountCycle(in);
    EXPECT_GT(ledger.slotBudget.value(), 0.0);
    EXPECT_EQ(ledger.allocated.value(), 1.0);
    for (gem5::statistics::Scalar *s : std::vector<gem5::statistics::Scalar *>{
             &ledger.retiring, &ledger.badSpec, &ledger.frontend,
             &ledger.backend, &ledger.boundary, &ledger.unknown,
             &ledger.sumCheck}) {
        const double v = s->value();
        EXPECT_TRUE(std::isnan(v)) << "expected NaN ratio";
        EXPECT_FALSE(std::isinf(v));
        EXPECT_NE(v, 0.0);
    }
}

TEST(LedgerRatios, ZeroBudgetRatiosAreNaN)
{
    // S12-family: budget 0 with non-zero numerator candidates.
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag a;
    ledger.registerAlloc(a, true, &a, 100);
    ledger.settleRetired(a, &a);   // numerator 1, denominator 0
    DispatchCycleInputs in;
    in.budget = 0;
    ledger.accountCycle(in);
    EXPECT_EQ(ledger.retiredCohort.value(), 1.0);
    EXPECT_EQ(ledger.slotBudget.value(), 0.0);
    EXPECT_TRUE(std::isnan(ledger.retiring.value()));
    EXPECT_TRUE(std::isnan(ledger.sumCheck.value()));
}

TEST(LedgerRatios, FrozenSnapshotRatiosStable)
{
    // seg3 == seg2 for the whole topdownV2 group: after the snapshot the
    // ratios (like every live-group member) must not move.
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag a, b;
    ledger.registerAlloc(a, true, &a, 100);
    ledger.registerAlloc(b, true, &b, 100);
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 2;
    in.crossings = 2;
    ledger.accountCycle(in);
    ledger.snapshot(200);
    const double retiring = ledger.retiring.value();
    const double boundary = ledger.boundary.value();
    // Post-freeze settlements (settledAfterClose diagnostics only).
    ledger.settleRetired(a, &a);
    ledger.settleSquashed(b, &b, SettleOrigin::IQ);
    // A frozen accountCycle must not refresh the live ratios.
    DispatchCycleInputs tail = baseInputs();
    tail.arrivals = 0;
    tail.crossings = 0;
    ledger.accountCycle(tail);
    EXPECT_EQ(ledger.retiring.value(), retiring);
    EXPECT_EQ(ledger.boundary.value(), boundary);
}

// ===========================================================================
// Persistent audit export (F1/F6, d4b-audit-export-v1): the lossless tuple
// table must land on disk for production runs — summary stats alone are
// not replay evidence (acceptance negative list).
// ===========================================================================

namespace
{

std::string
readFile(const std::string &path)
{
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

bool
contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // anonymous namespace

TEST(AuditExport, WritesHeaderAndFullTupleTable)
{
    const std::string path = "topdownV2_audit_gtest.json";
    std::remove(path.c_str());

    TopdownLedger ledger(nullptr, baseConfig(), true);
    ledger.setAuditExportPath(path);

    // Cycle 1: S21b-family silent chain, ready admission -> Frontend 8.
    DispatchCycleInputs c1 = baseInputs();
    c1.arrivals = 0;
    c1.crossings = 0;
    ledger.accountCycle(c1);
    // Cycle 2: S20-family admission BS block -> Recovery 8.
    DispatchCycleInputs c2 = baseInputs();
    c2.arrivals = 0;
    c2.crossings = 0;
    c2.blockIEW = true;
    c2.iewBlockReason = StallReason::CommitSquash;
    c2.blockRename = true;
    c2.renameBlockReason = StallReason::CommitSquash;
    ledger.accountCycle(c2);

    EXPECT_TRUE(ledger.exportAudit());
    const std::string text = readFile(path);
    EXPECT_FALSE(text.empty());

    // d4b-audit-export-v1 header with F6 version metadata.
    EXPECT_TRUE(contains(text, "\"schema_version\": \"d4b-audit-export-v1\""));
    EXPECT_TRUE(contains(text,
        "\"impl_schema_version\": \"d4b-audit-export-v1.1-hopmultiset\""));
    EXPECT_TRUE(contains(text, "\"policy_version\": \"d4b-policy-v4\""));
    EXPECT_TRUE(contains(text, "\"replay_complete\": true"));
    EXPECT_TRUE(contains(text, "\"dropped_patterns\": 0"));
    EXPECT_TRUE(contains(text, "\"resource_cap\": {\"max_patterns\": 16384"));

    // All 17 signal fields + slot weights + counts per pattern.
    EXPECT_TRUE(contains(text, "\"budget\": 8"));
    EXPECT_TRUE(contains(text, "\"residueAtStart\": 0"));
    EXPECT_TRUE(contains(text, "\"arrivals\": 0"));
    EXPECT_TRUE(contains(text, "\"crossings\": 0"));
    EXPECT_TRUE(contains(text, "\"poppedSquashed\": 0"));
    EXPECT_TRUE(contains(text, "\"drainMode\": false"));
    EXPECT_TRUE(contains(text, "\"breakReason\": \"NoStall\""));
    EXPECT_TRUE(contains(text, "\"blockIEW\": true"));
    EXPECT_TRUE(contains(text, "\"iewBlockReason\": \"CommitSquash\""));
    EXPECT_TRUE(contains(text, "\"blockRename\": true"));
    EXPECT_TRUE(contains(text, "\"renameBlockReason\": \"CommitSquash\""));
    EXPECT_TRUE(contains(text, "\"renameUpReasons\": []"));
    EXPECT_TRUE(contains(text, "\"commitSquashWire\": false"));
    EXPECT_TRUE(contains(text, "\"robSquashingWire\": false"));
    EXPECT_TRUE(contains(text, "\"count\": 1"));
    EXPECT_TRUE(contains(text, "\"Recovery\": 8"));
    EXPECT_TRUE(contains(text, "\"Frontend\": 8"));

    // Atomic write leaves no .tmp residue.
    EXPECT_EQ(readFile(path + ".tmp").size(), 0u);
    std::remove(path.c_str());
}

TEST(AuditExport, DistinguishesTuplesWithIdenticalSummaryBin)
{
    // Contract section 1.5c acceptance requirement: two cycles whose
    // <=64-bin summary projection is identical but whose full tuples
    // differ must be distinguishable in the exported table (this is what
    // makes an offline alternate-policy replay able to recover different
    // results).  Both cycles below classify Frontend=8 in the same
    // slotPatternDist bin; they differ in the informational squash wires.
    const std::string path = "topdownV2_audit_gtest2.json";
    std::remove(path.c_str());

    TopdownLedger ledger(nullptr, baseConfig(), true);
    ledger.setAuditExportPath(path);

    DispatchCycleInputs c1 = baseInputs();
    c1.arrivals = 0;
    c1.crossings = 0;
    ledger.accountCycle(c1);

    DispatchCycleInputs c2 = c1;
    c2.commitSquashWire = true;   // same summary bin, different tuple
    ledger.accountCycle(c2);

    const auto &table = ledger.patternTable();
    ASSERT_EQ(table.size(), 2u);

    EXPECT_TRUE(ledger.exportAudit());
    const std::string text = readFile(path);
    // Both tuples present with their distinguishing field: exactly one
    // pattern carries commitSquashWire=true.
    const std::string needle = "\"commitSquashWire\": true";
    size_t pos = text.find(needle);
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(text.find(needle, pos + needle.size()),
              std::string::npos);
    std::remove(path.c_str());
}

TEST(AuditExport, EpochSegmentsSurviveBeginEpochReset)
{
    const std::string path = "topdownV2_audit_gtest3.json";
    std::remove(path.c_str());

    TopdownLedger ledger(nullptr, baseConfig(), true);
    ledger.setAuditExportPath(path);

    DispatchCycleInputs c1 = baseInputs();
    c1.arrivals = 0;
    c1.crossings = 0;
    ledger.accountCycle(c1);
    EXPECT_TRUE(ledger.exportAudit());

    // ROI begin: dump+reset opens the next segment (epoch 1 -> 2); the
    // already-exported epoch must survive in the file.
    ledger.resetStats();

    DispatchCycleInputs c2 = baseInputs();
    c2.arrivals = 0;
    c2.crossings = 0;
    c2.blockIEW = true;
    c2.iewBlockReason = StallReason::ROBFull;
    ledger.accountCycle(c2);
    EXPECT_TRUE(ledger.exportAudit());

    const std::string text = readFile(path);
    EXPECT_TRUE(contains(text, "\"epoch\": 0"));
    EXPECT_TRUE(contains(text, "\"epoch\": 1"));
    // Epoch-1 pattern retained after the reset (Frontend 8), epoch-2
    // pattern present (Backend 8 via P2).
    EXPECT_TRUE(contains(text, "\"Frontend\": 8"));
    EXPECT_TRUE(contains(text, "\"Backend\": 8"));
    std::remove(path.c_str());
}

TEST(AuditExport, DisabledLedgerWritesNothing)
{
    const std::string path = "topdownV2_audit_gtest4.json";
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());

    TopdownLedger ledger(nullptr, baseConfig(), false);
    ledger.setAuditExportPath(path);
    ledger.accountCycle(baseInputs());
    // A disabled ledger exports nothing (inert by construction) and
    // reports success without touching the filesystem.
    EXPECT_TRUE(ledger.exportAudit());
    std::ifstream f(path);
    EXPECT_FALSE(f.good());
    EXPECT_EQ(ledger.replayCompleteStat.value(), 0.0);
}

TEST(AuditExport, OverflowMarksIncompleteInHeader)
{
    const std::string path = "topdownV2_audit_gtest5.json";
    std::remove(path.c_str());

    TopdownLedger ledger(nullptr, baseConfig(), true);
    ledger.setAuditExportPath(path);

    // Distinct tuples: vary residueAtStart (all classify FE=8; delivered
    // <= budget holds).
    for (unsigned i = 0; i < TopdownLedger::kPatternTableCap; i++) {
        DispatchCycleInputs in = baseInputs();
        in.arrivals = 0;
        in.crossings = 0;
        in.residueAtStart = i;
        ledger.accountCycle(in);
    }
    // One more distinct tuple: dropped after the cap.
    DispatchCycleInputs extra = baseInputs();
    extra.arrivals = 0;
    extra.crossings = 0;
    extra.residueAtStart = TopdownLedger::kPatternTableCap + 1;
    ledger.accountCycle(extra);

    EXPECT_TRUE(ledger.exportAudit());
    const std::string text = readFile(path);
    EXPECT_TRUE(contains(text, "\"replay_complete\": false"));
    EXPECT_TRUE(contains(text, "\"dropped_patterns\": 1"));
    std::remove(path.c_str());
}

// ===========================================================================
// Lifecycle observation counters (production coverage: S7/S16/S17/S18/
// epoch row4/S22b).  Statistics-only hooks whose purpose is to make
// "counter is zero" distinguishable from "path never fired": raw hook
// entry counts, absorbed duplicates by prior tag state, per-origin
// effective/absorbed splits, the nested-squash marker, and the S22b
// ROI-end boundary sample.  All of them obey the frozen live-group
// discipline: after the snapshot only the registered may-grow
// diagnostics keep growing.
// ===========================================================================

TEST(LifecycleObservation, IdempotenceAbsorptionAccounting)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);

    // Two registrations, one retirement, one squash, then the S16-style
    // duplicate notifications and a pre-C4 belt no-op.
    LedgerTag a, b, prec;
    ledger.registerAlloc(a, true, &a, 100);
    ledger.registerAlloc(b, true, &b, 100);
    ledger.settleRetired(a, &a, 100);              // effective retire
    ledger.settleRetired(a, &a, 100);              // duplicate: absorbed
    ledger.settleSquashed(b, &b, SettleOrigin::IQ, 100);       // effective
    ledger.settleSquashed(b, &b, SettleOrigin::RobWalk, 100);  // duplicate
    ledger.settleSquashed(prec, &prec, SettleOrigin::PreCutBelt, 100);

    EXPECT_EQ(ledger.registerHookCalls.value(), 2.0);
    EXPECT_EQ(ledger.settleRetiredHookCalls.value(), 2.0);
    EXPECT_EQ(ledger.settleRetiredEffective.value(), 1.0);
    EXPECT_EQ(ledger.settleRetiredAbsorbed.value(), 1.0);
    EXPECT_EQ(ledger.settleSquashedHookCalls.value(), 3.0);
    EXPECT_EQ(ledger.settleSquashedEffective.value(), 1.0);
    EXPECT_EQ(ledger.settleSquashedAbsorbed.value(), 2.0);
    EXPECT_EQ(ledger.settleAbsorbedUnallocated.value(), 1.0);
    EXPECT_EQ(ledger.settleAbsorbedRetired.value(), 1.0);
    EXPECT_EQ(ledger.settleAbsorbedSquashed.value(), 1.0);

    // Per-origin splits.
    EXPECT_EQ(ledger.settleOriginEffective[static_cast<unsigned>(
                  SettleOrigin::IQ)].value(), 1.0);
    EXPECT_EQ(ledger.settleOriginEffective[static_cast<unsigned>(
                  SettleOrigin::RobWalk)].value(), 0.0);
    EXPECT_EQ(ledger.settleOriginAbsorbed[static_cast<unsigned>(
                  SettleOrigin::RobWalk)].value(), 1.0);
    EXPECT_EQ(ledger.settleOriginAbsorbed[static_cast<unsigned>(
                  SettleOrigin::PreCutBelt)].value(), 1.0);

    // Documented identities (collecting, not frozen).
    EXPECT_EQ(ledger.settleRetiredEffective.value(),
              ledger.retiredCohort.value() +
              ledger.settledOutOfEpochRetired.value());
    EXPECT_EQ(ledger.settleSquashedEffective.value(),
              ledger.squashedCohort.value() +
              ledger.settledOutOfEpochSquashed.value());
    EXPECT_EQ(ledger.settleRetiredHookCalls.value() -
              ledger.settleRetiredEffective.value(),
              ledger.settleRetiredAbsorbed.value());
    EXPECT_EQ(ledger.settleSquashedHookCalls.value() -
              ledger.settleSquashedEffective.value(),
              ledger.settleSquashedAbsorbed.value());
}

TEST(LifecycleObservation, OutOfEpochRoutingIsObserved)
{
    // epoch row4 material: a pre-segment registration settling squashed
    // inside the segment lands in settledOutOfEpochSquashed AND in the
    // settleOriginOutOfEpoch vector.
    TopdownLedger ledger(nullptr, baseConfig(), true);
    LedgerTag pre;
    ledger.registerAlloc(pre, true, &pre, 10);
    ledger.resetStats();   // beginEpoch: pre is now a pre-segment member
    ledger.settleSquashed(pre, &pre, SettleOrigin::LSQ, 20);

    EXPECT_EQ(ledger.settledOutOfEpochSquashed.value(), 1.0);
    EXPECT_EQ(ledger.settleOriginOutOfEpoch[static_cast<unsigned>(
                  SettleOrigin::LSQ)].value(), 1.0);
    EXPECT_EQ(ledger.settleOriginEffective[static_cast<unsigned>(
                  SettleOrigin::LSQ)].value(), 0.0);
    EXPECT_EQ(ledger.settleSquashedEffective.value(), 1.0);
}

TEST(LifecycleObservation, BoundarySampleAtSnapshot)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);

    // Tick 100: two registrations during the tick, one settle during the
    // tick (pre-snapshot phase), then the Option-B end-of-tick snapshot.
    ledger.observeTick(100);
    LedgerTag a, b;
    ledger.registerAlloc(a, true, &a, 100);
    ledger.registerAlloc(b, true, &b, 100);
    ledger.settleRetired(a, &a, 100);
    DispatchCycleInputs in = baseInputs();
    in.arrivals = 2;
    in.crossings = 2;
    ledger.accountCycle(in);
    ledger.snapshot(100);

    EXPECT_EQ(ledger.snapshotBoundaryTick.value(), 100.0);
    EXPECT_EQ(ledger.snapshotTickRegistrations.value(), 2.0);
    EXPECT_EQ(ledger.snapshotTickSettlesPre.value(), 1.0);
    EXPECT_EQ(ledger.snapshotTickSettlesPost.value(), 0.0);

    // A settle in a LATER tick lands in settledAfterClose (may-grow) and
    // never moves the frozen boundary sample.
    ledger.settleSquashed(b, &b, SettleOrigin::RobWalk, 200);
    EXPECT_EQ(ledger.settledAfterCloseSquashed.value(), 1.0);
    EXPECT_EQ(ledger.snapshotTickSettlesPre.value(), 1.0);
    EXPECT_EQ(ledger.snapshotTickRegistrations.value(), 2.0);

    // Same-tick observation with no events: a snapshot on an eventless
    // tick reports zeros, not stale scratch.
    TopdownLedger quiet(nullptr, baseConfig(), true);
    quiet.observeTick(500);
    LedgerTag c;
    quiet.registerAlloc(c, true, &c, 400);   // previous tick's event
    quiet.snapshot(500);
    EXPECT_EQ(quiet.snapshotBoundaryTick.value(), 500.0);
    EXPECT_EQ(quiet.snapshotTickRegistrations.value(), 0.0);
    EXPECT_EQ(quiet.snapshotTickSettlesPre.value(), 0.0);
}

TEST(LifecycleObservation, NestedSquashMarkerAndFrozenDiscipline)
{
    TopdownLedger ledger(nullptr, baseConfig(), true);
    ledger.noteNestedSquash();
    ledger.noteNestedSquash();
    EXPECT_EQ(ledger.nestedSquashObservations.value(), 2.0);

    LedgerTag a;
    ledger.registerAlloc(a, true, &a, 10);
    ledger.snapshot(10);

    // Frozen live-group discipline: post-snapshot hook entries only feed
    // the registered may-grow diagnostics, never the observation group.
    ledger.noteNestedSquash();
    LedgerTag post;
    ledger.registerAlloc(post, true, &post, 20);
    ledger.settleRetired(a, &a, 20);
    EXPECT_EQ(ledger.nestedSquashObservations.value(), 2.0);
    EXPECT_EQ(ledger.registerHookCalls.value(), 1.0);
    EXPECT_EQ(ledger.postSnapshotRegistrations.value(), 1.0);
    EXPECT_EQ(ledger.settleRetiredHookCalls.value(), 0.0);
    EXPECT_EQ(ledger.settleRetiredEffective.value(), 0.0);
    EXPECT_EQ(ledger.settledAfterCloseRetired.value(), 1.0);
}

TEST(LifecycleObservation, SettleOriginNamesMatchEnum)
{
    EXPECT_STREQ(settleOriginName(SettleOrigin::Retire), "Retire");
    EXPECT_STREQ(settleOriginName(SettleOrigin::CommitBelt), "CommitBelt");
    EXPECT_STREQ(settleOriginName(SettleOrigin::PreCutBelt), "PreCutBelt");
    EXPECT_STREQ(settleOriginName(SettleOrigin::RobWalk), "RobWalk");
    EXPECT_STREQ(settleOriginName(SettleOrigin::IQ), "IQ");
    EXPECT_STREQ(settleOriginName(SettleOrigin::LSQ), "LSQ");
    EXPECT_STREQ(settleOriginName(SettleOrigin::ExecSkip), "ExecSkip");
    EXPECT_STREQ(settleOriginName(SettleOrigin::DispQue), "DispQue");
    EXPECT_STREQ(settleOriginName(SettleOrigin::BufferSquash),
                 "BufferSquash");
    EXPECT_EQ(static_cast<unsigned>(SettleOrigin::NumSettleOrigins), 9u);
}
