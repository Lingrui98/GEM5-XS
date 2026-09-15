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

#ifndef __CPU_O3_TOPDOWN_STATS_HELPERS_HH__
#define __CPU_O3_TOPDOWN_STATS_HELPERS_HH__

#include <cstdint>

namespace gem5
{
namespace o3
{
namespace tdstats
{

/**
 * @file topdown_stats_helpers.hh
 *
 * D4-A topdown statistics fixes: the DECISION LOGIC extracted from the
 * original inline sites so that the unit tests exercise the production
 * code directly (D4-B design contract section 5a: an inline copy model
 * is not a verification of production).  Every helper below is a
 * behavior-neutral extraction: the production call sites produce
 * bit-identical statistics, and the GTest (topdown_stats.test) links
 * THIS header instead of a duplicated model.
 *
 * Scope (see production/coverage/TEST_REFACTOR_SCOPE.md for the full
 * write-set analysis and the deliberately deferred parts):
 *   - R1  frontend-bubble attribution (fetch.cc measure* functions);
 *   - R3  predBtbHit snapshot predicate, decode selfSquash BTB-miss
 *         classification, per-tid redirect window machine;
 *   - R4  recovery window machine (same interval semantics as R3);
 *   - R7  memstall misslevel cumulative classification.
 * Deferred: R2/R6 (gem5 Formula-DSL composition sites) and R5
 * (event-site accounting discipline with no decision function).
 */

// ---------------------------------------------------------------------------
// R1 — cycle-level frontend-bubble attribution (fetch.cc)
// ---------------------------------------------------------------------------

/** Accounted delta for one cycle of frontend-bubble accounting. */
struct FrontendBubbleDelta
{
    long fetchBubbles = 0;    // unused decode slots charged
    long fetchBubblesMax = 0; // full-width bubble occurrences charged
};

/** Per-thread cycle state as seen by sendInstructionsToDecode. */
struct FrontendThreadCycleState
{
    bool blockFetch = false;   // backend can receive?
    bool robSquashing = false; // in squash recovery (fromCommit, delayed)?
};

/**
 * Per-thread accounting (Fetch::measureFrontendBubbles): gated on
 * !blockFetch && !robSquashing ("backend not stalled" TMA gate); charges
 * the unused slots and one full-width occurrence when nothing was
 * supplied.
 */
inline FrontendBubbleDelta
accountFrontendBubblesPerThread(const FrontendThreadCycleState &t,
                                unsigned insts_to_decode,
                                unsigned decode_width)
{
    FrontendBubbleDelta d;
    if (!t.blockFetch && !t.robSquashing) {
        const int unused_slots =
            static_cast<int>(decode_width) -
            static_cast<int>(insts_to_decode);
        if (unused_slots > 0) {
            d.fetchBubbles = unused_slots;
            if (unused_slots == static_cast<int>(decode_width)) {
                d.fetchBubblesMax = 1;
            }
        }
    }
    return d;
}

/**
 * Cycle-level aggregate for the empty-queue early-out path
 * (Fetch::measureFrontendBubblesEmptyQueueCycle): charges ONE
 * full-width bubble iff some thread is both unblocked and outside squash
 * recovery; returns after the first charge (never per thread).
 */
inline FrontendBubbleDelta
accountFrontendBubblesEmptyQueueCycle(const FrontendThreadCycleState *threads,
                                      unsigned num_threads,
                                      unsigned decode_width)
{
    for (unsigned i = 0; i < num_threads; ++i) {
        if (!threads[i].blockFetch && !threads[i].robSquashing) {
            return FrontendBubbleDelta{
                static_cast<long>(decode_width), 1};
        }
    }
    return FrontendBubbleDelta{0, 0};
}

// ---------------------------------------------------------------------------
// R3 — BTB-miss decode-redirect diagnostics
// ---------------------------------------------------------------------------

/**
 * The three Decode::selfSquash trigger points that can redirect fetch:
 *   1. readPredTaken() && !isControl()   (false hit);
 *   2. direct branch, target != predTarg (target mismatch);
 *   3. isReturn() && !readPredTaken()    (RAS fixup).
 */
enum class SelfSquashTrigger
{
    FalseHitNonControl,
    DirectTargetMismatch,
    ReturnRasFixup,
};

/**
 * Whether one decode selfSquash redirect counts as a BTB-miss resteer.
 * Triggers 2/3 count iff the instruction's fetch-time snapshot
 * (predBtbHit) says its PC hit no predBTBEntry; trigger 1 never counts
 * (readPredTaken implies the PC matched a taken predBTBEntry, so
 * predBtbHit is necessarily true there).
 */
inline bool
countsAsBtbMissResteer(SelfSquashTrigger trigger, bool pred_btb_hit)
{
    switch (trigger) {
    case SelfSquashTrigger::FalseHitNonControl:
        return false;
    case SelfSquashTrigger::DirectTargetMismatch:
    case SelfSquashTrigger::ReturnRasFixup:
        return !pred_btb_hit;
    }
    return false; // unreachable
}

/**
 * Fetch-time predBtbHit snapshot predicate
 * (Fetch::lookupAndUpdateNextPC): PC membership among the VALID entries
 * of the supplying FetchTarget.  Stricter than predict_taken: a hit
 * entry whose conditional branch is direction-predicted not-taken is
 * still a hit.  Templated over the entry container so the unit test can
 * drive it with a mirror of BTBEntry's {valid, pc} surface.
 */
template <typename EntryRange>
inline bool
predBtbHitSnapshot(uint64_t pc, const EntryRange &entries)
{
    for (const auto &entry : entries) {
        if (entry.valid && entry.pc == pc) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// R3/R4 — per-tid redirect / recovery window machine (fetch.cc)
// ---------------------------------------------------------------------------

/**
 * Half-open [start, cur_cycle) window with the overlap and ROI-boundary
 * rules shared by the BTB-miss redirect window (R3) and the frontend
 * recovery window (R4):
 *   - close(cur) charges cur - start cycles (the opening cycle itself is
 *     an empty-delivery cycle; the closing cycle delivered >= 1 and is
 *     not charged);
 *   - open(cur) closes any still-open window FIRST (overlap rule: a new
 *     redirect never double-charges a cycle already charged to the old
 *     window) and then opens a fresh one;
 *   - discard() drops an open window WITHOUT charging (the stats-reset
 *     ROI boundary: a window open at the boundary is billed to neither
 *     segment).
 */
struct RedirectWindow
{
    bool active = false;
    uint64_t start = 0;

    /** Returns the elapsed cycles charged by this close (0 if idle). */
    uint64_t close(uint64_t cur_cycle)
    {
        if (!active) {
            return 0;
        }
        const uint64_t elapsed = cur_cycle - start;
        active = false;
        return elapsed;
    }

    /**
     * Overlap rule: close-then-open, never double charge.  Returns the
     * elapsed cycles charged by the embedded close (the caller binds it
     * to its counter exactly like an explicit close would).
     */
    uint64_t open(uint64_t cur_cycle)
    {
        const uint64_t elapsed = close(cur_cycle);
        active = true;
        start = cur_cycle;
        return elapsed;
    }

    /** ROI boundary: drop without charging. */
    void discard() { active = false; }
};

// ---------------------------------------------------------------------------
// R7 — memstall misslevel cumulative semantics (issue_queue.cc)
// ---------------------------------------------------------------------------

/** Misslevel-derived memstall charges for one idle-FU cycle. */
struct MisslevelCharge
{
    bool any_load = false; // any outstanding miss in flight
    bool l1 = false;       // any depth >= 1 miss in flight
    bool l2 = false;       // any depth >= 2 miss in flight
    bool l3 = false;       // any depth >= 3 miss in flight
};

/**
 * Cumulative ("at least level k in flight") classification of the
 * LSQ::anyInflightLoadsNotComplete bit encoding:
 *   bit0 = an inflight load has missed cache level 1,
 *   bit1 = missed level 2, bit2 = missed level 3, bit3 = any outstanding.
 * depth==k necessarily missed every level below it, so a level-k miss
 * sets all the shallower cumulative buckets.
 */
inline MisslevelCharge
classifyMisslevel(int misslevel)
{
    MisslevelCharge c;
    c.any_load = misslevel != 0;
    c.l1 = (misslevel & 0x7) != 0;
    c.l2 = (misslevel & 0x6) != 0;
    c.l3 = (misslevel & 0x4) != 0;
    return c;
}

} // namespace tdstats
} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TOPDOWN_STATS_HELPERS_HH__
