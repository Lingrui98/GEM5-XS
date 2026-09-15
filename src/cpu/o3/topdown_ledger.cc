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

#include "cpu/o3/topdown_ledger.hh"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

#include "base/logging.hh"

namespace gem5
{

namespace o3
{

namespace
{

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/**
 * StallReason value name for the audit export (comm.hh enum name,
 * validation INTERFACE.md section 4: "StallReason 字段用枚举名文本").
 * The mixed sentinel renders as its dedicated out-of-enum name so the
 * export never injects a fake legal enum value.
 */
const char *
stallReasonName(StallReason r)
{
    if (r == kChainMixedStall)
        return "ChainMixed";
    switch (r) {
      case StallReason::NoStall:            return "NoStall";
      case StallReason::IcacheStall:        return "IcacheStall";
      case StallReason::ITlbStall:          return "ITlbStall";
      case StallReason::DTlbStall:          return "DTlbStall";
      case StallReason::BpStall:            return "BpStall";
      case StallReason::IntStall:           return "IntStall";
      case StallReason::TrapStall:          return "TrapStall";
      case StallReason::FTQBubble:          return "FTQBubble";
      case StallReason::FetchFragStall:     return "FetchFragStall";
      case StallReason::OtherFetchStall:    return "OtherFetchStall";
      case StallReason::OtherFragStall:     return "OtherFragStall";
      case StallReason::SquashStall:        return "SquashStall";
      case StallReason::FetchBufferInvalid: return "FetchBufferInvalid";
      case StallReason::InstMisPred:        return "InstMisPred";
      case StallReason::InstSquashed:       return "InstSquashed";
      case StallReason::SerializeStall:     return "SerializeStall";
      case StallReason::ScalarLongExecute:  return "ScalarLongExecute";
      case StallReason::VectorLongExecute:  return "VectorLongExecute";
      case StallReason::InstNotReady:       return "InstNotReady";
      case StallReason::LoadL1Bound:        return "LoadL1Bound";
      case StallReason::LoadL2Bound:        return "LoadL2Bound";
      case StallReason::LoadL3Bound:        return "LoadL3Bound";
      case StallReason::LoadMemBound:       return "LoadMemBound";
      case StallReason::StoreL1Bound:       return "StoreL1Bound";
      case StallReason::StoreL2Bound:       return "StoreL2Bound";
      case StallReason::StoreL3Bound:       return "StoreL3Bound";
      case StallReason::StoreMemBound:      return "StoreMemBound";
      case StallReason::MemSquashed:        return "MemSquashed";
      case StallReason::MemNotReady:        return "MemNotReady";
      case StallReason::MemCommitRateLimit: return "MemCommitRateLimit";
      case StallReason::Atomic:             return "Atomic";
      case StallReason::OtherMemStall:     return "OtherMemStall";
      case StallReason::MemDQBandwidth:     return "MemDQBandwidth";
      case StallReason::IntDQBandwidth:     return "IntDQBandwidth";
      case StallReason::FVDQBandwidth:     return "FVDQBandwidth";
      case StallReason::VectorReadyButNotIssued:
        return "VectorReadyButNotIssued";
      case StallReason::ScalarReadyButNotIssued:
        return "ScalarReadyButNotIssued";
      case StallReason::ResumeUnblock:      return "ResumeUnblock";
      case StallReason::CommitSquash:       return "CommitSquash";
      case StallReason::ROBFull:            return "ROBFull";
      case StallReason::RegFull:            return "RegFull";
      case StallReason::OtherStall:         return "OtherStall";
      case StallReason::NumStallReasons:    break;
    }
    return "<unknown-stall-reason>";
}

/** Render one uint64 reason-multiset mask as a JSON array of names. */
std::string
reasonsMaskJson(uint64_t mask)
{
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (unsigned r = 0; r < 64; r++) {
        if (mask & (1ULL << r)) {
            if (!first)
                os << ", ";
            os << "\"" << stallReasonName(static_cast<StallReason>(r))
               << "\"";
            first = false;
        }
    }
    os << "]";
    return os.str();
}

unsigned
toUnsigned(int v)
{
    // Terms are clamped only for counting; input guards flag the violation.
    return v > 0 ? static_cast<unsigned>(v) : 0u;
}

/**
 * P2 predicate (owner-frozen "backend provably blocks admission"):
 * a same-cycle admission-side block whose reason maps to the Backend
 * family via the section 1.5d table.  Both arms are checked against
 * the Backend family EXACTLY (== Backend, not "!= Recovery"): an
 * unclassified reason (Unknown class, admission-role BpStall,
 * out-of-table values, or a block without a sampled reason) must not
 * enter P2 as a Backend attribution on negative evidence
 * (CORE_REVIEW F2/F4: the != Recovery form silently treated Unknown
 * and even NoStall as Backend).
 */
bool
admissionBackendBlocked(const DispatchCycleInputs &in)
{
    if (in.blockIEW &&
        classifyStallReason(in.iewBlockReason, false) == SlotClass::Backend)
        return true;
    if (in.blockRename &&
        classifyStallReason(in.renameBlockReason, false) == SlotClass::Backend)
        return true;
    return false;
}

/**
 * Admission block present but the reason is unclassified evidence:
 * neither Backend family (P2) nor BS family (P3's blockIEW arm).  The
 * consumer provably refused this cycle (blockIEW/blockRename true),
 * so the shortage is NOT attributable upstream and the silent-ready
 * premise of P5 does not hold; the honest destination is Unknown
 * (main-session directive: "unclassified admission keeps the Unknown
 * path, must not fall through to P5 silent-ready FE").  Covers
 * admission-role BpStall (R-16), out-of-table sentinel values, and a
 * block whose reason was not sampled (NoStall).
 */
bool
admissionUnclassifiedBlocked(const DispatchCycleInputs &in)
{
    if (in.blockIEW) {
        const SlotClass c = classifyStallReason(in.iewBlockReason, false);
        if (c != SlotClass::Backend && c != SlotClass::Recovery)
            return true;
    }
    if (in.blockRename) {
        const SlotClass c = classifyStallReason(in.renameBlockReason, false);
        if (c != SlotClass::Backend && c != SlotClass::Recovery)
            return true;
    }
    return false;
}

/** P3 predicate: admission-side same-cycle BS-family block. */
bool
admissionRecoveryBlocked(const DispatchCycleInputs &in)
{
    return in.blockIEW &&
           classifyStallReason(in.iewBlockReason, false) == SlotClass::Recovery;
}

/** Squash-scene evidence (popped-term rule, section 1.5b). */
bool
squashSceneEvidence(const DispatchCycleInputs &in)
{
    return admissionRecoveryBlocked(in) || in.commitSquashWire ||
           in.robSquashingWire;
}

/** Number of non-silent chain hops. */
unsigned
chainNonSilentHops(const DispatchCycleInputs &in)
{
    unsigned hops = 0;
    if (in.renameUp != StallReason::NoStall) hops++;
    if (in.decodeUp != StallReason::NoStall) hops++;
    if (in.fetchUp != StallReason::NoStall) hops++;
    return hops;
}

/** The single non-silent chain hop reason (call only when hops == 1). */
StallReason
soleChainCause(const DispatchCycleInputs &in)
{
    if (in.renameUp != StallReason::NoStall) return in.renameUp;
    if (in.decodeUp != StallReason::NoStall) return in.decodeUp;
    return in.fetchUp;
}

const char *const kSlotClassNames[] = {
    "frontend", "backend", "recovery", "drain", "unknown"};

} // anonymous namespace

const char *
settleOriginName(SettleOrigin origin)
{
    static const char *const names[] = {
        "Retire", "CommitBelt", "PreCutBelt", "RobWalk", "IQ", "LSQ",
        "ExecSkip", "DispQue", "BufferSquash"};
    const unsigned idx = static_cast<unsigned>(origin);
    assert(idx < static_cast<unsigned>(SettleOrigin::NumSettleOrigins));
    return names[idx];
}

namespace
{

const char *const kPatternXNames[] = {
    "fullwidth",   // 0: budget fully allocated, no unused slots
    "drain",       // 1: P1 drain fired
    "admBE",       // 2: P2 admission-Backend fired (or popped BE admission)
    "admBS",       // 3: P3 admission-BS fired (or popped squash evidence)
    "chainOrBreak",// 4: P4 unique chain or section 1.5d break mapping
    "silentFE",    // 5: P5 silent chain + ready admission
    "ambiguous",   // 6: P6 / unsigned break / popped-unknown
    "other"        // 7: reserved
};

} // anonymous namespace

bool
DispatchCycleInputs::operator<(const DispatchCycleInputs &o) const
{
    if (budget != o.budget) return budget < o.budget;
    if (residueAtStart != o.residueAtStart)
        return residueAtStart < o.residueAtStart;
    if (arrivals != o.arrivals) return arrivals < o.arrivals;
    if (crossings != o.crossings) return crossings < o.crossings;
    if (poppedSquashed != o.poppedSquashed)
        return poppedSquashed < o.poppedSquashed;
    if (drainMode != o.drainMode) return !drainMode && o.drainMode;
    if (breakReason != o.breakReason) return breakReason < o.breakReason;
    if (blockIEW != o.blockIEW) return !blockIEW && o.blockIEW;
    if (iewBlockReason != o.iewBlockReason)
        return iewBlockReason < o.iewBlockReason;
    if (blockRename != o.blockRename) return !blockRename && o.blockRename;
    if (renameBlockReason != o.renameBlockReason)
        return renameBlockReason < o.renameBlockReason;
    if (renameUp != o.renameUp) return renameUp < o.renameUp;
    if (decodeUp != o.decodeUp) return decodeUp < o.decodeUp;
    if (fetchUp != o.fetchUp) return fetchUp < o.fetchUp;
    if (renameUpReasons != o.renameUpReasons)
        return renameUpReasons < o.renameUpReasons;
    if (decodeUpReasons != o.decodeUpReasons)
        return decodeUpReasons < o.decodeUpReasons;
    if (fetchUpReasons != o.fetchUpReasons)
        return fetchUpReasons < o.fetchUpReasons;
    if (commitSquashWire != o.commitSquashWire)
        return !commitSquashWire && o.commitSquashWire;
    if (robSquashingWire != o.robSquashingWire)
        return !robSquashingWire && o.robSquashingWire;
    return false;
}

ChainHopEvidence
foldChainHop(const std::vector<StallReason> &hop)
{
    ChainHopEvidence e;
    for (StallReason r : hop) {
        if (r != StallReason::NoStall) {
            assert(static_cast<unsigned>(r) <
                   static_cast<unsigned>(StallReason::NumStallReasons));
            e.reasonsMask |= 1ULL << static_cast<unsigned>(r);
        }
    }
    // Distinct-count via popcount: 0 silent, 1 sole reason, >=2 mixed.
    // Slot order never selects the representative (order-invariance is
    // the whole point: [IcacheStall, ROBFull] and its reverse fold
    // identically to the mixed sentinel).
    unsigned distinct = 0;
    uint64_t m = e.reasonsMask;
    while (m) {
        distinct += (m & 1u);
        m >>= 1;
    }
    if (distinct == 0) {
        e.representative = StallReason::NoStall;
    } else if (distinct == 1) {
        unsigned bit = 0;
        while (!(e.reasonsMask & (1ULL << bit)))
            bit++;
        e.representative = static_cast<StallReason>(bit);
    } else {
        e.representative = kChainMixedStall;
    }
    return e;
}

unsigned
SlotSplit::unusedTotal() const
{
    unsigned total = 0;
    for (size_t c = 0; c < static_cast<size_t>(SlotClass::NumSlotClasses); c++)
        total += counts[c];
    return total;
}

bool
SlotSplit::partitions(unsigned budget) const
{
    // counts[] are unsigned and therefore never negative; the partition
    // identity INV-4 is the exact sum check.
    return unusedTotal() + crossings == budget;
}

SlotClass
classifyStallReason(StallReason reason, bool upstreamRole)
{
    // The role only matters for NoStall (admission side: "no block, does
    // not participate"; chain side: P4/P5 count silent hops themselves).
    // BpStall only ever appears as chain evidence (fetch.cc doSquash sets
    // it); it maps to Recovery there (P4 causally-unique case) and is
    // never consumed unconditionally (contract section 1.5d v3 change).
    switch (reason) {
      case StallReason::NoStall:
        return SlotClass::NumSlotClasses;  // sentinel: not a classification

      // Frontend (section 1.5d)
      case StallReason::IcacheStall:
      case StallReason::ITlbStall:
      case StallReason::IntStall:
      case StallReason::TrapStall:
      case StallReason::FTQBubble:
      case StallReason::FetchFragStall:
      case StallReason::OtherFetchStall:
        return SlotClass::Frontend;

      // Backend (section 1.5d; SerializeStall deliberately Backend,
      // OtherFragStall deliberately Backend)
      case StallReason::DTlbStall:
      case StallReason::SerializeStall:
      case StallReason::ScalarLongExecute:
      case StallReason::VectorLongExecute:
      case StallReason::InstNotReady:
      case StallReason::LoadL1Bound:
      case StallReason::LoadL2Bound:
      case StallReason::LoadL3Bound:
      case StallReason::LoadMemBound:
      case StallReason::StoreL1Bound:
      case StallReason::StoreL2Bound:
      case StallReason::StoreL3Bound:
      case StallReason::StoreMemBound:
      case StallReason::MemNotReady:
      case StallReason::MemCommitRateLimit:
      case StallReason::Atomic:
      case StallReason::OtherMemStall:
      case StallReason::MemDQBandwidth:
      case StallReason::IntDQBandwidth:
      case StallReason::FVDQBandwidth:
      case StallReason::VectorReadyButNotIssued:
      case StallReason::ScalarReadyButNotIssued:
      case StallReason::ResumeUnblock:
      case StallReason::ROBFull:
      case StallReason::RegFull:
      case StallReason::OtherStall:
      case StallReason::OtherFragStall:
        return SlotClass::Backend;

      // Recovery / bad-speculation family (section 1.5d).  BpStall is
      // included ONLY as chain-role evidence (R-16 removed it from the
      // unconditional Recovery row): upstreamRole=true maps it to
      // Recovery when P4 finds it causally unique.  In the admission
      // role (upstreamRole=false) it maps to Unknown -- an
      // admission-side BpStall is unclassified evidence and must never
      // trigger P3 or sneak into P2 (CORE_REVIEW F2).
      case StallReason::SquashStall:
      case StallReason::InstMisPred:
      case StallReason::InstSquashed:
      case StallReason::CommitSquash:
        return SlotClass::Recovery;
      case StallReason::BpStall:
        return upstreamRole ? SlotClass::Recovery : SlotClass::Unknown;

      // Unknown (never-used values and any out-of-table future value)
      case StallReason::FetchBufferInvalid:
      case StallReason::MemSquashed:
      default:
        return SlotClass::Unknown;
    }
}

SlotSplit
classifyDispatchCycle(const DispatchCycleInputs &in)
{
    SlotSplit split;
    split.crossings = in.crossings;

    const int delivered =
        static_cast<int>(in.residueAtStart) + static_cast<int>(in.arrivals);

    // INV-0 / INV-0b input guards (contract section 1.2): delivered <=
    // budget and crossings <= delivered are hook contracts enforced at
    // runtime; violations flag the cycle (budgetOverrun) and invalidate
    // the ledger.  No reclassification or width adjustment may mask them.
    if (delivered < 0 ||
        static_cast<int>(in.crossings) > delivered ||
        delivered > static_cast<int>(in.budget)) {
        split.budgetViolation = true;
    }

    // Additive terms (section 1.5 identity).  Clamped for counting; a
    // negative term also implies a guard violation.
    const int breakBlocked =
        delivered - static_cast<int>(in.crossings) -
        static_cast<int>(in.poppedSquashed);
    const int upstreamShort = static_cast<int>(in.budget) - delivered;
    if (breakBlocked < 0 || upstreamShort < 0)
        split.budgetViolation = true;

    split.breakBlockedSlots = toUnsigned(breakBlocked);
    split.poppedSquashedSlots = toUnsigned(in.poppedSquashed);
    split.upstreamShortSlots = toUnsigned(upstreamShort);

    // --- breakBlocked term: section 1.5d mapping of the break reason ---
    if (split.breakBlockedSlots > 0) {
        if (in.breakReason == StallReason::NoStall) {
            // Defensive: slots withheld by a break whose reason was not
            // sampled.  Honest destination is Unknown (no reclassification
            // to force a nicer answer).
            split.breakRule = TermRule::BreakUnsignaled;
            split.counts[static_cast<size_t>(SlotClass::Unknown)] +=
                split.breakBlockedSlots;
        } else {
            split.breakRule = TermRule::BreakReasonMap;
            const SlotClass cls = classifyStallReason(in.breakReason, false);
            const size_t idx = cls == SlotClass::NumSlotClasses
                ? static_cast<size_t>(SlotClass::Unknown)
                : static_cast<size_t>(cls);
            split.counts[idx] += split.breakBlockedSlots;
        }
    }

    // --- poppedSquashed term (section 1.5b) ---
    if (split.poppedSquashedSlots > 0) {
        if (admissionBackendBlocked(in)) {
            split.poppedRule = TermRule::PoppedBEAdmission;
            split.counts[static_cast<size_t>(SlotClass::Backend)] +=
                split.poppedSquashedSlots;
        } else if (squashSceneEvidence(in)) {
            split.poppedRule = TermRule::PoppedSquashEvidence;
            split.counts[static_cast<size_t>(SlotClass::Recovery)] +=
                split.poppedSquashedSlots;
        } else {
            split.poppedRule = TermRule::PoppedUnknown;
            split.counts[static_cast<size_t>(SlotClass::Unknown)] +=
                split.poppedSquashedSlots;
        }
    }

    // --- upstreamShort term: policy P1..P6 (section 1.5b) ---
    if (split.upstreamShortSlots > 0) {
        if (in.drainMode) {
            // P1: same-cycle direct evidence.
            split.upstreamRule = TermRule::P1Drain;
            split.counts[static_cast<size_t>(SlotClass::Drain)] +=
                split.upstreamShortSlots;
        } else if (admissionBackendBlocked(in)) {
            // P2: consumer side provably refused this cycle; the shortage
            // is not attributed upstream (owner-frozen priority: Backend
            // admission block outranks Recovery).
            split.upstreamRule = TermRule::P2AdmissionBE;
            split.counts[static_cast<size_t>(SlotClass::Backend)] +=
                split.upstreamShortSlots;
        } else if (admissionRecoveryBlocked(in)) {
            // P3: allocator stalled by squash recovery (TMA-aligned
            // "allocator stalled due to recovery").
            split.upstreamRule = TermRule::P3AdmissionBS;
            split.counts[static_cast<size_t>(SlotClass::Recovery)] +=
                split.upstreamShortSlots;
        } else if (admissionUnclassifiedBlocked(in)) {
            // Admission block present but its reason is neither Backend
            // nor BS family (admission-role BpStall, out-of-table value,
            // or unsampled NoStall): the consumer provably refused, so
            // P5's silent-ready premise does NOT hold and upstream
            // attribution has no causally unique chain evidence either
            // -- honest destination Unknown (no reclassification to FE).
            split.upstreamRule = TermRule::AdmissionUnclassified;
            split.counts[static_cast<size_t>(SlotClass::Unknown)] +=
                split.upstreamShortSlots;
        } else {
            const unsigned hops = chainNonSilentHops(in);
            const bool anyHopMixed =
                in.renameUp == kChainMixedStall ||
                in.decodeUp == kChainMixedStall ||
                in.fetchUp == kChainMixedStall;
            if (anyHopMixed) {
                // Same-hop heterogeneous vector folded to the mixed
                // sentinel (foldChainHop): ambiguous evidence even when
                // only one hop is non-silent -- the frozen ambiguity
                // policy (P6 family) applies, Unknown.  Never the first
                // non-NoStall entry (slot order must not pick FE/BE).
                split.upstreamRule = TermRule::P4MixedHop;
                split.counts[static_cast<size_t>(SlotClass::Unknown)] +=
                    split.upstreamShortSlots;
            } else if (hops == 1) {
                // P4: causally unique lagged-chain cause.
                split.upstreamRule = TermRule::P4UniqueChain;
                const SlotClass cls =
                    classifyStallReason(soleChainCause(in), true);
                const size_t idx = cls == SlotClass::NumSlotClasses
                    ? static_cast<size_t>(SlotClass::Unknown)
                    : static_cast<size_t>(cls);
                split.counts[idx] += split.upstreamShortSlots;
            } else if (hops == 0) {
                // P5 (owner-frozen policy, not a theorem): chain silent,
                // admission provably ready, delivered < W -> the residual
                // explanation is "frontend did not supply".
                split.upstreamRule = TermRule::P5SilentFE;
                split.counts[static_cast<size_t>(SlotClass::Frontend)] +=
                    split.upstreamShortSlots;
            } else {
                // P6: multi-hop mixed/contradictory chain evidence.
                split.upstreamRule = TermRule::P6Ambiguous;
                split.counts[static_cast<size_t>(SlotClass::Unknown)] +=
                split.upstreamShortSlots;
            }
        }
    }

    return split;
}

PatternBin
patternBin(const DispatchCycleInputs &inputs, const SlotSplit &split)
{
    PatternBin bin;
    if (split.unusedTotal() == 0) {
        bin.x = 0;
        bin.y = 0;
        return bin;
    }
    if (split.counts[static_cast<size_t>(SlotClass::Drain)] > 0) {
        bin.x = 1;
    } else if (split.upstreamRule == TermRule::P2AdmissionBE ||
               split.poppedRule == TermRule::PoppedBEAdmission) {
        bin.x = 2;
    } else if (split.upstreamRule == TermRule::P3AdmissionBS ||
               split.poppedRule == TermRule::PoppedSquashEvidence) {
        bin.x = 3;
    } else if (split.upstreamRule == TermRule::P4UniqueChain ||
               split.breakRule == TermRule::BreakReasonMap) {
        bin.x = 4;
    } else if (split.upstreamRule == TermRule::P5SilentFE) {
        bin.x = 5;
    } else if (split.upstreamRule == TermRule::P6Ambiguous ||
               split.upstreamRule == TermRule::P4MixedHop ||
               split.upstreamRule == TermRule::AdmissionUnclassified ||
               split.poppedRule == TermRule::PoppedUnknown ||
               split.breakRule == TermRule::BreakUnsignaled) {
        bin.x = 6;
    } else {
        bin.x = 7;
    }

    // Dominant class with deterministic tie-breaking (lowest enum first).
    size_t best = 0;
    for (size_t c = 1; c < static_cast<size_t>(SlotClass::NumSlotClasses);
         c++) {
        if (split.counts[c] > split.counts[best])
            best = c;
    }
    bin.y = static_cast<unsigned>(best);
    return bin;
}

double
guardedRatio(double numerator, double denominator, LedgerRunState state)
{
    if (state != LedgerRunState::Valid)
        return kNaN;
    if (denominator == 0.0)
        return kNaN;
    return numerator / denominator;
}

// ---------------------------------------------------------------------------
// TopdownLedger
// ---------------------------------------------------------------------------

TopdownLedger::SnapshotStats::SnapshotStats(statistics::Group *parent)
    : statistics::Group(parent, "snapshot"),
      ADD_STAT(epoch, statistics::units::Count::get(),
               "Epoch stamp of the ROI-end end-of-tick snapshot (Option B)"),
      ADD_STAT(allocated, statistics::units::Count::get(),
               "C4 registrations of the segment at snapshot time"),
      ADD_STAT(retiredCohort, statistics::units::Count::get(),
               "Retired members of the segment cohort at snapshot time"),
      ADD_STAT(squashedCohort, statistics::units::Count::get(),
               "Squashed members of the segment cohort at snapshot time"),
      ADD_STAT(liveAtClose, statistics::units::Count::get(),
               "Live members of the segment cohort at snapshot time"),
      ADD_STAT(liveCapacityBound, statistics::units::Count::get(),
               "Configured ROB capacity used as the Live necessary-condition "
               "bound (liveOverCapacity guard)"),
      ADD_STAT(slotBudget, statistics::units::Count::get(),
               "W_C4 x cycles accrued at snapshot time"),
      ADD_STAT(feSlots, statistics::units::Count::get(),
               "Frontend-attributed unused slots at snapshot time"),
      ADD_STAT(beSlots, statistics::units::Count::get(),
               "Backend-attributed unused slots at snapshot time"),
      ADD_STAT(recoverySlots, statistics::units::Count::get(),
               "Recovery-attributed unused slots at snapshot time"),
      ADD_STAT(drainSlots, statistics::units::Count::get(),
               "Drain-attributed unused slots at snapshot time"),
      ADD_STAT(unknownSlots, statistics::units::Count::get(),
               "Unknown-attributed unused slots at snapshot time")
{
}

TopdownLedger::TopdownLedger(statistics::Group *parent, const Config &cfg,
                             bool enabled)
    : statistics::Group(parent, "topdownV2"),
      snapshotStats(this),
      cfg_(cfg),
      enabled_(enabled),
      ADD_STAT(statEnabled, statistics::units::Count::get(),
               "1 when enable_d4b_topdown_ledger is on (self-describing "
               "quiescent state: a disabled ledger prints 0/NaN everywhere "
               "while ledgerState reads 0)"),
      ADD_STAT(ledgerState, statistics::units::Count::get(),
               "0=valid, 1=unsupported (SMT/enableDispatchStage/width "
               "domain), 2=invalid (guard violation). Authoritative: any "
               "numerical acceptance requires state==0"),
      ADD_STAT(slotWidth, statistics::units::Count::get(),
               "Configured W_C4 slot budget per cycle (renameWidth; "
               "reported as configured, never rewritten to force output)"),
      ADD_STAT(slotBudget, statistics::units::Count::get(),
               "C4 slot budget accrued (W_C4 x cycles)"),
      ADD_STAT(allocated, statistics::units::Count::get(),
               "C4 registrations (dispatch events) of the segment cohort"),
      ADD_STAT(retiredCohort, statistics::units::Count::get(),
               "Retired members of the segment cohort (settlement routed by "
               "registration epoch stamp)"),
      ADD_STAT(squashedCohort, statistics::units::Count::get(),
               "Squashed members of the segment cohort"),
      ADD_STAT(liveAtClose, statistics::units::Count::get(),
               "Live (in-flight) members of the current-epoch cohort"),
      ADD_STAT(squashedInIQ, statistics::units::Count::get(),
               "L2 split: cohort squashes observed at IQ/LSQ/execute-side "
               "points"),
      ADD_STAT(squashedAtCommit, statistics::units::Count::get(),
               "L2 split: cohort squashes observed at commit-side points "
               "(drainSquashedHead belt + ROB walk)"),
      ADD_STAT(feSlots, statistics::units::Count::get(),
               "Unused C4 slots attributed Frontend"),
      ADD_STAT(beSlots, statistics::units::Count::get(),
               "Unused C4 slots attributed Backend"),
      ADD_STAT(recoverySlots, statistics::units::Count::get(),
               "Unused C4 slots attributed Recovery"),
      ADD_STAT(drainSlots, statistics::units::Count::get(),
               "Unused C4 slots attributed Drain (ROI drain mode)"),
      ADD_STAT(unknownSlots, statistics::units::Count::get(),
               "Unused C4 slots attributed Unknown (advisory ratio only; "
               "never a gate)"),
      ADD_STAT(slotPatternDist, statistics::units::Count::get(),
               "<=64-bin signal-mode x dominant-class cycle histogram "
               "(summary; the lossless replay input is the in-memory full "
               "tuple table, see replayComplete)"),
      ADD_STAT(settledAfterCloseRetired, statistics::units::Count::get(),
               "Post-snapshot retire settlements of snapshot-Live members"),
      ADD_STAT(settledAfterCloseSquashed, statistics::units::Count::get(),
               "Post-snapshot squash settlements of snapshot-Live members"),
      ADD_STAT(settledOutOfEpochRetired, statistics::units::Count::get(),
               "Retire settlements of pre-segment registrations (PreIn in "
               "identity E2; B2 material)"),
      ADD_STAT(settledOutOfEpochSquashed, statistics::units::Count::get(),
               "Squash settlements of pre-segment registrations"),
      ADD_STAT(postSnapshotRegistrations, statistics::units::Count::get(),
               "Registration attempts after the snapshot (frozen: counted "
               "only, never become Live)"),
      ADD_STAT(budgetOverrun, statistics::units::Cycle::get(),
               "Cycles failing the INV-0/0b delivered<=budget / "
               "crossings<=delivered hook contract (invalidating)"),
      ADD_STAT(budgetOverrunSlots, statistics::units::Count::get(),
               "Slot units of INV-0/0b violations"),
      ADD_STAT(registeredNotInROB, statistics::units::Count::get(),
               "C4 registrations where the instruction was not yet in the "
               "ROB (INV-6; expected 0; invalidating)"),
      ADD_STAT(liveOverCapacity, statistics::units::Count::get(),
               "Snapshots where Live exceeded the configured ROB capacity "
               "(necessary-condition guard; invalidating)"),
      ADD_STAT(identityViolations, statistics::units::Count::get(),
               "INV-4 partition / crossing-consistency violations "
               "(invalidating)"),
      ADD_STAT(allocatedIdentityViolations, statistics::units::Count::get(),
               "INV-1 re-registration / pointer-recycling detections "
               "(invalidating)"),
      ADD_STAT(configUnsupportedSMT, statistics::units::Count::get(),
               "numThreads>1 at construction (unsupported domain)"),
      ADD_STAT(configUnsupportedDispatchStage, statistics::units::Count::get(),
               "enableDispatchStage=true at construction (unsupported "
               "domain: second registration point unaudited)"),
      ADD_STAT(configUnsupportedWidth, statistics::units::Count::get(),
               "decodeWidth != renameWidth at construction (W_C4 support "
               "domain requires re-derivation)"),
      ADD_STAT(liveAgeMax, statistics::units::Tick::get(),
               "Age in ticks of the oldest Live member at snapshot time "
               "(tail-anomaly visibility)"),
      ADD_STAT(replayCompleteStat, statistics::units::Count::get(),
               "1 while every classified cycle is present in the lossless "
               "tuple table AND the ledger is collecting (statEnabled=1); "
               "0 for a disabled ledger (no table exists) and 0 after a "
               "resource-cap drop"),
      ADD_STAT(replayDistinctTuples, statistics::units::Count::get(),
               "Distinct full evidence tuples retained in the lossless "
               "table"),
      ADD_STAT(replayTableOverflowTuples, statistics::units::Count::get(),
               "Cycle records dropped after the tuple-table resource cap "
               "was reached (no silent truncation: replayComplete goes 0)"),
    ADD_STAT(registerHookCalls, statistics::units::Count::get(),
               "Lifecycle observation: registerAlloc hook entries while "
               "collecting (frozen live-group semantics: stops growing "
               "after the snapshot; post-snapshot attempts are counted in "
               "postSnapshotRegistrations only)"),
    ADD_STAT(settleRetiredHookCalls, statistics::units::Count::get(),
               "Lifecycle observation: settleRetired hook entries while "
               "collecting"),
    ADD_STAT(settleSquashedHookCalls, statistics::units::Count::get(),
               "Lifecycle observation: settleSquashed hook entries while "
               "collecting"),
    ADD_STAT(settleRetiredEffective, statistics::units::Count::get(),
               "Lifecycle observation: settleRetired hooks that transitioned "
               "a Live member (== retiredCohort + settledOutOfEpochRetired "
               "+ settledAfterCloseRetired)"),
    ADD_STAT(settleSquashedEffective, statistics::units::Count::get(),
               "Lifecycle observation: settleSquashed hooks that transitioned "
               "a Live member (== squashedCohort + settledOutOfEpochSquashed "
               "+ settledAfterCloseSquashed)"),
    ADD_STAT(settleRetiredAbsorbed, statistics::units::Count::get(),
               "Lifecycle observation: settleRetired hooks absorbed by the "
               "idempotence rule (tag not Live at the hook)"),
    ADD_STAT(settleSquashedAbsorbed, statistics::units::Count::get(),
               "Lifecycle observation: settleSquashed hooks absorbed by the "
               "idempotence rule (tag not Live at the hook); natural "
               "duplicate squash notifications (S16) land here"),
    ADD_STAT(settleAbsorbedUnallocated, statistics::units::Count::get(),
               "Lifecycle observation: absorbed settles whose tag was never "
               "registered (pre-C4 population drift-protection no-ops, e.g. "
               "the pre-cut belt hooks)"),
    ADD_STAT(settleAbsorbedRetired, statistics::units::Count::get(),
               "Lifecycle observation: absorbed settles whose tag was "
               "already Retired (duplicate retire notifications)"),
    ADD_STAT(settleAbsorbedSquashed, statistics::units::Count::get(),
               "Lifecycle observation: absorbed settles whose tag was "
               "already Squashed (duplicate squash notifications, S16)"),
    ADD_STAT(settleOriginEffective, statistics::units::Count::get(),
               "Lifecycle observation (S18 class x origin matrix): effective "
               "in-segment squashed settles per observation point; total == "
               "squashedCohort"),
    ADD_STAT(settleOriginOutOfEpoch, statistics::units::Count::get(),
               "Lifecycle observation: effective squashed settles per "
               "observation point routed out-of-epoch or after-close; "
               "total == settledOutOfEpochSquashed + "
               "settledAfterCloseSquashed"),
    ADD_STAT(settleOriginAbsorbed, statistics::units::Count::get(),
               "Lifecycle observation: absorbed settleSquashed hooks per "
               "observation point; total == settleSquashedAbsorbed"),
    ADD_STAT(nestedSquashObservations, statistics::units::Count::get(),
               "Lifecycle observation (S7): squash requests accepted by "
               "commit while a previous ROB squash walk was still in "
               "progress (commitStatus==ROBSquashing at arbitration)"),
    ADD_STAT(snapshotBoundaryTick, statistics::units::Tick::get(),
               "S22b boundary sample: tick of the Option-B end-of-tick "
               "snapshot (0 = no snapshot this segment)"),
    ADD_STAT(snapshotTickRegistrations, statistics::units::Count::get(),
               "S22b boundary sample: C4 registrations during the snapshot "
               "tick (all land inside the frozen segment)"),
    ADD_STAT(snapshotTickSettlesPre, statistics::units::Count::get(),
               "S22b boundary sample: effective settles during the snapshot "
               "tick before the snapshot call (all land inside the frozen "
               "segment)"),
    ADD_STAT(snapshotTickSettlesPost, statistics::units::Count::get(),
               "S22b boundary sample: effective settles after the snapshot "
               "call within the same tick (structurally 0: the snapshot "
               "block runs after every stage of that tick)"),
      ADD_STAT(retiring, statistics::units::Ratio::get(),
               "topdownV2 L1: retiredCohort / slotBudget (allocation-cohort "
               "semantics, TMA-aligned naming only)"),
      ADD_STAT(badSpec, statistics::units::Ratio::get(),
               "topdownV2 L1: (squashedCohort + recoverySlots) / slotBudget"),
      ADD_STAT(frontend, statistics::units::Ratio::get(),
               "topdownV2 L1: feSlots / slotBudget"),
      ADD_STAT(backend, statistics::units::Ratio::get(),
               "topdownV2 L1: beSlots / slotBudget (directly measured, NOT "
               "a residual)"),
      ADD_STAT(boundary, statistics::units::Ratio::get(),
               "topdownV2 L1: (drainSlots + liveAtClose) / slotBudget "
               "(explicit edge term; no TMA counterpart)"),
      ADD_STAT(unknown, statistics::units::Ratio::get(),
               "topdownV2 L1: unknownSlots / slotBudget (advisory only; "
               "never a gate)"),
      ADD_STAT(sumCheck, statistics::units::Ratio::get(),
               "Representation invariant: sum of the six L1 ratios (1 when "
               "valid). Bookkeeping self-consistency, NOT causal proof.")
{
    slotPatternDist
        .init(8, 8)
        .flags(statistics::total);

    for (unsigned x = 0; x < 8; x++)
        slotPatternDist.subname(x, kPatternXNames[x]);

    const unsigned kNumOrigins =
        static_cast<unsigned>(SettleOrigin::NumSettleOrigins);
    settleOriginEffective.init(kNumOrigins);
    settleOriginOutOfEpoch.init(kNumOrigins);
    settleOriginAbsorbed.init(kNumOrigins);
    for (unsigned o = 0; o < kNumOrigins; o++) {
        const char *nm = settleOriginName(static_cast<SettleOrigin>(o));
        settleOriginEffective.subname(o, nm);
        settleOriginOutOfEpoch.subname(o, nm);
        settleOriginAbsorbed.subname(o, nm);
    }
    for (unsigned y = 0; y < 5; y++)
        slotPatternDist.ysubname(y, kSlotClassNames[y]);
    slotPatternDist.ysubname(5, "reserved0");
    slotPatternDist.ysubname(6, "reserved1");
    slotPatternDist.ysubname(7, "reserved2");

    // Supported-domain detection (contract section 1.6, owner-frozen D-2):
    // single thread, same-width, dispatch-stage-off.  Unsupported is an
    // explicit state with NaN ratios, never silent zeroing.
    if (cfg_.numThreads > 1) {
        unsupportedConfig_ = true;
        configUnsupportedSMT = 1;
    }
    if (cfg_.enableDispatchStage) {
        unsupportedConfig_ = true;
        configUnsupportedDispatchStage = 1;
    }
    if (!cfg_.upstreamWidthMatches) {
        unsupportedConfig_ = true;
        configUnsupportedWidth = 1;
    }

    statEnabled = enabled_ ? 1 : 0;
    slotWidth = cfg_.slotWidth;
    // FAIL-1 fix (acceptance card): the off arm must print 0/NaN for the
    // whole topdownV2 group.  replayComplete is a collection-completeness
    // self-report: 1 only for a LEDGER THAT IS COLLECTING with every
    // classified cycle retained (an empty enabled table is complete).
    // A disabled ledger has no table, so it reports 0 ("not
    // replay-complete"), never 1; the enabled state is carried by
    // statEnabled.  resetStats()/beginEpoch() re-stamp identically.
    replayComplete_ = enabled_;
    replayCompleteStat = enabled_ ? 1 : 0;
    setLedgerStateStat();
    refreshRatios();

    // Dump-time advisory checks (contract section 1.5f): sumCheck drift
    // is a construction-break alarm; the Unknown ratio is advisory and
    // NEVER a gate.  One-shot warnings only; the disabled ledger stays
    // silent (A/B off-arm stderr must not move).  The same callback
    // drives the persistent audit export (fail-fast on I/O errors).
    statistics::registerDumpCallback([this]() {
        checkAdvisories();
        exportAudit();
    });
}

LedgerRunState
TopdownLedger::runState() const
{
    if (invalidated_)
        return LedgerRunState::Invalid;
    if (unsupportedConfig_)
        return LedgerRunState::Unsupported;
    return LedgerRunState::Valid;
}

void
TopdownLedger::setLedgerStateStat()
{
    ledgerState = static_cast<statistics::Counter>(runState());
}

void
TopdownLedger::refreshRatios()
{
    if (!enabled_) {
        // Quiescent (off-arm): every ratio is an explicit quiet NaN — the
        // ledger never outputs 0 or inf to masquerade as a legal ratio.
        retiring = kNaN;
        badSpec = kNaN;
        frontend = kNaN;
        backend = kNaN;
        boundary = kNaN;
        unknown = kNaN;
        sumCheck = kNaN;
        return;
    }

    const LedgerRunState st = runState();
    const double budget = slotBudget.value();
    retiring = guardedRatio(retiredCohort.value(), budget, st);
    badSpec = guardedRatio(squashedCohort.value() + recoverySlots.value(),
                           budget, st);
    frontend = guardedRatio(feSlots.value(), budget, st);
    backend = guardedRatio(beSlots.value(), budget, st);
    boundary = guardedRatio(drainSlots.value() + liveAtClose.value(),
                            budget, st);
    unknown = guardedRatio(unknownSlots.value(), budget, st);
    if (st == LedgerRunState::Valid) {
        sumCheck = retiring.value() + badSpec.value() + frontend.value() +
                   backend.value() + boundary.value() + unknown.value();
    } else {
        sumCheck = kNaN;
    }
}

void
TopdownLedger::checkAdvisories()
{
    if (!enabled_)
        return;

    const double sc = sumCheck.value();
    if (!warnedSumCheck_ && runState() == LedgerRunState::Valid &&
        !std::isnan(sc) && std::fabs(sc - 1.0) > 1e-9) {
        warnedSumCheck_ = true;
        warn("topdownV2: sumCheck = %.12f deviates from 1 by more than "
             "1e-9 — identity construction is broken (do NOT clamp or "
             "reclassify to mask it)\n", sc);
    }

    // Unknown-ratio advisory (contract section 1.5f: advisory only, never
    // a gate; the threshold is fixed at 1% pending an owner knob).
    if (!warnedUnknown_ && runState() == LedgerRunState::Valid &&
        slotBudget.value() > 0) {
        const double ur = unknownSlots.value() / slotBudget.value();
        if (ur > 0.01) {
            warnedUnknown_ = true;
            warn("topdownV2: unknownSlots ratio %.4f exceeds the 1%% "
                 "advisory threshold (implementation-quality hint only; "
                 "classification policy must NOT be tuned to reduce it)\n",
                 ur);
        }
    }
}

void
TopdownLedger::invalidate()
{
    invalidated_ = true;
    setLedgerStateStat();
    refreshRatios();
}

void
TopdownLedger::observeTick(uint64_t whenTick)
{
    if (!enabled_)
        return;
    // S22b scratch: the per-tick window starts at the CPU tick boundary
    // (before the commit phase); the snapshot call at the end of the
    // same tick reads it.  Pure observation: no accounting state changes.
    if (!haveObservedTick_ || whenTick != observedTick_) {
        observedTick_ = whenTick;
        haveObservedTick_ = true;
        tickRegistrations_ = 0;
        tickSettlesPre_ = 0;
    }
}

void
TopdownLedger::noteNestedSquash()
{
    if (!enabled_ || frozen_)
        return;
    nestedSquashObservations++;
}

void
TopdownLedger::registerAlloc(LedgerTag &tag, bool inROB, const void *instId,
                             uint64_t whenTick)
{
    if (!enabled_)
        return;

    if (!frozen_) {
        registerHookCalls++;
        if (observedTick_ == whenTick && haveObservedTick_)
            tickRegistrations_++;
    }

    if (frozen_) {
        // Contract section 1.2b(d): post-snapshot registrations never
        // change DynInst state and never join the snapshot cohort.
        postSnapshotRegistrations++;
        return;
    }

    if (tag.state != LedgerState::Unallocated ||
        liveMap_.find(instId) != liveMap_.end()) {
        // INV-1 runtime recheck (double registration or pointer recycling
        // of a still-Live member).
        allocatedIdentityViolations++;
        invalidate();
        return;
    }

    if (!inROB) {
        // INV-6 hook contract: registration implies same-tick ROB
        // residency (commit-side insert runs before IEW dispatch).
        registeredNotInROB++;
        invalidate();
    }

    tag.state = LedgerState::Live;
    tag.epoch = currentEpoch_;
    liveMap_[instId] = std::make_pair(currentEpoch_, whenTick);

    allocated++;
    liveAtClose++;
    registrationsSinceAccount_++;

    setLedgerStateStat();
}

bool
TopdownLedger::settle(LedgerTag &tag, const void *instId, bool retired,
                      SettleOrigin origin, uint64_t whenTick)
{
    if (!enabled_)
        return false;

    // Lifecycle observation (statistics-only; never a decision input).
    // Hook-entry counts and the S22b per-tick scratch run BEFORE the
    // idempotence gate so the production evidence can distinguish
    // effective settles from absorbed duplicates (S16) and from pre-C4
    // drift-protection no-ops.  The per-origin/absorbed vectors and the
    // hook/effective/absorbed scalars respect the frozen live-group
    // discipline: after the snapshot only the registered may-grow
    // diagnostics (settledAfterClose*/settledOutOfEpoch*/
    // postSnapshotRegistrations) keep growing.
    const unsigned oidx = static_cast<unsigned>(origin);
    if (!frozen_) {
        if (retired)
            settleRetiredHookCalls++;
        else
            settleSquashedHookCalls++;
        if (observedTick_ == whenTick && haveObservedTick_)
            tickSettlesPre_++;
    }

    // Idempotence (INV-2): only a Live instruction settles, exactly once.
    if (tag.state != LedgerState::Live) {
        if (!frozen_) {
            if (retired)
                settleRetiredAbsorbed++;
            else
                settleSquashedAbsorbed++;
            switch (tag.state) {
            case LedgerState::Unallocated:
                settleAbsorbedUnallocated++;
                break;
            case LedgerState::Retired:
                settleAbsorbedRetired++;
                break;
            case LedgerState::Squashed:
                settleAbsorbedSquashed++;
                break;
            default:
                break;
            }
            if (!retired)
                settleOriginAbsorbed[oidx]++;
        }
        return false;
    }

    const bool currentEpochInst = (tag.epoch == currentEpoch_);

    if (frozen_) {
        // Routing table (contract section 1.2b(d)): epoch stamp decides
        // first.  Only snapshot-epoch Live members feed settledAfterClose.
        if (tag.epoch == snapshotEpoch_) {
            if (retired)
                settledAfterCloseRetired++;
            else
                settledAfterCloseSquashed++;
        } else {
            if (retired)
                settledOutOfEpochRetired++;
            else
                settledOutOfEpochSquashed++;
        }
    } else if (currentEpochInst) {
        // In-segment cohort settlement: the live group updates.
        if (retired) {
            retiredCohort++;
            settleRetiredEffective++;
        } else {
            squashedCohort++;
            settleOriginEffective[oidx]++;
            const bool iqSide = (origin == SettleOrigin::IQ ||
                                origin == SettleOrigin::LSQ ||
                                origin == SettleOrigin::ExecSkip ||
                                origin == SettleOrigin::DispQue);
            if (iqSide)
                squashedInIQ++;
            else
                squashedAtCommit++;
            settleSquashedEffective++;
        }
        liveAtClose--;
    } else {
        // Pre-segment registration settling inside the segment (INV-5).
        if (retired)
            settledOutOfEpochRetired++;
        else
            settledOutOfEpochSquashed++;
        if (retired)
            settleRetiredEffective++;
    }
    if (!retired && !currentEpochInst) {
        // Effective squashed settle routed out-of-epoch/after-close
        // (frozen or pre-segment): per-origin observation, frozen
        // live-group discipline.
        if (!frozen_)
            settleOriginOutOfEpoch[oidx]++;
        if (!frozen_)
            settleSquashedEffective++;
    }

    tag.state = retired ? LedgerState::Retired : LedgerState::Squashed;
    liveMap_.erase(instId);
    return true;
}

void
TopdownLedger::settleRetired(LedgerTag &tag, const void *instId,
                             uint64_t whenTick)
{
    settle(tag, instId, true, SettleOrigin::Retire, whenTick);
}

void
TopdownLedger::settleSquashed(LedgerTag &tag, const void *instId,
                              SettleOrigin origin, uint64_t whenTick)
{
    settle(tag, instId, false, origin, whenTick);
}

void
TopdownLedger::recordCycle(const DispatchCycleInputs &inputs,
                           const SlotSplit &split)
{
    slotBudget += inputs.budget;
    feSlots += split.counts[static_cast<size_t>(SlotClass::Frontend)];
    beSlots += split.counts[static_cast<size_t>(SlotClass::Backend)];
    recoverySlots += split.counts[static_cast<size_t>(SlotClass::Recovery)];
    drainSlots += split.counts[static_cast<size_t>(SlotClass::Drain)];
    unknownSlots += split.counts[static_cast<size_t>(SlotClass::Unknown)];

    const PatternBin bin = patternBin(inputs, split);
    slotPatternDist[bin.x][bin.y]++;

    // Lossless audit table (contract section 1.5c): full evidence tuple ->
    // cycle count, per-term slot weights, per-class slot weights.  The
    // table is the replay input; slotPatternDist above is only a summary.
    auto it = patternTable_.find(inputs);
    if (it != patternTable_.end()) {
        PatternRecord &rec = it->second;
        rec.cycles++;
        rec.crossings += split.crossings;
        rec.breakBlockedSlots += split.breakBlockedSlots;
        rec.poppedSquashedSlots += split.poppedSquashedSlots;
        rec.upstreamShortSlots += split.upstreamShortSlots;
        for (size_t c = 0;
             c < static_cast<size_t>(SlotClass::NumSlotClasses); c++)
            rec.classSlots[c] += split.counts[c];
    } else if (patternTable_.size() < kPatternTableCap) {
        PatternRecord rec;
        rec.cycles = 1;
        rec.crossings = split.crossings;
        rec.breakBlockedSlots = split.breakBlockedSlots;
        rec.poppedSquashedSlots = split.poppedSquashedSlots;
        rec.upstreamShortSlots = split.upstreamShortSlots;
        for (size_t c = 0;
             c < static_cast<size_t>(SlotClass::NumSlotClasses); c++)
            rec.classSlots[c] = split.counts[c];
        patternTable_.emplace(inputs, rec);
        replayDistinctTuples = patternTable_.size();
    } else {
        // Explicit resource cap exceeded: mark and count, never silently
        // truncate and keep claiming replayability.
        replayComplete_ = false;
        replayCompleteStat = 0;
        replayTableOverflowTuples++;
    }
}

void
TopdownLedger::accountCycle(const DispatchCycleInputs &inputs)
{
    if (!enabled_ || frozen_)
        return;
    // Crossing cross-check: the caller's per-cycle crossing count must
    // equal the number of registrations since the last accountCycle.
    if (inputs.crossings != registrationsSinceAccount_) {
        identityViolations++;
        invalidate();
    }
    registrationsSinceAccount_ = 0;

    const SlotSplit split = classifyDispatchCycle(inputs);

    if (split.budgetViolation) {
        const int delivered =
            static_cast<int>(inputs.residueAtStart) +
            static_cast<int>(inputs.arrivals);
        unsigned over = 0;
        if (delivered > static_cast<int>(inputs.budget))
            over += static_cast<unsigned>(delivered - inputs.budget);
        if (static_cast<int>(inputs.crossings) > delivered)
            over += static_cast<unsigned>(
                static_cast<int>(inputs.crossings) - delivered);
        const int breakBlocked =
            delivered - static_cast<int>(inputs.crossings) -
            static_cast<int>(inputs.poppedSquashed);
        if (breakBlocked < 0)
            over += static_cast<unsigned>(-breakBlocked);
        budgetOverrun++;
        budgetOverrunSlots += over;
        invalidate();
    }

    // INV-4 partition assertion (bookkeeping identity, not causality).
    // Dormant-contract gap closed (CORE_REVIEW F5): a partition violation
    // is an invalidating guard exactly like the others (contract section
    // 1.6 names INV-4).  classifyDispatchCycle currently cannot produce
    // a partitions-violating split without also flagging budgetViolation,
    // so this branch is not independently reachable through production
    // inputs today -- it is still executed (and unit-tested via a real
    // compound-violation cycle) because future classification changes
    // must flip ledgerState, not just bump a counter.
    if (!split.partitions(inputs.budget)) {
        identityViolations++;
        invalidate();
    }

    recordCycle(inputs, split);

    refreshRatios();
}

void
TopdownLedger::setAuditExportPath(const std::string &path)
{
    auditExportPath_ = path;
}

bool
TopdownLedger::exportAudit() const
{
    if (auditExportPath_.empty() || !enabled_ || exportFailed_)
        return true;   // nothing configured / disabled / already fatal

    // -- freeze the current epoch's table at this dump point -------------
    // (const method: mutates only the mutable export cache below)
    std::ostringstream pat;
    bool first_pat = true;
    for (const auto &kv : patternTable_) {
        const DispatchCycleInputs &in = kv.first;
        const PatternRecord &rec = kv.second;
        if (!first_pat)
            pat << ",\n";
        first_pat = false;
        pat << "    {\"inputs\": {\"budget\": " << in.budget
            << ", \"residueAtStart\": " << in.residueAtStart
            << ", \"arrivals\": " << in.arrivals
            << ", \"crossings\": " << in.crossings
            << ", \"poppedSquashed\": " << in.poppedSquashed
            << ", \"drainMode\": " << (in.drainMode ? "true" : "false")
            << ", \"breakReason\": \"" << stallReasonName(in.breakReason) << "\""
            << ", \"blockIEW\": " << (in.blockIEW ? "true" : "false")
            << ", \"iewBlockReason\": \""
            << stallReasonName(in.iewBlockReason) << "\""
            << ", \"blockRename\": " << (in.blockRename ? "true" : "false")
            << ", \"renameBlockReason\": \""
            << stallReasonName(in.renameBlockReason) << "\""
            << ", \"renameUp\": \"" << stallReasonName(in.renameUp) << "\""
            << ", \"decodeUp\": \"" << stallReasonName(in.decodeUp) << "\""
            << ", \"fetchUp\": \"" << stallReasonName(in.fetchUp) << "\""
            << ", \"renameUpReasons\": "
            << reasonsMaskJson(in.renameUpReasons)
            << ", \"decodeUpReasons\": "
            << reasonsMaskJson(in.decodeUpReasons)
            << ", \"fetchUpReasons\": "
            << reasonsMaskJson(in.fetchUpReasons)
            << ", \"commitSquashWire\": "
            << (in.commitSquashWire ? "true" : "false")
            << ", \"robSquashingWire\": "
            << (in.robSquashingWire ? "true" : "false")
            << "}, \"count\": " << rec.cycles
            << ", \"slots\": {\"Frontend\": "
            << rec.classSlots[static_cast<size_t>(SlotClass::Frontend)]
            << ", \"Backend\": "
            << rec.classSlots[static_cast<size_t>(SlotClass::Backend)]
            << ", \"Recovery\": "
            << rec.classSlots[static_cast<size_t>(SlotClass::Recovery)]
            << ", \"Drain\": "
            << rec.classSlots[static_cast<size_t>(SlotClass::Drain)]
            << ", \"Unknown\": "
            << rec.classSlots[static_cast<size_t>(SlotClass::Unknown)]
            << "}, \"crossings\": " << rec.crossings << "}";
    }
    exportedEpochs_[currentEpoch_] = pat.str();
    exportedEpochDropped_[currentEpoch_] =
        replayTableOverflowTuples.value();

    // -- assemble header + all frozen epochs, write atomically -----------
    // Header completeness aggregates EVERY exported epoch (AND of the
    // per-epoch completeness), and dropped_patterns is the total across
    // epochs -- a stale per-epoch drop must never be reported as a
    // complete table at the top level.
    bool allComplete = replayComplete_;
    uint64_t totalDropped = 0;
    for (const auto &kv : exportedEpochDropped_) {
        totalDropped += kv.second;
        if (kv.second != 0)
            allComplete = false;
    }
    std::ostringstream out;
    out << "{\n"
        << "  \"schema_version\": \"" << kAuditSchemaVersion << "\",\n"
        << "  \"impl_schema_version\": \""
        << kAuditImplSchemaVersion << "\",\n"
        << "  \"policy_version\": \"" << kAuditPolicyVersion << "\",\n"
        << "  \"replay_complete\": "
        << (allComplete ? "true" : "false") << ",\n"
        << "  \"dropped_patterns\": " << totalDropped << ",\n"
        << "  \"resource_cap\": {\"max_patterns\": "
        << kPatternTableCap << "},\n"
        << "  \"current_epoch\": " << currentEpoch_ << ",\n"
        << "  \"epochs\": [\n";
    bool first_epoch = true;
    for (const auto &kv : exportedEpochs_) {
        if (!first_epoch)
            out << ",\n";
        first_epoch = false;
        const uint64_t dropped =
            exportedEpochDropped_.count(kv.first) ?
                exportedEpochDropped_.at(kv.first) : 0;
        const bool complete = dropped == 0;
        out << "    {\"epoch\": " << kv.first
            << ", \"replay_complete\": "
            << (complete ? "true" : "false")
            << ", \"dropped_patterns\": " << dropped
            << ", \"patterns\": [\n" << kv.second << "]}";
    }
    out << "]\n}\n";

    const std::string tmp = auditExportPath_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            exportFailed_ = true;
            fatal("topdownV2 audit export: cannot open %s\n", tmp);
        }
        f << out.str();
        f.flush();
        if (!f) {
            exportFailed_ = true;
            fatal("topdownV2 audit export: write/stream error on %s\n",
                  tmp);
        }
    }
    if (std::rename(tmp.c_str(), auditExportPath_.c_str()) != 0) {
        exportFailed_ = true;
        fatal("topdownV2 audit export: atomic rename %s -> %s failed\n",
              tmp, auditExportPath_);
    }
    return true;
}

void
TopdownLedger::beginEpoch()
{
    currentEpoch_++;
    frozen_ = false;
    registrationsSinceAccount_ = 0;
    // Segment-scoped audit table restarts with the segment.  A collecting
    // (enabled) ledger re-arms the completeness self-report; a disabled
    // ledger keeps replayComplete=0 (FAIL-1: off arm stays 0/NaN).
    patternTable_.clear();
    replayComplete_ = enabled_;
    replayCompleteStat = enabled_ ? 1 : 0;
}

void
TopdownLedger::snapshot(uint64_t whenTick)
{
    if (!enabled_ || frozen_)
        return;

    frozen_ = true;
    snapshotEpoch_ = currentEpoch_;

    snapshotStats.epoch = currentEpoch_;
    snapshotStats.allocated = allocated.value();
    snapshotStats.retiredCohort = retiredCohort.value();
    snapshotStats.squashedCohort = squashedCohort.value();
    snapshotStats.liveAtClose = liveAtClose.value();
    snapshotStats.liveCapacityBound = cfg_.robCapacity;
    snapshotStats.slotBudget = slotBudget.value();
    snapshotStats.feSlots = feSlots.value();
    snapshotStats.beSlots = beSlots.value();
    snapshotStats.recoverySlots = recoverySlots.value();
    snapshotStats.drainSlots = drainSlots.value();
    snapshotStats.unknownSlots = unknownSlots.value();

    // S22b boundary disambiguation sample (statistics-only, written once
    // here): the per-tick scratch was reset by observeTick() at the start
    // of this CPU tick and accumulated the commit-phase settles and
    // IEW-phase registrations of THIS tick, proving they land inside the
    // frozen segment.  No stage runs after this block within the same
    // tick, so the same-tick post-snapshot settle count is structurally 0;
    // post-snapshot settles appear in later ticks as settledAfterClose*.
    snapshotBoundaryTick = whenTick;
    snapshotTickRegistrations =
        (haveObservedTick_ && observedTick_ == whenTick) ?
        tickRegistrations_ : 0;
    snapshotTickSettlesPre =
        (haveObservedTick_ && observedTick_ == whenTick) ?
        tickSettlesPre_ : 0;
    snapshotTickSettlesPost = 0;

    // liveAgeMax: oldest live member of the snapshot cohort (in ticks).
    uint64_t oldestReg = whenTick;
    for (const auto &kv : liveMap_) {
        if (kv.second.first == currentEpoch_ &&
            kv.second.second < oldestReg)
            oldestReg = kv.second.second;
    }
    liveAgeMax = whenTick > oldestReg ? whenTick - oldestReg : 0;

    // Necessary-condition guard (a): Live <= configured ROB capacity.
    if (liveAtClose.value() > cfg_.robCapacity) {
        liveOverCapacity++;
        invalidate();
    }

    setLedgerStateStat();
    refreshRatios();
    checkAdvisories();
}

void
TopdownLedger::resetStats()
{
    // Default behaviour first: wipe the stat storages (including the
    // nested snapshot group).
    statistics::Group::resetStats();

    // ROI begin / statistics reset: open the next segment.
    beginEpoch();

    // Re-stamp configuration stats that the storage wipe cleared.
    // (beginEpoch(), which ran above, re-stamps replayCompleteStat:
    // 1 only for a collecting enabled ledger, 0 for a disabled one.)
    statEnabled = enabled_ ? 1 : 0;
    slotWidth = cfg_.slotWidth;
    configUnsupportedSMT = cfg_.numThreads > 1 ? 1 : 0;
    configUnsupportedDispatchStage = cfg_.enableDispatchStage ? 1 : 0;
    configUnsupportedWidth = cfg_.upstreamWidthMatches ? 0 : 1;
    replayDistinctTuples = 0;
    setLedgerStateStat();
    refreshRatios();
}

} // namespace o3
} // namespace gem5
