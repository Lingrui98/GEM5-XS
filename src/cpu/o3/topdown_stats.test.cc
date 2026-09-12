// Unit tests for the D4-A topdown statistics fixes (batch 1: R1/R2/R6/R7).
//
// These tests exercise the *decision logic* of each statistics-accounting
// fix as pure functions, deliberately avoiding a full gem5 simulation build,
// following the self-contained pattern of trace_mode_regression.test.cc
// (commit 4433a5a60b).  All logic is duplicated inline so that the test is
// self-contained; each part is annotated with the production source it
// guards so that drift can be detected by inspection.
//
// NOTE: No production files are modified by this test.  If the production
//       logic changes, the inlined copies here must be updated in lockstep.
//
// Build (NULL ISA):
//   scons build/NULL/cpu/o3/topdown_stats.test.opt --unit-test -j$(nproc)
// Run:
//   ./build/NULL/cpu/o3/topdown_stats.test.opt

#include <gtest/gtest.h>

#include <vector>

// ===========================================================================
// PART R1 — cycle-level frontend-bubble attribution (empty-queue path)
// Guard: src/cpu/o3/fetch.cc
//   Fetch::sendInstructionsToDecode   (path selection: paths 1/2/3)
//   Fetch::measureFrontendBubbles     (per-thread accounting, paths 1/2)
//   Fetch::measureFrontendBubblesEmptyQueueCycle  (cycle-level aggregate,
//                                                  added by this commit)
//
// Failure mode (without fix): when some thread is not backend-blocked but
// every unblocked thread has an empty fetch queue, sendInstructionsToDecode
// early-returned through `if (tid == -1)` without calling any accounting
// function, so the only cycles that can produce a full-width bubble
// (fetchBubbles_max) were silently dropped.  The fix charges at most ONE
// full-width bubble per cycle (never per thread, never tid == -1 passed to a
// per-thread indexed stats function).
//
// Truth table (per cycle; A = exists unblocked thread, B = exists candidate
// = exists unblocked thread with non-empty queue, R(i) = !robSquashing[i]):
//   #  A  B  robSquashing            path   delta (bubbles, bubbles_max)
//   1  0  0  any                     2      (0, 0)
//   2  1  0  exists R(i)=1           3      (W, 1)
//   3  1  0  all R(i)=0              3      (0, 0)
//   4  1  1  R(sel)=1                1      (W-k, 0),  1 <= k <= W
//   5  1  1  R(sel)=0                1      (0, 0)
// ===========================================================================

namespace
{

// Fake per-thread cycle state as seen by sendInstructionsToDecode.
struct FakeThreadState
{
    bool blockFetch    = false;  // backend can receive?
    bool robSquashing  = false;  // in squash recovery (fromCommit, delayed)?
    bool queueEmpty    = false;  // fetchQueue[tid].empty()?
};

// Accounted delta for one cycle.
struct BubbleDelta
{
    long fetchBubbles    = 0;
    long fetchBubbles_max = 0;
};

// Exact reimplementation of Fetch::measureFrontendBubbles (fetch.cc):
// per-thread accounting, gated on !blockFetch && !robSquashing.
BubbleDelta
measureFrontendBubblesFake(unsigned insts_to_decode,
                           const FakeThreadState &t,
                           unsigned decode_width)
{
    BubbleDelta d;
    if (!t.blockFetch && !t.robSquashing) {
        const int unused_slots =
            static_cast<int>(decode_width) - static_cast<int>(insts_to_decode);
        if (unused_slots > 0) {
            d.fetchBubbles = unused_slots;
            if (unused_slots == static_cast<int>(decode_width)) {
                d.fetchBubbles_max = 1;
            }
        }
    }
    return d;
}

// Exact reimplementation of Fetch::measureFrontendBubblesEmptyQueueCycle
// (fetch.cc, added by this fix): cycle-level aggregate; charges one
// full-width bubble iff some thread is both unblocked and outside squash
// recovery; returns after the first charge (no double counting).
BubbleDelta
measureFrontendBubblesEmptyQueueCycleFake(
    const std::vector<FakeThreadState> &threads, unsigned decode_width)
{
    for (const auto &t : threads) {
        if (!t.blockFetch && !t.robSquashing) {
            return BubbleDelta{static_cast<long>(decode_width), 1};
        }
    }
    return BubbleDelta{0, 0};
}

// Exact reimplementation of the cycle-level path selection in
// Fetch::sendInstructionsToDecode (fetch.cc):
//   path 2 (A == 0): per-thread measureFrontendBubbles(0, i) for all i
//                    (gate fails for every thread -> no bubbles)
//   path 3 (A == 1, B == 0): measureFrontendBubblesEmptyQueueCycle()
//   path 1 (A == 1, B == 1): measureFrontendBubbles(k, tid_sel)
// selectUnstalledThread returns -1 iff no candidate exists (B == 0).
BubbleDelta
accountFrontendBubblesCycle(const std::vector<FakeThreadState> &threads,
                            unsigned decode_width,
                            int sel_tid,
                            unsigned insts_to_decode)
{
    bool any_thread_active = false;
    for (const auto &t : threads) {
        if (!t.blockFetch) {
            any_thread_active = true;
        }
    }
    if (!any_thread_active) {
        // path 2: all threads backend-blocked; gate !blockFetch fails for
        // every thread so no bubbles are charged.
        return BubbleDelta{0, 0};
    }

    bool has_candidate = false;
    for (const auto &t : threads) {
        if (!t.blockFetch && !t.queueEmpty) {
            has_candidate = true;
        }
    }
    if (!has_candidate) {
        // path 3: empty-queue early out (tid == -1).
        return measureFrontendBubblesEmptyQueueCycleFake(threads,
                                                         decode_width);
    }

    // path 1: a thread was selected and delivered insts_to_decode >= 1.
    return measureFrontendBubblesFake(insts_to_decode, threads[sel_tid],
                                      decode_width);
}

} // anonymous namespace

// --- R1 truth table, row by row (W = 8) -------------------------------

// Row 1: A=0 (all threads blockFetch) -> no frontend bubble, regardless of
// robSquashing / queue state.
TEST(R1TruthTable, Row1AllThreadsBlockedChargesNothing)
{
    const std::vector<FakeThreadState> threads = {
        {true, false, true},   // blocked, not squashing, empty queue
        {true, true, false},   // blocked, squashing, non-empty queue
    };
    const auto d = accountFrontendBubblesCycle(threads, 8, /*sel_tid=*/-1,
                                               /*k=*/0);
    EXPECT_EQ(d.fetchBubbles, 0);
    EXPECT_EQ(d.fetchBubbles_max, 0);
}

// Row 2: A=1, B=0, some unblocked thread outside squash recovery ->
// exactly one full-width bubble (fetchBubbles += W, fetchBubbles_max += 1).
// This is the cycle class the fix restores.
TEST(R1TruthTable, Row2EmptyQueueUnblockedNotSquashingChargesFullWidth)
{
    const std::vector<FakeThreadState> threads = {
        {false, false, true},   // unblocked, not squashing, empty queue
    };
    const auto d = accountFrontendBubblesCycle(threads, 8, -1, 0);
    EXPECT_EQ(d.fetchBubbles, 8);
    EXPECT_EQ(d.fetchBubbles_max, 1);
}

// Row 3: A=1, B=0, every unblocked thread is robSquashing -> no charge;
// the cycle belongs to bad-speculation recovery, not the frontend.
TEST(R1TruthTable, Row3EmptyQueueAllUnblockedSquashingChargesNothing)
{
    const std::vector<FakeThreadState> threads = {
        {false, true, true},   // unblocked but squashing, empty queue
    };
    const auto d = accountFrontendBubblesCycle(threads, 8, -1, 0);
    EXPECT_EQ(d.fetchBubbles, 0);
    EXPECT_EQ(d.fetchBubbles_max, 0);
}

// Row 4: A=1, B=1, selected thread outside squash recovery -> partial
// bubble (W - k) slots, no max increment (existing behaviour, unchanged).
TEST(R1TruthTable, Row4PartialSupplyChargesUnusedSlotsOnly)
{
    const std::vector<FakeThreadState> threads = {
        {false, false, false},   // candidate thread, non-empty queue
    };
    const auto d = accountFrontendBubblesCycle(threads, 8, /*sel_tid=*/0,
                                               /*k=*/4);
    EXPECT_EQ(d.fetchBubbles, 4);
    EXPECT_EQ(d.fetchBubbles_max, 0);
}

// Row 5: A=1, B=1, selected thread robSquashing -> no charge (existing
// behaviour, unchanged).
TEST(R1TruthTable, Row5PartialSupplyWhileSquashingChargesNothing)
{
    const std::vector<FakeThreadState> threads = {
        {false, true, false},   // candidate thread but squashing
    };
    const auto d = accountFrontendBubblesCycle(threads, 8, 0, 4);
    EXPECT_EQ(d.fetchBubbles, 0);
    EXPECT_EQ(d.fetchBubbles_max, 0);
}

// --- R1 SMT semantics --------------------------------------------------

// SMT: several empty-queue unblocked threads must still be charged only
// ONE full-width bubble per cycle (no per-thread double charging).
TEST(R1Smt, MultipleEmptyQueueThreadsChargeSingleFullWidthBubble)
{
    const std::vector<FakeThreadState> threads = {
        {false, false, true},
        {false, false, true},
        {false, false, true},
    };
    const auto d = accountFrontendBubblesCycle(threads, 8, -1, 0);
    EXPECT_EQ(d.fetchBubbles, 8);
    EXPECT_EQ(d.fetchBubbles_max, 1);
}

// SMT: mixed recovery state — as long as ONE unblocked thread is outside
// squash recovery, the empty-queue cycle is charged once.
TEST(R1Smt, OneRecoverableThreadAmongSquashersIsEnough)
{
    const std::vector<FakeThreadState> threads = {
        {false, true, true},    // squashing
        {false, false, true},   // not squashing -> gate satisfied
    };
    const auto d = accountFrontendBubblesCycle(threads, 8, -1, 0);
    EXPECT_EQ(d.fetchBubbles, 8);
    EXPECT_EQ(d.fetchBubbles_max, 1);
}

// SMT: all unblocked threads squashing (even with a blocked thread present)
// -> no charge.
TEST(R1Smt, AllUnblockedSquashingWithBlockedThreadChargesNothing)
{
    const std::vector<FakeThreadState> threads = {
        {true, false, true},    // blocked
        {false, true, true},    // unblocked but squashing
    };
    const auto d = accountFrontendBubblesCycle(threads, 8, -1, 0);
    EXPECT_EQ(d.fetchBubbles, 0);
    EXPECT_EQ(d.fetchBubbles_max, 0);
}

// --- G4(a): forced empty-queue workload --------------------------------

// G4(a): a workload with forced empty-queue cycles (unblocked, not
// squashing, empty queue) must yield, after the fix,
//   frontendLatencyBound > 0  and  frontendLatencyBound <= frontendBound
// under the FIXED (slot-uniform) latency formula from the R2 fix:
//   frontendBound        = fetchBubbles / (issueWidth * numCycles)
//   frontendLatencyBound = fetchBubbles_max * decodeWidth /
//                          (issueWidth * numCycles)
// Scenario (W = 8, issueWidth = 8, 5 cycles): 3 forced-empty cycles + 2
// cycles supplying k = 4 instructions (partial).
//   fetchBubbles = 3*8 + 2*4 = 32, fetchBubbles_max = 3
//   frontendBound        = 32 / (8*5) = 0.8
//   frontendLatencyBound = 3*8 / (8*5) = 0.6
TEST(G4aForcedEmptyQueue, LatencyBoundPositiveAndWithinFrontendBound)
{
    const unsigned W = 8;
    const FakeThreadState empty_cycle{false, false, true};
    const FakeThreadState supply_cycle{false, false, false};

    long fetch_bubbles = 0;
    long fetch_bubbles_max = 0;
    for (int c = 0; c < 3; ++c) {
        const auto d = accountFrontendBubblesCycle({empty_cycle}, W, -1, 0);
        fetch_bubbles += d.fetchBubbles;
        fetch_bubbles_max += d.fetchBubbles_max;
    }
    for (int c = 0; c < 2; ++c) {
        const auto d = accountFrontendBubblesCycle({supply_cycle}, W, 0, 4);
        fetch_bubbles += d.fetchBubbles;
        fetch_bubbles_max += d.fetchBubbles_max;
    }

    EXPECT_EQ(fetch_bubbles, 32);
    EXPECT_EQ(fetch_bubbles_max, 3);

    const double num_cycles = 5.0;
    const double issue_width = 8.0;   // == decodeWidth (alias in cpu.cc)
    const double frontend_bound =
        static_cast<double>(fetch_bubbles) / (issue_width * num_cycles);
    const double latency_bound =
        static_cast<double>(fetch_bubbles_max) * W /
        (issue_width * num_cycles);

    EXPECT_NEAR(frontend_bound, 0.8, 1e-12);
    EXPECT_NEAR(latency_bound, 0.6, 1e-12);
    EXPECT_GT(latency_bound, 0.0);
    EXPECT_LE(latency_bound, frontend_bound);
}

// --- G4(b): downstream-blocked workload --------------------------------

// G4(b): all threads blockFetch (downstream cannot receive) -> no frontend
// bubble is charged on any cycle.
TEST(G4bDownstreamBlocked, NoFrontendBubblesCharged)
{
    const std::vector<FakeThreadState> threads = {
        {true, false, true},
    };
    for (int c = 0; c < 5; ++c) {
        const auto d = accountFrontendBubblesCycle(threads, 8, -1, 0);
        EXPECT_EQ(d.fetchBubbles, 0);
        EXPECT_EQ(d.fetchBubbles_max, 0);
    }
}
