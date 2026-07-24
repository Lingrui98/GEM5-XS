#include "cpu/pred/btb/sway_beta_controller.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <tuple>
#include <vector>

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace sway
{

namespace
{
// Population mean of a finite-only sample (matches statistics.fmean over the
// finite values). Returns NaN if the sample is empty.
double
fmeanFinite(const std::vector<double> &values)
{
    double sum = 0.0;
    uint64_t n = 0;
    for (double v : values) {
        if (std::isfinite(v)) {
            sum += v;
            ++n;
        }
    }
    if (n == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return sum / static_cast<double>(n);
}

// Population standard deviation (ddof=0) about a known mean, over finite
// values only. Matches statistics.pstdev(values, mu=mean). Returns 0.0 if
// there are no finite values; the caller treats stddev==0 as ZeroStd.
double
pstdevFinite(const std::vector<double> &values, double mean)
{
    double sumsq = 0.0;
    uint64_t n = 0;
    for (double v : values) {
        if (std::isfinite(v)) {
            const double d = v - mean;
            sumsq += d * d;
            ++n;
        }
    }
    if (n == 0) {
        return 0.0;
    }
    return std::sqrt(sumsq / static_cast<double>(n));
}
} // namespace

void
BetaController::appendWarmupHistory(const BetaPhaseMetrics &metrics)
{
    for (size_t i = 0; i < NumBetaMetrics; ++i) {
        const double v = metrics.values[i];
        if (std::isfinite(v)) {
            histories_[i].append(v);
        }
    }
}

void
BetaController::freezeReferences()
{
    if (referencesFrozen_) {
        return;
    }
    referencesFrozen_ = true;
    for (size_t i = 0; i < NumBetaMetrics; ++i) {
        BetaReference &ref = references_[i];
        const std::vector<double> values = histories_[i].contents();
        uint64_t finiteCount = 0;
        for (double v : values) {
            if (std::isfinite(v)) {
                ++finiteCount;
            }
        }
        ref.sampleCount = finiteCount;
        if (finiteCount < BetaMinHistory) {
            ref.status = BetaRefStatus::ShortHistory;
            ref.mean = 0.0;
            ref.stddev = 0.0;
            continue;
        }
        const double mean = fmeanFinite(values);
        const double stddev = pstdevFinite(values, mean);
        ref.mean = mean;
        ref.stddev = stddev;
        if (stddev == 0.0) {
            ref.status = BetaRefStatus::ZeroStd;
        } else {
            ref.status = BetaRefStatus::Ok;
        }
    }
}

void
BetaController::computeZScores(
    const BetaPhaseMetrics &metrics,
    std::array<double, NumBetaMetrics> &scores,
    std::array<BetaRefStatus, NumBetaMetrics> &statuses) const
{
    scores.fill(betaNA());
    statuses.fill(BetaRefStatus::SampleNa);
    for (size_t i = 0; i < NumBetaMetrics; ++i) {
        const BetaReference &ref = references_[i];
        if (ref.status != BetaRefStatus::Ok) {
            // short_history or zero_std: not evaluable; carry the reason.
            statuses[i] = ref.status;
            continue;
        }
        const double value = metrics.values[i];
        if (!std::isfinite(value)) {
            statuses[i] = BetaRefStatus::SampleNa;
            continue;
        }
        // Donor metrics (b_b) are sign-flipped: lower B-b = colder = better
        // donor, so z > 0 means "colder than baseline". Donee metrics (b_c)
        // use the conventional (value - mean) / std.
        double score;
        if (betaMetricIsDonor(static_cast<BetaMetric>(i))) {
            score = (ref.mean - value) / ref.stddev;
        } else {
            score = (value - ref.mean) / ref.stddev;
        }
        if (!std::isfinite(score)) {
            statuses[i] = BetaRefStatus::SampleNa;
            continue;
        }
        scores[i] = score;
        statuses[i] = BetaRefStatus::Ok;
    }
}

void
BetaController::beginPhase()
{
    for (size_t i = 0; i < doneeCooldown_.size(); ++i) {
        if (doneeCooldown_[i] > 0) {
            --doneeCooldown_[i];
        }
    }
}

void
BetaController::resolveBorrows(
    int phaseId, uint64_t phaseStartInst, uint64_t phaseEndInst,
    const BetaPhaseMetrics &metrics,
    const std::array<double, NumBetaMetrics> &scores,
    const std::array<BetaRefStatus, NumBetaMetrics> &statuses,
    bool &returnOccurred,
    BetaMetric &returnDonor,
    BetaMetric &returnDonee)
{
    (void)metrics;
    returnOccurred = false;
    // D-Return: returns are evaluated at per-donee cool-down expiry (T2
    // re-eval primary). A borrow whose donee cool-down has just reached zero
    // is re-evaluated: if the pair still jointly crosses, the donation is
    // sustained (arbitration may re-fire it); otherwise the borrowed way
    // returns, tagged by the failing side — T3 regression if the donee
    // pressure resolved, T4 donor pressure if the donor is no longer cold,
    // else the generic cool-down-expiry return. At most one return per phase
    // (one-action-per-phase substrate cadence); the first eligible donee wins.
    for (size_t slot = 0; slot < activeBorrowForDonee_.size(); ++slot) {
        if (!activeBorrowForDonee_[slot]) {
            continue;
        }
        // Only re-evaluate at cool-down expiry (cooldown reached 0 this phase).
        if (doneeCooldown_[slot] != 0) {
            continue;
        }
        const BetaMetric doneeMetric =
            (slot == 1) ? BetaMetric::TageT7 : BetaMetric::TageT3;
        // The borrow's donor metric is the donor of the most recent pending
        // fire on this donee (stable within a borrow by construction).
        BetaMetric donorMetric = BetaMetric::MbtbBank0Way3;
        bool foundDonor = false;
        for (auto it = fires_.rbegin(); it != fires_.rend(); ++it) {
            if (it->resolution == BetaResolution::Pending &&
                it->donee == doneeMetric) {
                donorMetric = it->donor;
                foundDonor = true;
                break;
            }
        }
        if (!foundDonor) {
            continue;
        }
        const double zDonor = scores[betaMetricIndex(donorMetric)];
        const double zDonee = scores[betaMetricIndex(doneeMetric)];
        const bool donorOk = statuses[betaMetricIndex(donorMetric)] ==
                              BetaRefStatus::Ok;
        const bool doneeOk = statuses[betaMetricIndex(doneeMetric)] ==
                              BetaRefStatus::Ok;
        const bool crossed = donorOk && doneeOk &&
            zDonor > thetaDonor_ && zDonee > thetaDonee_;
        if (crossed) {
            // Sustained: the donation continues; arbitration may re-fire.
            continue;
        }
        BetaResolution tag;
        if (!doneeOk || zDonee <= thetaDonee_) {
            tag = BetaResolution::T3RegressionReturn;
        } else if (!donorOk || zDonor <= thetaDonor_) {
            tag = BetaResolution::T4DonorPressureReturn;
        } else {
            tag = BetaResolution::CooldownExpiryReturn;
        }
        for (BetaFire &f : fires_) {
            if (f.resolution == BetaResolution::Pending &&
                f.donee == doneeMetric) {
                f.resolution = tag;
                f.returnPhaseId = phaseId;
                f.returnPhaseStartInst = phaseStartInst;
                f.returnPhaseEndInst = phaseEndInst;
            }
        }
        activeBorrowForDonee_[slot] = false;
        returnOccurred = true;
        returnDonor = donorMetric;
        returnDonee = doneeMetric;
        // One return per phase: stop after the first resolved donee.
        return;
    }
}

BetaDecision
BetaController::processPhase(int phaseId, uint64_t phaseStartInst,
                              uint64_t phaseEndInst,
                              const std::string &segment,
                              bool replayEligible,
                              const BetaPhaseMetrics &metrics)
{
    BetaDecision decision;
    if (segment == "warmup") {
        if (replayEligible) {
            appendWarmupHistory(metrics);
        }
        lastSegment_ = "warmup";
        return decision;
    }
    if (segment == "transition" || !replayEligible) {
        lastSegment_ = "transition";
        return decision;
    }
    // sample + eligible: freeze references on the first sample phase, then
    // decide every sample phase. References are never updated during the
    // sample segment (matches replay_beta._build_references).
    if (!referencesFrozen_) {
        freezeReferences();
    }
    lastSegment_ = "sample";

    beginPhase();
    decision.cooldownBefore[0] = doneeCooldown_[0];
    decision.cooldownBefore[1] = doneeCooldown_[1];

    computeZScores(metrics, decision.zScores, decision.statuses);

    bool returnOccurred = false;
    BetaMetric returnDonor = BetaMetric::MbtbBank0Way3;
    BetaMetric returnDonee = BetaMetric::TageT3;
    resolveBorrows(phaseId, phaseStartInst, phaseEndInst, metrics,
                   decision.zScores, decision.statuses,
                   returnOccurred, returnDonor, returnDonee);

    if (returnOccurred) {
        // A return is this phase's one realloc action; do not also fire.
        decision.returnOccurred = true;
        decision.returnDonor = returnDonor;
        decision.returnDonee = returnDonee;
        decision.cooldownAfter[0] = doneeCooldown_[0];
        decision.cooldownAfter[1] = doneeCooldown_[1];
        return decision;
    }

    // Build the global available-pair list across both directions, mirroring
    // replay_beta.replay_workload. A pair is available iff both metrics are ok
    // this phase, both z strictly exceed theta, the donee is not under
    // cool-down, and the donee does not already hold an active borrow.
    struct Pair
    {
        BetaDirection direction;
        BetaMetric donor;
        BetaMetric donee;
        double zDonor;
        double zDonee;
    };
    std::vector<Pair> crossed;
    std::vector<Pair> available;
    uint32_t evaluablePairCount = 0;
    bool anyEvaluable = false;

    const std::array<BetaDirection, 2> directions = {
        BetaDirection::MbtbToTage, BetaDirection::IttageToTage
    };
    for (BetaDirection d : directions) {
        const auto &donors = betaDonorsForDirection(d);
        for (BetaMetric donor : donors) {
            for (BetaMetric donee : TageDonees) {
                const uint8_t di = betaMetricIndex(donor);
                const uint8_t ei = betaMetricIndex(donee);
                const bool donorOk = decision.statuses[di] ==
                                     BetaRefStatus::Ok;
                const bool doneeOk = decision.statuses[ei] ==
                                     BetaRefStatus::Ok;
                if (donorOk && doneeOk) {
                    ++evaluablePairCount;
                    anyEvaluable = true;
                }
                if (!(donorOk && doneeOk)) {
                    continue;
                }
                const double zD = decision.zScores[di];
                const double zE = decision.zScores[ei];
                if (!(zD > thetaDonor_ && zE > thetaDonee_)) {
                    continue;
                }
                crossed.push_back({d, donor, donee, zD, zE});
                if (cooldownSlotRead(donee) != 0) {
                    continue;
                }
                // A donee holding an active borrow that was sustained at this
                // phase's cool-down expiry remains available: a re-fire renews
                // the borrow (re-arms cool-down, no new way transfer). A donee
                // with no active borrow fires as a new donation. Borrows still
                // cooling (cooldown>0) are excluded above.
                available.push_back({d, donor, donee, zD, zE});
            }
        }
    }

    decision.evaluablePairs = anyEvaluable;
    decision.crossedPairCount = static_cast<uint32_t>(crossed.size());
    decision.availablePairCount = static_cast<uint32_t>(available.size());
    decision.cooldownBlocked = !crossed.empty() && available.empty();

    if (available.empty()) {
        decision.cooldownAfter[0] = doneeCooldown_[0];
        decision.cooldownAfter[1] = doneeCooldown_[1];
        return decision;
    }

    // Select by donor z, then donee z, then stable direction/donor/donee id
    // (lower index wins on ties -> negate for max). Identical to
    // replay_beta._select_winning_pair.
    auto directionIndex = [](BetaDirection d) {
        return d == BetaDirection::IttageToTage ? 1 : 0;
    };
    auto key = [&](const Pair &p) {
        return std::make_tuple(
            p.zDonor, p.zDonee,
            -static_cast<int>(directionIndex(p.direction)),
            -static_cast<int>(betaMetricIndex(p.donor)),
            -static_cast<int>(betaMetricIndex(p.donee)));
    };
    Pair winner = available.front();
    auto bestKey = key(winner);
    for (const auto &p : available) {
        const auto k = key(p);
        if (k > bestKey) {
            bestKey = k;
            winner = p;
        }
    }

    // Fire. A donee that already holds an active borrow (sustained across the
    // expiry re-eval above) cannot be in `available`, so this is a new
    // donation: the way actually transfers, a borrow is opened, and the
    // donee cool-down is armed.
    const bool isNewDonation = !activeBorrowSlotRead(winner.donee);
    activeBorrowSlot(winner.donee) = true;
    cooldownSlot(winner.donee) = cooldownForDonee(winner.donee);

    BetaFire fire;
    fire.phaseId = phaseId;
    fire.phaseStartInst = phaseStartInst;
    fire.phaseEndInst = phaseEndInst;
    fire.direction = winner.direction;
    fire.donor = winner.donor;
    fire.donee = winner.donee;
    fire.zDonor = winner.zDonor;
    fire.zDonee = winner.zDonee;
    fire.thetaDonor = thetaDonor_;
    fire.thetaDonee = thetaDonee_;
    fire.isNewDonation = isNewDonation;
    fire.resolution = BetaResolution::Pending;
    fires_.push_back(fire);

    decision.newFire = true;
    decision.fire = fire;
    decision.isNewDonation = isNewDonation;
    decision.cooldownAfter[0] = doneeCooldown_[0];
    decision.cooldownAfter[1] = doneeCooldown_[1];
    return decision;
}

void
BetaController::finalizeAtEnd(int endPhaseId, uint64_t endInst)
{
    for (BetaFire &f : fires_) {
        if (f.resolution == BetaResolution::Pending) {
            f.resolution = BetaResolution::ActiveAtEnd;
            f.returnPhaseId = endPhaseId;
            f.returnPhaseStartInst = endInst;
            f.returnPhaseEndInst = endInst;
        }
    }
}

} // namespace sway

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
