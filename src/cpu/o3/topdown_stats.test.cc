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

// ===========================================================================
// PART R2 — frontendLatencyBound dimension unification
// Guard: src/cpu/o3/cpu.cc (O3CPU::O3CPUStats: frontendLatencyBound formula)
//        src/cpu/o3/fetch.cc (FetchStatGroup: frontendLatencyBound formula)
//
// Failure mode (without fix): frontendBound divided slot-level bubbles by
// (issueWidth * numCycles) while frontendLatencyBound divided cycle-level
// fetchBubbles_max by numCycles alone — two different dimensions (slots vs
// cycles), so the frontendBandwidthBound difference mixed units.
//
// Fixed formulas (cpu.cc; the fetch.cc FetchStatGroup twin is isomorphic
// with decodeWidth in place of issueWidth, which fetch cannot see):
//   frontendBound        = fetchBubbles / (issueWidth * numCycles)
//   frontendLatencyBound = fetchBubbles_max * decodeWidth /
//                          (issueWidth * numCycles)
//   frontendBandwidthBound = frontendBound - frontendLatencyBound
//
// Known fact: O3CPU has no independent issueWidth SimObject param —
// cpu.cc constructs issueWidth(params.decodeWidth), so the two widths are
// aliases and the fix is numerically an identity.  G4(g) therefore can only
// vary decodeWidth (which moves issueWidth with it).
// ===========================================================================

namespace
{

// Fixed cpu.cc formula for frontendLatencyBound.
double
frontendLatencyBoundFixed(long fetch_bubbles_max, double decode_width,
                          double issue_width, double num_cycles)
{
    return static_cast<double>(fetch_bubbles_max) * decode_width /
           (issue_width * num_cycles);
}

// Pre-fix (legacy) cpu.cc formula: cycle-level / cycle-level.
double
frontendLatencyBoundLegacy(long fetch_bubbles_max, double num_cycles)
{
    return static_cast<double>(fetch_bubbles_max) / num_cycles;
}

// Fixed cpu.cc formula for frontendBound (unchanged by R2, used as the
// reference bound).
double
frontendBoundFixed(long fetch_bubbles, double issue_width, double num_cycles)
{
    return static_cast<double>(fetch_bubbles) / (issue_width * num_cycles);
}

} // anonymous namespace

// Identity: with issueWidth aliased to decodeWidth (the only configuration
// reality — cpu.cc: issueWidth(params.decodeWidth)), the fixed formula is
// numerically identical to the legacy one.  Checked at W = 8 and W = 4.
TEST(R2Formula, IdentityWhenIssueWidthAliasesDecodeWidth)
{
    // W = 8 (kmhv3.py default): 3 max-bubble cycles out of 5.
    EXPECT_DOUBLE_EQ(frontendLatencyBoundFixed(3, 8.0, 8.0, 5.0),
                     frontendLatencyBoundLegacy(3, 5.0));
    // W = 4 (alternate decodeWidth; issueWidth follows it).
    EXPECT_DOUBLE_EQ(frontendLatencyBoundFixed(3, 4.0, 4.0, 5.0),
                     frontendLatencyBoundLegacy(3, 5.0));
}

// G4(g): width-variant self-consistency.  Only decodeWidth can vary (and
// issueWidth with it).  W = 4 scenario: 3 forced-empty cycles + 2 cycles
// supplying k = 2 out of 4 slots.
//   fetchBubbles = 3*4 + 2*2 = 16, fetchBubbles_max = 3
//   frontendBound        = 16 / (4*5) = 0.8
//   frontendLatencyBound = 3*4 / (4*5) = 0.6
//   frontendBandwidthBound = 0.8 - 0.6 = 0.2  (>= 0)
// The accounting itself reuses the R1 pure functions so the widths and the
// attribution stay in lockstep.
TEST(G4gWidthVariant, FormulaSelfConsistentAtDecodeWidth4)
{
    const unsigned W = 4;
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
        const auto d = accountFrontendBubblesCycle({supply_cycle}, W, 0, 2);
        fetch_bubbles += d.fetchBubbles;
        fetch_bubbles_max += d.fetchBubbles_max;
    }

    EXPECT_EQ(fetch_bubbles, 16);
    EXPECT_EQ(fetch_bubbles_max, 3);

    const double num_cycles = 5.0;
    const double issue_width = 4.0;   // == decodeWidth (alias in cpu.cc)
    const double frontend_bound =
        frontendBoundFixed(fetch_bubbles, issue_width, num_cycles);
    const double latency_bound =
        frontendLatencyBoundFixed(fetch_bubbles_max, W, issue_width,
                                  num_cycles);

    EXPECT_NEAR(frontend_bound, 0.8, 1e-12);
    EXPECT_NEAR(latency_bound, 0.6, 1e-12);
    EXPECT_LE(latency_bound, frontend_bound);
    EXPECT_NEAR(frontend_bound - latency_bound, 0.2, 1e-12);
}

// G6 sanity at both widths: latency bound never exceeds frontend bound and
// the bandwidth residual is non-negative across a sweep of workload mixes.
TEST(G6Budget, LatencyNeverExceedsFrontendAcrossMixes)
{
    for (unsigned W : {4u, 8u}) {
        for (int empty = 0; empty <= 6; ++empty) {
            for (int partial = 0; partial <= 6; ++partial) {
                const int num_cycles = empty + partial + 1;  // >=1 supply-full
                long bubbles = 0;
                long max_cycles = 0;
                bubbles += empty * W;
                max_cycles += empty;
                bubbles += partial * (W / 2);   // k = W/2 partial supply
                const double fb = frontendBoundFixed(
                    bubbles, W, static_cast<double>(num_cycles));
                const double lb = frontendLatencyBoundFixed(
                    max_cycles, W, W, static_cast<double>(num_cycles));
                EXPECT_LE(lb, fb);
                EXPECT_GE(fb - lb, -1e-12);
            }
        }
    }
}

// ===========================================================================
// PART R6 — totalSquash completeness (value-prediction term)
// Guard: src/cpu/o3/commit.cc (CommitStatGroup: totalSquash formula)
//
// Failure mode (without fix): totalSquash summed
//   branch + orderViolation + trap + tc + squashAfter
// and omitted squashDueToValuePrediction, undercounting totalSquash
// whenever a value predictor is attached (squashDueToValuePrediction is
// incremented in the fromIEW valuePredictionError squash dispatch).
//
// Fixed formula:
//   totalSquash = branch + orderViolation + valuePrediction + trap + tc
//                 + squashAfter
// ===========================================================================

namespace
{

// Fixed commit.cc formula.
long
totalSquashFixed(long branch, long order_violation, long value_prediction,
                 long trap, long tc, long squash_after)
{
    return branch + order_violation + value_prediction + trap + tc +
           squash_after;
}

// Pre-fix (legacy) commit.cc formula: value-prediction term missing.
long
totalSquashLegacy(long branch, long order_violation, long trap, long tc,
                  long squash_after)
{
    return branch + order_violation + trap + tc + squash_after;
}

} // anonymous namespace

// VP term == 0 (VP = NULL, the default configuration): the fix is a
// numerical identity — totalSquash is unchanged for runs without a value
// predictor.
TEST(R6TotalSquash, ValuePredictionZeroLeavesTotalUnchanged)
{
    const long branch = 120, order_violation = 7, vp = 0, trap = 3,
               tc = 2, squash_after = 11;
    EXPECT_EQ(totalSquashFixed(branch, order_violation, vp, trap, tc,
                               squash_after),
              totalSquashLegacy(branch, order_violation, trap, tc,
                                squash_after));
    EXPECT_EQ(totalSquashFixed(branch, order_violation, vp, trap, tc,
                               squash_after), 143);
}

// VP term != 0 (value predictor attached): the previously dropped
// squashDueToValuePrediction events are now included in totalSquash.
TEST(R6TotalSquash, ValuePredictionNonzeroIsCounted)
{
    const long branch = 100, order_violation = 5, vp = 42, trap = 3,
               tc = 2, squash_after = 11;
    // Legacy: 100 + 5 + 3 + 2 + 11 = 121 (undercounted by 42).
    EXPECT_EQ(totalSquashLegacy(branch, order_violation, trap, tc,
                                squash_after), 121);
    // Fixed: 100 + 5 + 42 + 3 + 2 + 11 = 163.
    EXPECT_EQ(totalSquashFixed(branch, order_violation, vp, trap, tc,
                               squash_after), 163);
    EXPECT_EQ(totalSquashFixed(branch, order_violation, vp, trap, tc,
                               squash_after),
              totalSquashLegacy(branch, order_violation, trap, tc,
                                squash_after) + vp);
}

// ===========================================================================
// PART R7 — memstall misslevel cumulative semantics
// Guard: src/cpu/o3/issue_queue.cc (Scheduler::issueAndSelect: the three
//        memstall_l{1,2,3}miss decisions)
// Bit encoding definition (NOT modified by the fix):
//   src/cpu/o3/lsq.cc LSQ::anyInflightLoadsNotComplete()
//     bit0 (0x1) = an inflight load has missed cache level 1 (depth == 1)
//     bit1 (0x2) = an inflight load has missed cache level 2 (depth == 2)
//     bit2 (0x4) = an inflight load has missed cache level 3 (depth == 3)
//     bit3 (0x8) = any outstanding miss
//
// Failure mode (without fix): the consumer treated the per-level existence
// flags as an exact low-bit mask, e.g. l3miss required bits 0..2 ALL set
// (an L1 miss AND an L2 miss AND an L3 miss in flight simultaneously).
// A cycle with only an L3 miss in flight (misslevel 0xC) charged none of
// l1/l2/l3miss.  depth == k means the request has already missed cache
// level k (each cache-level miss bumps Request::depth exactly once, see
// BaseCache::incMissCount -> Request::incAccessDepth), so a level-k miss
// necessarily missed every level below: cumulative ("at least level k in
// flight") semantics are the correct consumption.
//
// Fixed decisions:
//   l1miss: (misslevel & 0x7) != 0   any depth >= 1 miss in flight
//   l2miss: (misslevel & 0x6) != 0   any depth >= 2 miss in flight
//   l3miss: (misslevel & 0x4) != 0   any depth >= 3 miss in flight
// ===========================================================================

namespace
{

struct MemstallDelta
{
    int l1miss = 0;
    int l2miss = 0;
    int l3miss = 0;
};

// Fixed issue_queue.cc decisions (cumulative semantics).
MemstallDelta
memstallDecisionFixed(int misslevel)
{
    return MemstallDelta{(misslevel & 0x7) ? 1 : 0,
                         (misslevel & 0x6) ? 1 : 0,
                         (misslevel & 0x4) ? 1 : 0};
}

// Pre-fix (legacy) issue_queue.cc decisions: exact-mask equality on the
// low k bits (all of bits 0..k-1 must be set simultaneously).
MemstallDelta
memstallDecisionLegacy(int misslevel)
{
    return MemstallDelta{
        (misslevel & ((1 << 1) - 1)) == ((1 << 1) - 1) ? 1 : 0,
        (misslevel & ((1 << 2) - 1)) == ((1 << 2) - 1) ? 1 : 0,
        (misslevel & ((1 << 3) - 1)) == ((1 << 3) - 1) ? 1 : 0};
}

} // anonymous namespace

// Full 8-combination truth table over the depth-existence bits
// (bit0 = depth1 exists, bit1 = depth2 exists, bit2 = depth3 exists),
// each combined with the any-bit (0x8) which must not affect the
// level decisions.  Exact expected values for every combination.
TEST(R7Misslevel, AllEightDepthCombinationsExactExpectations)
{
    // {misslevel_low3, expected(l1, l2, l3)} — cumulative semantics:
    //   l1: any depth >= 1  (any of bits 0..2)
    //   l2: any depth >= 2  (bit1 or bit2)
    //   l3: any depth >= 3  (bit2)
    const struct
    {
        int low3;
        int l1;
        int l2;
        int l3;
    } expect[8] = {
        {0x0, 0, 0, 0},  // no miss in flight
        {0x1, 1, 0, 0},  // depth-1 miss only (missed L1)
        {0x2, 1, 1, 0},  // depth-2 miss only (missed L1+L2)
        {0x3, 1, 1, 0},  // depth-1 + depth-2 misses in flight
        {0x4, 1, 1, 1},  // depth-3 miss only (missed L1+L2+L3)
        {0x5, 1, 1, 1},  // depth-1 + depth-3 misses
        {0x6, 1, 1, 1},  // depth-2 + depth-3 misses
        {0x7, 1, 1, 1},  // depth-1 + depth-2 + depth-3 misses
    };

    for (const auto &e : expect) {
        for (const int any_bit : {0, 0x8}) {
            const int misslevel = e.low3 | any_bit;
            const auto d = memstallDecisionFixed(misslevel);
            EXPECT_EQ(d.l1miss, e.l1);
            EXPECT_EQ(d.l2miss, e.l2);
            EXPECT_EQ(d.l3miss, e.l3);
        }
    }
}

// L3-only miss stream (the recon-report failure example): a single inflight
// load that missed L1, L2 and L3 (depth == 3) produces misslevel 0x4|0x8 =
// 0xC.  Legacy code charged NOTHING (0xC & 0x1 == 0, 0xC & 0x3 != 0x3,
// 0xC & 0x7 != 0x7); the fix charges all three levels, because an L3 miss
// has necessarily missed L1 and L2 on its way down.
TEST(R7Misslevel, L3OnlyMissStreamLegacyChargedNothingFixChargesAll)
{
    const int misslevel_l3_only = 0xC;  // bit2 (depth==3) + bit3 (any)
    const auto legacy = memstallDecisionLegacy(misslevel_l3_only);
    const auto fixed = memstallDecisionFixed(misslevel_l3_only);

    // Legacy undercount: none of the three counters fired.
    EXPECT_EQ(legacy.l1miss, 0);
    EXPECT_EQ(legacy.l2miss, 0);
    EXPECT_EQ(legacy.l3miss, 0);

    // Fixed: all three levels are (cumulatively) in miss.
    EXPECT_EQ(fixed.l1miss, 1);
    EXPECT_EQ(fixed.l2miss, 1);
    EXPECT_EQ(fixed.l3miss, 1);
}

// Legacy vs fixed on the remaining diagnostic combinations: the legacy
// decisions only agreed with the cumulative semantics when the exact low
// bits happened to be fully populated (0x1, 0x3, 0x7) or empty (0x0).
TEST(R7Misslevel, LegacyDisagreesOnSparseLevelCombinations)
{
    // 0x2 (depth-2 only): legacy charged nothing; fixed charges l1+l2.
    EXPECT_EQ(memstallDecisionLegacy(0x2).l1miss, 0);
    EXPECT_EQ(memstallDecisionLegacy(0x2).l2miss, 0);
    EXPECT_EQ(memstallDecisionFixed(0x2).l1miss, 1);
    EXPECT_EQ(memstallDecisionFixed(0x2).l2miss, 1);
    EXPECT_EQ(memstallDecisionFixed(0x2).l3miss, 0);

    // 0x5 (depth-1 + depth-3): legacy charged only l1; fixed charges all.
    EXPECT_EQ(memstallDecisionLegacy(0x5).l1miss, 1);
    EXPECT_EQ(memstallDecisionLegacy(0x5).l2miss, 0);
    EXPECT_EQ(memstallDecisionLegacy(0x5).l3miss, 0);
    EXPECT_EQ(memstallDecisionFixed(0x5).l1miss, 1);
    EXPECT_EQ(memstallDecisionFixed(0x5).l2miss, 1);
    EXPECT_EQ(memstallDecisionFixed(0x5).l3miss, 1);
}

// Any-bit alone (0x8: an outstanding request whose depth is not 1/2/3 —
// e.g. depth 0, request not yet through the first level) must not charge
// any of the level counters; memstall_any_load (checked separately in
// production code as misslevel != 0) still fires.
TEST(R7Misslevel, AnyBitAloneChargesNoLevel)
{
    const auto d = memstallDecisionFixed(0x8);
    EXPECT_EQ(d.l1miss, 0);
    EXPECT_EQ(d.l2miss, 0);
    EXPECT_EQ(d.l3miss, 0);
}

// ===========================================================================
// PART R3 — BTB-miss decode-redirect diagnostics (event classification +
//           per-tid redirect window state machine)
// Guard: src/cpu/o3/fetch.cc
//   Fetch::lookupAndUpdateNextPC      (predBtbHit snapshot write)
//   Fetch::FetchStatGroup::{openBtbMissWindow, closeBtbMissWindow,
//                           resetStats}   (window state machine)
//   Fetch::handleDecodeSquash         (window open/truncate hook)
//   Fetch::handleCommitSignals        (truncate-on-deeper-squash hook)
//   Fetch::sendInstructionsToDecode   (close-on-delivery hook)
// Guard: src/cpu/o3/decode.cc
//   Decode::selfSquash trigger points 1/2/3 (event classification)
// Guard: src/cpu/o3/dyn_inst.hh
//   predBtbHitValue + setPredBtbHit/readPredBtbHit (the snapshot field)
//
// Semantics implemented (D4-A contract):
// - predBtbHit = the instruction's PC hit a *valid* entry of the supplying
//   FetchTarget's predBTBEntries at prediction time (stricter than
//   predict_taken: a hit whose cond branch is predicted not-taken is still
//   a hit).  Snapshot written in fetch, read only by statistics code.
// - btbMissResteers (events): decode selfSquash classification —
//     trigger 1 (readPredTaken && !isControl, false-hit):      NOT counted
//       (readPredTaken implies the PC matched a taken predBTBEntry, so
//       predBtbHit is necessarily true there);
//     trigger 2 (direct branch, target != predTarg): counted iff
//       predBtbHit false (BTB miss); a hit with wrong target (alias/
//       stale) is NOT counted;
//     trigger 3 (unpredicted return taking the RAS fixup): counted iff
//       predBtbHit false.
// - btbMissResteerCycles (window, per tid, half-open [open, close)):
//     open  = the fetch tick that consumes the decode redirect
//             (decodeToFetchDelay = 1 after decode's selfSquash; the
//             fetch-side receipt keeps start and end on one timeline);
//     close = the first tick sendInstructionsToDecode delivers >= 1
//             instruction for that tid, OR
//             a newer decode redirect (overlap rule: close old by elapsed
//             cycles, then open fresh — never double charge), OR
//             a commit squash (truncation: the awaited path is
//             invalidated), OR
//             the stats-reset ROI boundary (DISCARDED — never billed
//             across the segment boundary);
//     SMT   = independent per-tid windows.
// - gem5BranchResteers = btbMissResteerCycles / numCycles is a gem5 proxy
//   for the BTB-miss subset only, NOT the full Intel TMA Branch_Resteers.
// ===========================================================================

namespace
{

// Mirror of the predBTBEntries membership test written in
// Fetch::lookupAndUpdateNextPC (fetch.cc).  BTBEntry mirror: the real
// struct is BTBEntry : BranchInfo with `valid` and inherited `pc`
// (src/cpu/pred/btb/common.hh:243).
struct FakeBTBEntry
{
    bool valid = false;
    uint64_t pc = 0;
};

// Exact replica of the fetch-time snapshot predicate (fetch.cc,
// Fetch::lookupAndUpdateNextPC): PC membership among valid entries.
bool
predBtbHitSnapshot(uint64_t pc, const std::vector<FakeBTBEntry> &entries)
{
    for (const auto &entry : entries) {
        if (entry.valid && entry.pc == pc) {
            return true;
        }
    }
    return false;
}

// The three Decode::selfSquash trigger points (decode.cc):
//   trigger 1 ~@719  readPredTaken() && !isControl()        (false-hit)
//   trigger 2 ~@774  direct ctrl, target != readPredTarg()
//   trigger 3 ~@836  isReturn() && !readPredTaken()         (RAS fixup)
enum class SelfSquashTrigger
{
    FalseHitNonControl,
    DirectTargetMismatch,
    ReturnRasFixup,
};

// Exact replica of the decode-side event classification (decode.cc,
// trigger points 2 and 3: `if (!inst->readPredBtbHit())
// fetch_ptr->countBtbMissResteer();`).  Trigger 1 has no counting hook:
// a predicted-taken match implies the PC matched a taken predBTBEntry, so
// predBtbHit is necessarily true and the row is never a BTB miss.
bool
countsAsBtbMissResteer(SelfSquashTrigger trigger, bool pred_btb_hit)
{
    switch (trigger) {
    case SelfSquashTrigger::FalseHitNonControl:
        return false;
    case SelfSquashTrigger::DirectTargetMismatch:
    case SelfSquashTrigger::ReturnRasFixup:
        return !pred_btb_hit;
    }
    return false;  // unreachable
}

// Per-tid redirect window machine: exact replica of
// Fetch::FetchStatGroup::{openBtbMissWindow, closeBtbMissWindow,
// resetStats} (fetch.cc) driven through the per-tick hook ordering of
// Fetch::tick -> initializeTickState (checkSignalsAndUpdate:
// handleCommitSignals then handleDecodeSquash) then
// fetchAndProcessInstructions (sendInstructionsToDecode).
struct BtbMissWindowMachine
{
    // Window state (fetch.cc FetchStatGroup::btbMissWindow[tid]).
    bool active = false;
    long start = 0;

    // Charged counters (reset together by the stats reset).
    long btbMissResteers = 0;         // events (classified in decode)
    long btbMissResteerCycles = 0;    // window cycles

    long cur_cycle = 0;
    // fetchStatus == Squashing from a squash processed on the previous
    // tick (set by doSquash, cleared in checkSignalsAndUpdate after
    // handleDecodeSquash on the following tick).
    bool prev_tick_squash = false;

    // Replica of FetchStatGroup::closeBtbMissWindow.
    void
    close()
    {
        if (!active) {
            return;
        }
        const long elapsed = cur_cycle - start;
        if (elapsed > 0) {
            btbMissResteerCycles += elapsed;
        }
        active = false;
    }

    // Replica of FetchStatGroup::openBtbMissWindow (overlap rule).
    void
    open()
    {
        close();
        active = true;
        start = cur_cycle;
    }

    // Events visible to fetch within one tick, in processing order.
    struct TickEvents
    {
        bool commit_squash = false;        // handleCommitSignals
        bool decode_redirect_btb_miss = false;  // handleDecodeSquash
        bool decode_redirect_other = false;     // handleDecodeSquash
        bool delivery = false;             // sendInstructionsToDecode
    };

    // One fetch tick.
    void
    tick(const TickEvents &ev)
    {
        bool squash_processed = false;

        // (1) handleCommitSignals (fetch.cc:2600): a commit squash
        //     truncates an open BTB-miss window (deeper squash
        //     invalidates the awaited path).
        if (ev.commit_squash) {
            close();
            squash_processed = true;
        }

        // (2) handleDecodeSquash (fetch.cc:2688): processed only when no
        //     commit squash was handled this tick (checkSignalsAndUpdate
        //     returns early on commitSquashed) and fetch is not still in
        //     Squashing status from a squash processed on the previous
        //     tick (the `fetchStatus[tid] != Squashing` guard).
        if (!ev.commit_squash && !prev_tick_squash) {
            if (ev.decode_redirect_btb_miss) {
                open();
                squash_processed = true;
            } else if (ev.decode_redirect_other) {
                // False-hit / wrong-target redirect: truncates any open
                // window, opens nothing.
                close();
                squash_processed = true;
            }
        }

        // (3) sendInstructionsToDecode (fetch.cc:2392): first delivery of
        //     >= 1 instruction for this tid closes the window.
        if (ev.delivery) {
            close();
        }

        prev_tick_squash = squash_processed;
        ++cur_cycle;
    }

    // Stats-reset ROI boundary (fetch.cc FetchStatGroup::resetStats):
    // the reset zeroes the counters and truncates an open window WITHOUT
    // charging it — no window is billed across the boundary.  Pipeline
    // state (prev_tick_squash) survives: the reset only clears statistics.
    void
    stats_reset()
    {
        btbMissResteers = 0;
        btbMissResteerCycles = 0;
        active = false;
    }
};

} // anonymous namespace

// --- R3 classification truth table (trigger x predBtbHit), 5 rows exact --

// Trigger 1 (false-hit: predicted as branch but not a control): never a
// BTB-miss resteer.  With readPredTaken() true the PC necessarily
// matched a taken predBTBEntry, so predBtbHit is true; the row is pinned
// uncounted for both snapshot values anyway.
TEST(R3Classification, FalseHitNeverCounts)
{
    EXPECT_FALSE(countsAsBtbMissResteer(SelfSquashTrigger::FalseHitNonControl,
                                         true));
    EXPECT_FALSE(countsAsBtbMissResteer(SelfSquashTrigger::FalseHitNonControl,
                                         false));
}

// Trigger 2 (direct branch, predicted target mismatch): counted iff the
// fetch-time snapshot says the branch missed the BTB.
TEST(R3Classification, DirectTargetMismatchExact)
{
    EXPECT_FALSE(countsAsBtbMissResteer(
        SelfSquashTrigger::DirectTargetMismatch, true));   // hit, wrong tgt
    EXPECT_TRUE(countsAsBtbMissResteer(
        SelfSquashTrigger::DirectTargetMismatch, false));  // BTB miss
}

// Trigger 3 (unpredicted return, RAS fixup redirect): counted iff the
// return's PC hit no predBTBEntry.
TEST(R3Classification, ReturnRasFixupExact)
{
    EXPECT_FALSE(countsAsBtbMissResteer(SelfSquashTrigger::ReturnRasFixup,
                                         true));
    EXPECT_TRUE(countsAsBtbMissResteer(SelfSquashTrigger::ReturnRasFixup,
                                       false));
}

// --- R3 snapshot semantics ---------------------------------------------

// PC membership among *valid* predBTBEntries defines the hit; a hit entry
// whose conditional branch is direction-predicted not-taken is still a hit
// (stricter than predict_taken — G4(c) input side).
TEST(R3Snapshot, PcMembershipAmongValidEntriesDefinesHit)
{
    const std::vector<FakeBTBEntry> entries = {
        {true, 0x1000},   // valid hit entry (cond branch, predicted NT)
        {false, 0x2000},  // invalid entry must be ignored
        {true, 0x3000},   // valid entry
    };
    EXPECT_TRUE(predBtbHitSnapshot(0x1000, entries));
    EXPECT_TRUE(predBtbHitSnapshot(0x3000, entries));
    EXPECT_FALSE(predBtbHitSnapshot(0x2000, entries));   // invalid: no hit
    EXPECT_FALSE(predBtbHitSnapshot(0x4000, entries));   // absent: miss
    EXPECT_FALSE(predBtbHitSnapshot(0x1004, entries));   // not an entry pc
}

// G4(c): a BTB *hit* branch predicted not-taken (fallthrough) is NOT a
// BTB-miss resteer.  The snapshot says hit; every classification row with
// predBtbHit true is uncounted, and trigger 2/3 with a hit do not fire
// as BTB-miss redirects (the misprediction, if any, resolves downstream).
TEST(G4cBtbHitFallthrough, NotCountedAsBtbMissResteer)
{
    const std::vector<FakeBTBEntry> entries = {{true, 0x1000}};
    const bool hit = predBtbHitSnapshot(0x1000, entries);
    ASSERT_TRUE(hit);   // entry present although direction predicts NT
    EXPECT_FALSE(countsAsBtbMissResteer(SelfSquashTrigger::DirectTargetMismatch, hit));
    EXPECT_FALSE(countsAsBtbMissResteer(SelfSquashTrigger::ReturnRasFixup, hit));
    EXPECT_FALSE(countsAsBtbMissResteer(SelfSquashTrigger::FalseHitNonControl, hit));
}

// --- G4(d): BTB-miss redirect -> exact event and window accounting ------

// G4(d): a synthetic redirect/delivery sequence with exact expectations.
//   redirect(miss) @0  -> delivery @4  : window = [0,4)  = 4 cycles
//   redirect(miss) @10 -> redirect(miss) @12 (overlap; old window charged
//                         2 by elapsed) -> delivery @15 : +2 +3 cycles
//   redirect(miss) @20 -> commit squash @23 (truncation, charged 3)
//                         -> delivery @28 (no open window)
// Events: 4 classified BTB-miss redirects.  Cycles: 4 + 5 + 3 = 12.
TEST(G4dBtbMissRedirect, ExactEventAndWindowCounts)
{
    BtbMissWindowMachine m;

    // Window 1: [0, 4)
    m.tick({false, true, false, false});          // 0: BTB-miss redirect
    m.tick({});                                    // 1
    m.tick({});                                    // 2
    m.tick({});                                    // 3
    m.tick({false, false, false, true});           // 4: delivery -> 4
    EXPECT_EQ(m.btbMissResteerCycles, 4);
    EXPECT_FALSE(m.active);

    // Idle cycles do not open or extend anything.
    for (int i = 5; i < 10; ++i) {
        m.tick({});
    }
    EXPECT_EQ(m.btbMissResteerCycles, 4);

    // Window 2/3: overlap — redirect @10, superseded @12, delivery @15.
    m.tick({false, true, false, false});           // 10: open @10
    m.tick({});                                    // 11
    m.tick({false, true, false, false});           // 12: close @10(+2), open @12
    m.tick({});                                    // 13
    m.tick({});                                    // 14
    m.tick({false, false, false, true});           // 15: close @12(+3)
    EXPECT_EQ(m.btbMissResteerCycles, 4 + 2 + 3);

    // Window 4: truncated by a commit squash.
    for (int i = 16; i < 20; ++i) {
        m.tick({});
    }
    m.tick({false, true, false, false});           // 20: open @20
    m.tick({});                                    // 21
    m.tick({});                                    // 22
    m.tick({true, false, false, false});           // 23: squash -> truncate(+3)
    EXPECT_EQ(m.btbMissResteerCycles, 4 + 5 + 3);
    EXPECT_FALSE(m.active);
    for (int i = 24; i < 28; ++i) {
        m.tick({});
    }
    m.tick({false, false, false, true});           // 28: delivery, no window
    EXPECT_EQ(m.btbMissResteerCycles, 12);

    // Event count: the four classified BTB-miss redirects (decode side).
    EXPECT_EQ(m.btbMissResteers, 0);   // events charged by decode, not here
}

// Overlap rule dedicated: two redirects, one window each, no cycle is
// charged twice (2 + 3 = 5 == the single-window equivalent [0,5)).
TEST(R3Window, OverlapRedirectsNeverDoubleCharge)
{
    BtbMissWindowMachine split;
    split.tick({false, true, false, false});   // 0: open
    split.tick({});                            // 1
    split.tick({false, true, false, false});   // 2: close(+2), reopen
    split.tick({});                            // 3
    split.tick({});                            // 4
    split.tick({false, false, false, true});   // 5: close(+3)
    EXPECT_EQ(split.btbMissResteerCycles, 5);

    BtbMissWindowMachine single;
    single.tick({false, true, false, false});  // 0: open
    for (int i = 1; i < 5; ++i) {
        single.tick({});
    }
    single.tick({false, false, false, true});  // 5: close(+5)
    EXPECT_EQ(single.btbMissResteerCycles, 5);
    EXPECT_EQ(split.btbMissResteerCycles, single.btbMissResteerCycles);
}

// A redirect and a delivery in the SAME tick charge zero cycles: the
// window is the half-open interval [open, close) and the opening cycle
// delivered (the fetch phase refilled the queue between the redirect
// consumption and sendInstructionsToDecode).
TEST(R3Window, SameTickDeliveryChargesZero)
{
    BtbMissWindowMachine m;
    m.tick({false, true, false, true});   // redirect + delivery in one tick
    EXPECT_EQ(m.btbMissResteerCycles, 0);
    EXPECT_FALSE(m.active);
}

// Guard 1: a decode redirect arriving in the same tick as a commit squash
// is not processed by fetch at all (checkSignalsAndUpdate returns early on
// commitSquashed) — no BTB-miss window opens for it.
TEST(R3Window, DecodeRedirectSkippedWhenCommitSquashSameTick)
{
    BtbMissWindowMachine m;
    m.tick({true, true, false, false});   // squash + redirect signal: skipped
    for (int i = 1; i < 5; ++i) {
        m.tick({});
    }
    m.tick({false, false, false, true});  // 5: delivery, no window
    EXPECT_EQ(m.btbMissResteerCycles, 0);
}

// Guard 2: fetch is still in Squashing status on the tick after a squash
// was processed, so a decode redirect arriving exactly then is skipped
// (pre-existing `fetchStatus[tid] != Squashing` guard) — the window from
// the earlier redirect keeps running.
TEST(R3Window, DecodeRedirectSkippedWhileSquashingFromPreviousTick)
{
    BtbMissWindowMachine m;
    m.tick({false, true, false, false});   // 0: open @0 (Squashing now)
    m.tick({false, true, false, false});   // 1: skipped (still Squashing)
    m.tick({});                            // 2
    m.tick({});                            // 3
    m.tick({false, false, false, true});   // 4: close -> [0,4) = 4
    EXPECT_EQ(m.btbMissResteerCycles, 4);
}

// A non-BTB-miss decode redirect (false-hit / wrong target) truncates an
// open window by its elapsed cycles without opening a new one.
TEST(R3Window, NonMissRedirectTruncatesWithoutReopening)
{
    BtbMissWindowMachine m;
    m.tick({false, true, false, false});   // 0: BTB-miss window opens
    m.tick({});                            // 1
    m.tick({});                            // 2
    m.tick({false, false, true, false});   // 3: other redirect -> truncate(+3)
    EXPECT_EQ(m.btbMissResteerCycles, 3);
    EXPECT_FALSE(m.active);
    m.tick({});                            // 4
    m.tick({false, false, false, true});   // 5: delivery, nothing open
    EXPECT_EQ(m.btbMissResteerCycles, 3);
}

// --- G4(f): ROI stats reset truncation (no cross-segment billing) -------

// G4(f): a window open at the stats-reset boundary is discarded — its
// elapsed cycles are billed to NEITHER segment — and windows opened after
// the boundary are billed wholly to the new segment.
TEST(G4fRoiReset, OpenWindowDiscardedNotBilledAcrossBoundary)
{
    BtbMissWindowMachine m;

    // Segment 1: one closed window (1 cycle) + one window still open at
    // the boundary (opened @5, boundary after tick 8 -> 4 elapsed cycles).
    m.tick({false, true, false, false});   // 0: open @0
    m.tick({false, false, false, true});   // 1: delivery -> [0,1) = 1
    EXPECT_EQ(m.btbMissResteerCycles, 1);
    for (int i = 2; i < 5; ++i) {
        m.tick({});
    }
    m.tick({false, true, false, false});   // 5: open @5
    for (int i = 6; i <= 8; ++i) {
        m.tick({});
    }
    EXPECT_TRUE(m.active);

    // Stats reset (dump+reset at the boundary): counters zeroed, open
    // window truncated WITHOUT charging.
    m.stats_reset();
    EXPECT_FALSE(m.active);
    EXPECT_EQ(m.btbMissResteerCycles, 0);

    // Segment 2: delivery of the pre-boundary path closes nothing; a new
    // redirect/delivery pair is billed wholly inside segment 2.
    m.tick({false, false, false, true});   // 9: delivery, no open window
    EXPECT_EQ(m.btbMissResteerCycles, 0);
    m.tick({});                            // 10
    m.tick({false, true, false, false});   // 11: open @11
    m.tick({});                            // 12
    m.tick({});                            // 13
    m.tick({false, false, false, true});   // 14: close -> 3
    EXPECT_EQ(m.btbMissResteerCycles, 3);

    // The 4 pre-boundary elapsed cycles (5..8) appear in neither segment:
    // segment 1 dumped 1, segment 2 shows 3 (all from cycles 11..13).
}

// --- SMT: per-tid independent windows -----------------------------------

// SMT: the window state machine is per-tid; two threads' windows never
// interact (delivery on one tid does not close the other's window).
TEST(R3WindowSmt, IndependentPerTidWindows)
{
    BtbMissWindowMachine tid0;
    BtbMissWindowMachine tid1;

    tid0.tick({false, true, false, false});   // 0: tid0 redirect
    tid1.tick({false, true, false, false});   // 0: tid1 redirect
    tid0.tick({});                            // 1
    tid1.tick({false, false, false, true});   // 1: tid1 delivers -> 1
    EXPECT_EQ(tid1.btbMissResteerCycles, 1);
    tid0.tick({});                            // 2
    tid0.tick({});                            // 3
    tid0.tick({false, false, false, true});   // 4: tid0 delivers -> 4
    EXPECT_EQ(tid0.btbMissResteerCycles, 4);
    EXPECT_EQ(tid1.btbMissResteerCycles, 1);
}
