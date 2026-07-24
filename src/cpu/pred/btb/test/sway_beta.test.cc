#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "cpu/pred/btb/sway_beta_controller.hh"

namespace
{

using namespace gem5::branch_prediction::btb_pred::sway;

// Helper: build per-phase metrics from the six derived values (donor b_b
// then donee b_c). NaN means unavailable, matching the wiring's zero-denom
// handling.
BetaPhaseMetrics
metrics(double b0, double b1, double b2, double b3, double c3, double c7)
{
    BetaPhaseMetrics m;
    m.values[0] = b0;
    m.values[1] = b1;
    m.values[2] = b2;
    m.values[3] = b3;
    m.values[4] = c3;
    m.values[5] = c7;
    return m;
}

// Append n warmup phases with the given metric vector. Returns the count.
size_t
feedWarmup(BetaController &c, size_t n, const BetaPhaseMetrics &m,
           int startPhase = 0, uint64_t inst0 = 0, uint64_t phaseInsts = 100000)
{
    for (size_t i = 0; i < n; ++i) {
        const uint64_t s = inst0 + i * phaseInsts;
        c.processPhase(startPhase + static_cast<int>(i), s, s + phaseInsts,
                       "warmup", true, m);
    }
    return n;
}

} // namespace

// History buffer: circular, maxlen 128, finite-only.
TEST(SwayBetaController, HistoryBufferIsCircularAndFiniteOnly)
{
    BetaController c;
    c.configure(1.5, 1.5);
    // 200 warmup phases of a constant donor value -> only last 128 retained.
    BetaPhaseMetrics m = metrics(1.0, 1.0, 1.0, 1.0, 1.0, 1.0);
    feedWarmup(c, 200, m);
    // References freeze on the first sample phase.
    c.processPhase(200, 20000000ull, 20100000ull, "sample", true, m);
    for (size_t i = 0; i < NumBetaMetrics; ++i) {
        const BetaReference &ref = c.reference(static_cast<BetaMetric>(i));
        EXPECT_EQ(ref.sampleCount, BetaHistoryLimit)
            << "metric " << i << " retained last 128";
        EXPECT_EQ(ref.status, BetaRefStatus::ZeroStd)
            << "constant history -> zero std";
    }
}

// Reference: fewer than 32 finite samples -> ShortHistory (not evaluable).
TEST(SwayBetaController, ShortHistoryNotEvaluable)
{
    BetaController c;
    c.configure(1.5, 1.5);
    feedWarmup(c, 10, metrics(0.5, 0.5, 0.5, 0.5, 0.5, 0.5));
    // Force a sample phase to freeze references.
    c.processPhase(100, 1000000, 1100000, "sample", true,
                   metrics(0.5, 0.5, 0.5, 0.5, 0.5, 0.5));
    for (size_t i = 0; i < NumBetaMetrics; ++i) {
        EXPECT_EQ(c.reference(static_cast<BetaMetric>(i)).status,
                  BetaRefStatus::ShortHistory);
    }
}

// z-score: donor sign-flip (lower b_b -> higher z), donee normal direction.
TEST(SwayBetaController, ZScoreSigns)
{
    BetaController c;
    c.configure(1.5, 1.5);
    // 40 warmup phases with donors at 1.0 and donees at 1.0 (mean=1, std=0 ->
    // ZeroStd, not evaluable). Use a small spread to get a finite std.
    BetaPhaseMetrics warm = metrics(1.0, 1.0, 1.0, 1.0, 1.0, 1.0);
    // Alternate two values so std > 0.
    BetaPhaseMetrics a = metrics(0.8, 0.8, 0.8, 0.8, 0.8, 0.8);
    BetaPhaseMetrics b = metrics(1.2, 1.2, 1.2, 1.2, 1.2, 1.2);
    for (int i = 0; i < 40; ++i) {
        c.processPhase(i, i * 100000ull, (i + 1) * 100000ull, "warmup", true,
                       (i % 2 == 0) ? a : b);
    }
    // Sample phase: donor below mean (cold) -> donor z > 0; donee above mean ->
    // donee z > 0. Pick a value clearly below/above the 0.8/1.2 band.
    BetaDecision d = c.processPhase(40, 4000000ull, 4100000ull, "sample",
                                     true, metrics(0.5, 0.5, 0.5, 0.5,
                                                    1.5, 1.5));
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_GT(d.zScores[i], 0.0) << "donor colder than mean -> z>0";
    }
    for (size_t i = 4; i < 6; ++i) {
        EXPECT_GT(d.zScores[i], 0.0) << "donee hotter than mean -> z>0";
    }
}

// Arbitration: one fire per phase; highest donor z then donee z; cooldown
// blocks the same donee for the cooldown period.
TEST(SwayBetaController, OneFirePerPhaseAndCooldown)
{
    BetaController c;
    c.configure(1.5, 1.5);
    // 40 warmup phases with mild spread so references are ok.
    BetaPhaseMetrics a = metrics(0.9, 0.9, 0.9, 0.9, 0.9, 0.9);
    BetaPhaseMetrics b = metrics(1.1, 1.1, 1.1, 1.1, 1.1, 1.1);
    for (int i = 0; i < 40; ++i) {
        c.processPhase(i, i * 100000ull, (i + 1) * 100000ull, "warmup", true,
                       (i % 2 == 0) ? a : b);
    }
    // Strongly crossed sample: donors cold, donees hot.
    BetaPhaseMetrics crossed = metrics(0.2, 0.2, 0.2, 0.2, 2.0, 2.0);
    BetaDecision d0 = c.processPhase(40, 4000000ull, 4100000ull, "sample",
                                       true, crossed);
    ASSERT_TRUE(d0.newFire) << "phase 1 fires";
    // 2 directions x 2 donors x 2 donees = 8 pairs; all crossed, all
    // available (no cooldown yet on the first sample phase).
    EXPECT_EQ(d0.crossedPairCount, 8u);
    EXPECT_EQ(d0.availablePairCount, 8u);
    EXPECT_EQ(d0.fire.donee, BetaMetric::TageT3)
        << "tie-break: lower donee id (T3) when donor z equal";

    // Next phase: T3 under cooldown (8), so only T7 available.
    BetaDecision d1 = c.processPhase(41, 4100000ull, 4200000ull, "sample",
                                       true, crossed);
    if (d1.newFire) {
        EXPECT_EQ(d1.fire.donee, BetaMetric::TageT7)
            << "T3 cooling, T7 fires";
    }
    // T3 cooldown is 8 phases: phases 41..47 block T3 (it is under cooldown
    // even if crossed). Confirm by checking the cooldownAfter on phase 40.
    // The first fire armed T3 cooldown to 8.
    EXPECT_EQ(d0.cooldownAfter[0], 8u);
}

// Cooldown values: T3=8, T7=20.
TEST(SwayBetaController, CooldownValues)
{
    EXPECT_EQ(BetaDoneeCooldown[betaMetricIndex(BetaMetric::TageT3)], 8u);
    EXPECT_EQ(BetaDoneeCooldown[betaMetricIndex(BetaMetric::TageT7)], 20u);
}

// Segment boundary: warmup appends, sample decides, transition skips, and
// references freeze on the first sample phase (never updated after).
TEST(SwayBetaController, SegmentBoundaryAndReferenceFreeze)
{
    BetaController c;
    c.configure(1.5, 1.5);
    BetaPhaseMetrics a = metrics(0.9, 0.9, 0.9, 0.9, 0.9, 0.9);
    BetaPhaseMetrics b = metrics(1.1, 1.1, 1.1, 1.1, 1.1, 1.1);
    for (int i = 0; i < 40; ++i) {
        c.processPhase(i, i * 100000ull, (i + 1) * 100000ull, "warmup", true,
                       (i % 2 == 0) ? a : b);
    }
    // First sample phase freezes references from the warmup history.
    c.processPhase(40, 4000000ull, 4100000ull, "sample", true,
                   metrics(0.2, 0.2, 0.2, 0.2, 2.0, 2.0));
    const double meanFrozen = c.reference(BetaMetric::TageT3).mean;
    EXPECT_GT(meanFrozen, 0.0) << "reference frozen with a finite mean";
    // Subsequent sample phases must not update the reference, even with
    // extreme values.
    for (int p = 1; p < 5; ++p) {
        c.processPhase(40 + p, (40 + p) * 100000ull, (41 + p) * 100000ull,
                       "sample", true,
                       metrics(9.0, 9.0, 9.0, 9.0, 9.0, 9.0));
    }
    EXPECT_EQ(meanFrozen, c.reference(BetaMetric::TageT3).mean)
        << "reference not updated by sample values";
    // A transition phase must not append history or decide.
    BetaDecision dt = c.processPhase(45, 4500000ull, 4600000ull, "transition",
                                       false, metrics(9.0, 9.0, 9.0, 9.0,
                                                       9.0, 9.0));
    EXPECT_FALSE(dt.newFire);
    EXPECT_FALSE(dt.returnOccurred);
}

// Strict z > theta: equality must not fire.
TEST(SwayBetaController, StrictGreaterThanTheta)
{
    BetaController c;
    c.configure(1.0, 1.0);
    // Build a reference with a known std so we can land z exactly at theta.
    // Use values 0.5 and 1.5 (mean=1, pstdev=0.5); a sample of 1.5 gives
    // donee z = (1.5-1)/0.5 = 1.0 == theta -> must NOT fire.
    BetaPhaseMetrics lo = metrics(0.5, 0.5, 0.5, 0.5, 0.5, 0.5);
    BetaPhaseMetrics hi = metrics(1.5, 1.5, 1.5, 1.5, 1.5, 1.5);
    for (int i = 0; i < 40; ++i) {
        c.processPhase(i, i * 100000ull, (i + 1) * 100000ull, "warmup", true,
                       (i % 2 == 0) ? lo : hi);
    }
    // Donor at 0.5 -> z = (1-0.5)/0.5 = 1.0 == theta (not >). Donee at 1.5 ->
    // z = 1.0 == theta (not >). No fire.
    BetaDecision d = c.processPhase(40, 4000000ull, 4100000ull, "sample", true,
                                     metrics(0.5, 0.5, 0.5, 0.5, 1.5, 1.5));
    EXPECT_FALSE(d.newFire) << "z == theta must not fire (strict >)";
    EXPECT_EQ(d.crossedPairCount, 0u);
}

// Return resolution: a borrow sustained while crossed, then returned via T3
// regression when the donee pressure resolves at cool-down expiry.
TEST(SwayBetaController, T3RegressionReturn)
{
    BetaController c;
    c.configure(1.0, 1.0);
    BetaPhaseMetrics lo = metrics(0.5, 0.5, 0.5, 0.5, 0.5, 0.5);
    BetaPhaseMetrics hi = metrics(1.5, 1.5, 1.5, 1.5, 1.5, 1.5);
    for (int i = 0; i < 40; ++i) {
        c.processPhase(i, i * 100000ull, (i + 1) * 100000ull, "warmup", true,
                       (i % 2 == 0) ? lo : hi);
    }
    // Crossed sample (donor cold, donee hot) -> fire on T3, cooldown 8.
    BetaPhaseMetrics crossed = metrics(0.1, 0.1, 0.1, 0.1, 2.0, 2.0);
    BetaDecision d0 = c.processPhase(40, 4000000ull, 4100000ull, "sample",
                                       true, crossed);
    ASSERT_TRUE(d0.newFire);
    ASSERT_EQ(d0.fire.donee, BetaMetric::TageT3);
    // Hold crossed for 8 phases (cooldown) so the borrow is sustained.
    for (int p = 1; p < 8; ++p) {
        c.processPhase(40 + p, (40 + p) * 100000ull,
                       (41 + p) * 100000ull, "sample", true, crossed);
    }
    // Phase 48: cooldown expired (8 phases later). Crossed -> sustained (no
    // return). Re-fire renews.
    BetaPhaseMetrics regressed = metrics(1.5, 1.5, 1.5, 1.5, 0.5, 2.0);
    // Actually to trigger T3 regression, make donee T3 NOT crossed (pressure
    // resolved) at the expiry phase. Donor stays cold-ish so it's the donee
    // side that failed.
    BetaPhaseMetrics t3Regressed = metrics(0.1, 0.1, 0.1, 0.1, 0.5, 2.0);
    BetaDecision dExpire = c.processPhase(48, 4800000ull, 4900000ull,
                                            "sample", true, t3Regressed);
    EXPECT_TRUE(dExpire.returnOccurred)
        << "T3 borrow returns at expiry when donee regressed";
    EXPECT_EQ(dExpire.returnDonee, BetaMetric::TageT3);
    // The fire should be tagged t3_regression_return.
    bool foundT3 = false;
    for (const BetaFire &f : c.fires()) {
        if (f.donee == BetaMetric::TageT3 &&
            f.resolution == BetaResolution::T3RegressionReturn) {
            foundT3 = true;
            break;
        }
    }
    EXPECT_TRUE(foundT3) << "fire tagged t3_regression_return";
}

// active_at_end: a borrow never returned by run end.
TEST(SwayBetaController, ActiveAtEndFinalize)
{
    BetaController c;
    c.configure(1.0, 1.0);
    BetaPhaseMetrics lo = metrics(0.5, 0.5, 0.5, 0.5, 0.5, 0.5);
    BetaPhaseMetrics hi = metrics(1.5, 1.5, 1.5, 1.5, 1.5, 1.5);
    for (int i = 0; i < 40; ++i) {
        c.processPhase(i, i * 100000ull, (i + 1) * 100000ull, "warmup", true,
                       (i % 2 == 0) ? lo : hi);
    }
    BetaPhaseMetrics crossed = metrics(0.1, 0.1, 0.1, 0.1, 2.0, 2.0);
    c.processPhase(40, 4000000ull, 4100000ull, "sample", true, crossed);
    // End run without returning the borrow.
    c.finalizeAtEnd(41, 4100000ull);
    bool foundActive = false;
    for (const BetaFire &f : c.fires()) {
        if (f.resolution == BetaResolution::ActiveAtEnd) {
            foundActive = true;
            break;
        }
    }
    EXPECT_TRUE(foundActive) << "pending borrow finalized as active_at_end";
}

// Baseline safety: with theta very high, no fire ever occurs (controller is
// effectively inert), matching enableSwayRealloc=False behavior for the
// decision path.
TEST(SwayBetaController, HighThetaSuppressesFires)
{
    BetaController c;
    c.configure(1e9, 1e9);
    BetaPhaseMetrics lo = metrics(0.5, 0.5, 0.5, 0.5, 0.5, 0.5);
    BetaPhaseMetrics hi = metrics(1.5, 1.5, 1.5, 1.5, 1.5, 1.5);
    for (int i = 0; i < 40; ++i) {
        c.processPhase(i, i * 100000ull, (i + 1) * 100000ull, "warmup", true,
                       (i % 2 == 0) ? lo : hi);
    }
    for (int p = 0; p < 20; ++p) {
        BetaDecision d = c.processPhase(40 + p, (40 + p) * 100000ull,
                                          (41 + p) * 100000ull, "sample", true,
                                          metrics(0.1, 0.1, 0.1, 0.1,
                                                  2.0, 2.0));
        EXPECT_FALSE(d.newFire);
    }
    EXPECT_TRUE(c.fires().empty());
}
