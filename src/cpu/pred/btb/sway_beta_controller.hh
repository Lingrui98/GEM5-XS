#ifndef __CPU_PRED_BTB_SWAY_BETA_CONTROLLER_HH__
#define __CPU_PRED_BTB_SWAY_BETA_CONTROLLER_HH__

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace sway
{

/**
 * @brief Self-normalized beta decision engine — the live in-sim port of the
 *        Stage 3 open-loop replay policy (util/xs_scripts/sway/replay_beta.py).
 *
 * The locked metric/decision semantics live in
 * `~/project/papers/sway-paper/docs/SWAY_METRIC_TAXONOMY.md` §3-§5 and
 * `.trellis/spec/backend/sway-phase-stream-contract.md`. This class is a
 * faithful C++ port of the offline replay so that live metrics are the same
 * quantities the open-loop replay consumed. Do not change the derived metric
 * formulas, the 128/32 history bounds, the population std (ddof=0), the strict
 * z > theta test, the global one-fire-per-phase arbitration, the stable
 * tie-break order, or the T3=8 / T7=20 per-donee cooldowns.
 *
 * The engine is pure logic (no gem5 dependency) so the history buffer,
 * z-score, arbitration, cooldown, segment-boundary, and return-resolution
 * behavior can be unit-tested directly against replay_beta.py fixtures.
 */

static constexpr uint8_t NumBetaMetrics = 6;
static constexpr size_t BetaHistoryLimit = 128;
static constexpr size_t BetaMinHistory = 32;

// Per-donee cooldowns in phases. Must match replay_beta.py DONEE_COOLDOWN
// (T3=8, T7=20) and sway::tageDoneeCooldownPhases for tables 3 and 7.
static constexpr std::array<unsigned, NumBetaMetrics> BetaDoneeCooldown = {
    0, 0, 0, 0, 8, 20
};

// Metric identifiers, in the exact order of replay_beta.py ALL_METRICS so that
// donor/donee tie-break indices are stable and identical to the open-loop
// replay.
//   0: b_b_mbtb_bank0_way3   (donor, MBTB->TAGE)
//   1: b_b_mbtb_bank1_way3   (donor, MBTB->TAGE)
//   2: b_b_ittage_t2         (donor, ITTAGE->TAGE)
//   3: b_b_ittage_t3         (donor, ITTAGE->TAGE)
//   4: b_c_tage_t3           (donee)
//   5: b_c_tage_t7           (donee)
enum class BetaMetric : uint8_t
{
    MbtbBank0Way3 = 0,
    MbtbBank1Way3 = 1,
    IttageT2 = 2,
    IttageT3 = 3,
    TageT3 = 4,
    TageT7 = 5
};

enum class BetaDirection : uint8_t
{
    MbtbToTage = 0,
    IttageToTage = 1
};

static constexpr std::array<BetaMetric, 2> MbtbDonors = {
    BetaMetric::MbtbBank0Way3, BetaMetric::MbtbBank1Way3
};
static constexpr std::array<BetaMetric, 2> IttageDonors = {
    BetaMetric::IttageT2, BetaMetric::IttageT3
};
static constexpr std::array<BetaMetric, 2> TageDonees = {
    BetaMetric::TageT3, BetaMetric::TageT7
};

inline bool
betaMetricIsDonor(BetaMetric m)
{
    return m == BetaMetric::MbtbBank0Way3 ||
           m == BetaMetric::MbtbBank1Way3 ||
           m == BetaMetric::IttageT2 ||
           m == BetaMetric::IttageT3;
}

inline BetaDirection
betaDirectionForDonor(BetaMetric donor)
{
    return (donor == BetaMetric::IttageT2 || donor == BetaMetric::IttageT3)
        ? BetaDirection::IttageToTage
        : BetaDirection::MbtbToTage;
}

inline const std::array<BetaMetric, 2>&
betaDonorsForDirection(BetaDirection d)
{
    return d == BetaDirection::IttageToTage ? IttageDonors : MbtbDonors;
}

inline uint8_t
betaMetricIndex(BetaMetric m)
{
    return static_cast<uint8_t>(m);
}

inline const char *
betaMetricLabel(BetaMetric m)
{
    switch (m) {
      case BetaMetric::MbtbBank0Way3: return "MBTB.bank0.way3";
      case BetaMetric::MbtbBank1Way3: return "MBTB.bank1.way3";
      case BetaMetric::IttageT2:       return "ITTAGE.T2";
      case BetaMetric::IttageT3:       return "ITTAGE.T3";
      case BetaMetric::TageT3:         return "TAGE.T3";
      case BetaMetric::TageT7:         return "TAGE.T7";
    }
    return "invalid";
}

inline const char *
betaDirectionLabel(BetaDirection d)
{
    return d == BetaDirection::IttageToTage ? "ITTAGE->TAGE" : "MBTB->TAGE";
}

// Sentinel encoding a missing derived metric value (zero denominator, or a
// non-finite sample). Mirrors replay_beta.py's None. Use isBetaNA() to test.
inline double betaNA() { return std::numeric_limits<double>::quiet_NaN(); }
inline bool isBetaNA(double v) { return !std::isfinite(v); }

/**
 * Per-phase derived B-b / B-c metric values. Indexed by BetaMetric.
 * A value of betaNA() means "unavailable" (zero denominator or non-finite).
 *
 * The caller derives these with EXACTLY the replay_beta.py arithmetic:
 *   b_b_mbtb_bank{0,1}_way3 = hits[bank.way3] / mean(8 MBTB way hits)
 *   b_b_ittage_t{2,3}       = updates[t] / mean(ITTAGE T0..T4 updates)
 *   b_c_tage_t{3,7}          = (evicts / (updates + alloc_success))
 *                              * (resident / capacity)
 * Zero denominators produce betaNA(), never 0.0.
 */
struct BetaPhaseMetrics
{
    std::array<double, NumBetaMetrics> values{};
    BetaPhaseMetrics() { values.fill(betaNA()); }
};

enum class BetaRefStatus : uint8_t
{
    Ok = 0,
    ShortHistory = 1,
    ZeroStd = 2,
    // Per-phase only (never stored on a BetaReference): the reference is ok but
    // this phase's derived value is unavailable or non-finite.
    SampleNa = 3
};

struct BetaReference
{
    BetaRefStatus status{BetaRefStatus::ShortHistory};
    uint64_t sampleCount{0};
    double mean{0.0};
    double stddev{0.0}; // population standard deviation (ddof=0)
};

enum class BetaResolution : uint8_t
{
    Pending = 0,
    CooldownExpiryReturn = 1, // T2: cool-down-expiry re-eval primary
    T3RegressionReturn = 2,   // T3: donee pressure regressed
    T4DonorPressureReturn = 3,// T4: donor needs the way back
    ActiveAtEnd = 4            // still borrowed at run end
};

inline const char *
betaResolutionLabel(BetaResolution r)
{
    switch (r) {
      case BetaResolution::Pending:               return "pending";
      case BetaResolution::CooldownExpiryReturn:  return "cooldown_expiry_return";
      case BetaResolution::T3RegressionReturn:    return "t3_regression_return";
      case BetaResolution::T4DonorPressureReturn: return "t4_donor_pressure_return";
      case BetaResolution::ActiveAtEnd:           return "active_at_end";
    }
    return "pending";
}

struct BetaFire
{
    int phaseId{-1};
    uint64_t phaseStartInst{0};
    uint64_t phaseEndInst{0};
    BetaDirection direction{BetaDirection::MbtbToTage};
    BetaMetric donor{BetaMetric::MbtbBank0Way3};
    BetaMetric donee{BetaMetric::TageT3};
    double zDonor{0.0};
    double zDonee{0.0};
    double thetaDonor{0.0};
    double thetaDonee{0.0};
    bool isNewDonation{true}; // false for a renewal of an active borrow
    BetaResolution resolution{BetaResolution::Pending};
    int returnPhaseId{-1};
    uint64_t returnPhaseStartInst{0};
    uint64_t returnPhaseEndInst{0};
};

struct BetaDecision
{
    // A phase produces at most one realloc action: either a new/renewal fire
    // (newFire) or a return of a prior borrow (returnOccurred), never both.
    bool newFire{false};
    BetaFire fire{};
    bool isNewDonation{false}; // true: way actually transfers; false: renewal
    bool returnOccurred{false};
    BetaMetric returnDonor{BetaMetric::MbtbBank0Way3}; // native side
    BetaMetric returnDonee{BetaMetric::TageT3};         // TAGE table returned
    // Per-metric z-score for the deciding phase (betaNA() if unavailable).
    std::array<double, NumBetaMetrics> zScores{};
    // Per-metric status for the deciding phase.
    std::array<BetaRefStatus, NumBetaMetrics> statuses{};
    // Cooldown snapshot before arbitration (T3, T7), after beginPhase decrement.
    std::array<unsigned, 2> cooldownBefore{0, 0};
    // Cooldown snapshot after arbitration (T3, T7).
    std::array<unsigned, 2> cooldownAfter{0, 0};
    bool evaluablePairs{false};
    uint32_t crossedPairCount{0};
    uint32_t availablePairCount{0};
    bool cooldownBlocked{false};
    bool arbitrationLost{false};
};

class BetaController
{
  public:
    BetaController() { reset(); }

    void configure(double thetaDonor, double thetaDonee)
    {
        thetaDonor_ = thetaDonor;
        thetaDonee_ = thetaDonee;
        reset();
    }

    double thetaDonor() const { return thetaDonor_; }
    double thetaDonee() const { return thetaDonee_; }

    void reset()
    {
        for (auto &buf : histories_) {
            buf.clear();
        }
        references_.fill(BetaReference{});
        referencesFrozen_ = false;
        for (auto &c : doneeCooldown_) {
            c = 0;
        }
        for (auto &b : activeBorrowForDonee_) {
            b = false;
        }
        fires_.clear();
        lastSegment_ = "warmup";
    }

    /**
     * Process one phase. segment is "warmup", "sample", or "transition";
     * replayEligible is (segment != "transition"). On warmup+eligible, the
     * finite per-metric values are appended to the circular history. On the
     * first sample+eligible phase, the reference distribution is computed and
     * frozen (never updated again). On sample+eligible phases, z-scores are
     * computed and at most one legal pair fires globally. transition phases
     * do not append history and do not decide.
     */
    BetaDecision processPhase(int phaseId, uint64_t phaseStartInst,
                               uint64_t phaseEndInst,
                               const std::string &segment,
                               bool replayEligible,
                               const BetaPhaseMetrics &metrics);

    /**
     * Resolve every still-pending borrow as ActiveAtEnd. Call once at run end
     * (or when the sample segment ends) before reading fires().
     */
    void finalizeAtEnd(int endPhaseId, uint64_t endInst);

    const std::vector<BetaFire>& fires() const { return fires_; }
    const BetaReference& reference(BetaMetric m) const
    {
        return references_[betaMetricIndex(m)];
    }

  private:
    struct HistoryBuffer
    {
        std::vector<double> data;
        size_t head{0};
        bool full{false};

        void clear()
        {
            data.clear();
            data.shrink_to_fit();
            data.reserve(BetaHistoryLimit);
            head = 0;
            full = false;
        }

        // Circular append (maxlen == BetaHistoryLimit). Matches
        // collections.deque(maxlen=128) semantics: once full, oldest drops.
        void append(double v)
        {
            if (data.size() < BetaHistoryLimit) {
                data.push_back(v);
            } else {
                data[head] = v;
                full = true;
            }
            head = (head + 1) % BetaHistoryLimit;
        }

        // The logical contents in insertion order (oldest first). For a full
        // buffer this is data[head..end] ++ data[0..head).
        std::vector<double> contents() const
        {
            std::vector<double> out;
            out.reserve(data.size());
            if (full) {
                for (size_t i = 0; i < data.size(); ++i) {
                    out.push_back(data[(head + i) % data.size()]);
                }
            } else {
                out = data;
            }
            return out;
        }

        size_t size() const { return data.size(); }
    };

    void appendWarmupHistory(const BetaPhaseMetrics &metrics);
    void freezeReferences();
    void computeZScores(const BetaPhaseMetrics &metrics,
                        std::array<double, NumBetaMetrics> &scores,
                        std::array<BetaRefStatus, NumBetaMetrics> &statuses) const;
    void beginPhase();
    unsigned cooldownForDonee(BetaMetric donee) const
    {
        return BetaDoneeCooldown[betaMetricIndex(donee)];
    }
    unsigned& cooldownSlot(BetaMetric donee)
    {
        // TageT3 -> slot 0, TageT7 -> slot 1
        return doneeCooldown_[donee == BetaMetric::TageT7 ? 1 : 0];
    }
    unsigned cooldownSlotRead(BetaMetric donee) const
    {
        return doneeCooldown_[donee == BetaMetric::TageT7 ? 1 : 0];
    }

    // True iff a borrowed way is currently held on the given donee (at most
    // one per donee). TageT3 -> slot 0, TageT7 -> slot 1.
    bool& activeBorrowSlot(BetaMetric donee)
    {
        return activeBorrowForDonee_[donee == BetaMetric::TageT7 ? 1 : 0];
    }
    bool activeBorrowSlotRead(BetaMetric donee) const
    {
        return activeBorrowForDonee_[donee == BetaMetric::TageT7 ? 1 : 0];
    }

    void resolveBorrows(int phaseId, uint64_t phaseStartInst,
                        uint64_t phaseEndInst,
                        const BetaPhaseMetrics &metrics,
                        const std::array<double, NumBetaMetrics> &scores,
                        const std::array<BetaRefStatus, NumBetaMetrics> &statuses,
                        bool &returnOccurred,
                        BetaMetric &returnDonor,
                        BetaMetric &returnDonee);

    double thetaDonor_{1.5};
    double thetaDonee_{1.5};
    std::array<HistoryBuffer, NumBetaMetrics> histories_{};
    std::array<BetaReference, NumBetaMetrics> references_{};
    bool referencesFrozen_{false};
    // Per-donee cooldown counters, indexed [0]=T3, [1]=T7.
    std::array<unsigned, 2> doneeCooldown_{0, 0};
    // True while a borrowed way is held on a donee (at most one per donee).
    // Indexed [0]=T3, [1]=T7.
    std::array<bool, 2> activeBorrowForDonee_{false, false};
    std::vector<BetaFire> fires_{};
    std::string lastSegment_{"warmup"};
};

} // namespace sway

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_BTB_SWAY_BETA_CONTROLLER_HH__
