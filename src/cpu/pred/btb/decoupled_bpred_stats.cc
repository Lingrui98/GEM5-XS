#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <tuple>

#include "base/output.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/btb/decoupled_bpred.hh"
#include "debug/BTB.hh"
#include "debug/Profiling.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

namespace
{

double
utility(uint64_t active, uint64_t total)
{
    return total == 0 ? 0.0 :
        static_cast<double>(active) / static_cast<double>(total);
}

const char *
ownerName(uint8_t owner)
{
    return owner < sway::NumScopes ? sway::scopeName(owner) : "invalid";
}

} // anonymous namespace

void
DecoupledBPUWithBTB::initDB()
{

    bpdb.init_db();
    enableBranchTrace = checkGivenSwitch(bpDBSwitches, std::string("basic"));
    if (enableBranchTrace) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("fsqId", UINT64),
            std::make_pair("startPC", UINT64),
            std::make_pair("controlPC", UINT64),
            std::make_pair("controlType", UINT64),
            std::make_pair("taken", UINT64),
            std::make_pair("mispred", UINT64),
            std::make_pair("fallThruPC", UINT64),
            std::make_pair("source", UINT64),
            std::make_pair("target", UINT64)
        };
        bptrace = bpdb.addAndGetTrace("BPTRACE", fields_vec);
        bptrace->init_table();
        removeGivenSwitch(bpDBSwitches, std::string("basic"));
        someDBenabled = true;
    }

    enablePredFSQTrace = checkGivenSwitch(bpDBSwitches, std::string("predfsq"));
    if (enablePredFSQTrace) {
        // Initialize prediction trace manager for recording predictions
        std::vector<std::pair<std::string, DataType>> pred_fields_vec = {
            std::make_pair("fsqId", UINT64),
            std::make_pair("startPC", UINT64),
            std::make_pair("predTaken", UINT64),
            std::make_pair("predEndPC", UINT64),
            std::make_pair("controlPC", UINT64),
            std::make_pair("target", UINT64),
            std::make_pair("predSource", UINT64),
            std::make_pair("btbHit", UINT64)
        };
        predTraceManager = bpdb.addAndGetTrace("PREDTRACE", pred_fields_vec);
        predTraceManager->init_table();
        removeGivenSwitch(bpDBSwitches, std::string("predfsq"));
        someDBenabled = true;
    }
}

DecoupledBPUWithBTB::SwayF1UtilitySnapshot
DecoupledBPUWithBTB::SwayController::computeF1Utility(
    const SwayF1Counters& counters)
{
    auto delta = [](uint64_t current, uint64_t previous) {
        return current >= previous ? current - previous : current;
    };
    auto ratio = [](uint64_t numerator, uint64_t denominator) {
        return denominator == 0 ? 0.0 :
            static_cast<double>(numerator) / static_cast<double>(denominator);
    };

    SwayF1UtilitySnapshot utility;
    const uint64_t mbtbMiss =
        delta(counters.mbtbPredMiss, previousF1Counters.mbtbPredMiss);
    const uint64_t mbtbHit =
        delta(counters.mbtbPredHit, previousF1Counters.mbtbPredHit);
    utility.mbtb = ratio(mbtbMiss, mbtbMiss + mbtbHit);

    const uint64_t mbtbCondMiss =
        delta(counters.mbtbCondMiss, previousF1Counters.mbtbCondMiss);
    const uint64_t mbtbCondHit =
        delta(counters.mbtbCondHit, previousF1Counters.mbtbCondHit);
    utility.mbtbCond = ratio(
        mbtbCondMiss, std::max<uint64_t>(1, mbtbCondMiss + mbtbCondHit));

    const uint64_t ittageMiss =
        delta(counters.ittagePredMiss, previousF1Counters.ittagePredMiss);
    const uint64_t ittageHit =
        delta(counters.ittagePredHit, previousF1Counters.ittagePredHit);
    utility.ittage = ratio(ittageMiss, ittageMiss + ittageHit);

    const uint64_t tageMispred =
        delta(counters.tageUpdateMispred,
              previousF1Counters.tageUpdateMispred);
    const uint64_t tageFiltered =
        delta(counters.tageUpdateFilteredEntries,
              previousF1Counters.tageUpdateFilteredEntries);
    const uint64_t tageDenominator = std::max<uint64_t>(1, tageFiltered);
    utility.tage = ratio(tageMispred, tageDenominator);

    for (unsigned table = 0; table < utility.tageTable.size(); ++table) {
        const uint64_t tableMispred = delta(
            counters.tageTableMispreds[table],
            previousF1Counters.tageTableMispreds[table]);
        utility.tageTable[table] = ratio(tableMispred, tageDenominator);
    }

    previousF1Counters = counters;
    return utility;
}

DecoupledBPUWithBTB::SwayController::Decision
DecoupledBPUWithBTB::SwayController::chooseReallocation(
    const std::vector<SwayUtilityRow>& phaseRows,
    const std::array<sway::MbtbTightSlotState,
                     sway::NumMbtbTightSlots>& mbtbSlots,
    const std::array<MBTB::TightSlotPhaseSnapshot,
                     sway::NumMbtbTightSlots>& mbtbSlotRows,
    const SwayF1Counters& f1Counters)
{
    Decision decision;
    if (!enabled) {
        return decision;
    }

    const auto f1Utility = computeF1Utility(f1Counters);
    decision.f1Utility = f1Utility;
    if (phaseRows.size() < 2) {
        return decision;
    }

    struct ComponentAggregate
    {
        uint64_t totalWays{0};
        uint64_t activeWays{0};

        void add(const SwayUtilityRow &row)
        {
            totalWays += row.totalWays;
            activeWays += row.activeWays;
        }

        double utility() const
        {
            return totalWays == 0 ? 0.0 :
                static_cast<double>(activeWays) /
                static_cast<double>(totalWays);
        }
    };

    ComponentAggregate mbtbUtility;
    ComponentAggregate tageUtility;
    std::array<const SwayUtilityRow *, sway::NumScopes> rowsByOwner{};
    const SwayUtilityRow *availableMbtbDonorRow = nullptr;
    const MBTB::TightSlotPhaseSnapshot *availableMbtbDonorSlot = nullptr;
    std::array<const SwayUtilityRow *, sway::NumTageTables> tageRows{};
    for (const auto &row : phaseRows) {
        const uint8_t owner = sway::ownerFromScope(row.scope);
        if (owner == sway::InvalidOwner || row.totalWays == 0) {
            continue;
        }
        rowsByOwner[owner] = &row;
        if (sway::isMbtbOwner(owner)) {
            mbtbUtility.add(row);
        } else if (sway::isTageOwner(owner)) {
            tageUtility.add(row);
            const unsigned table = sway::tageTable(owner);
            if (table < tageRows.size()) {
                tageRows[table] = &row;
            }
        }
    }

    if (mbtbUtility.totalWays == 0 || tageUtility.totalWays == 0 ||
        doneeTables.empty()) {
        return decision;
    }

    const double donorComponentUtility = f1Utility.mbtbCond;
    const double doneeComponentUtility = f1Utility.tage;
    auto slotUtility = [](const MBTB::TightSlotPhaseSnapshot &slot) {
        return slot.totalWays == 0 ? 0.0 :
            static_cast<double>(slot.activeWays) /
            static_cast<double>(slot.totalWays);
    };

    std::array<bool, sway::NumTageTables> activeMbtbDonee{};
    for (const auto &slot : mbtbSlots) {
        if (slot.slotId == sway::InvalidIndex ||
            slot.slotId >= mbtbSlotRows.size() ||
            slot.sourceSram == sway::InvalidIndex ||
            !slot.donatedToTage()) {
            continue;
        }

        const unsigned table = slot.doneeTable();
        if (table >= activeMbtbDonee.size() || !doneeTables.contains(table)) {
            continue;
        }
        activeMbtbDonee[table] = true;

        if (!doneeCooldownJustExpired(table)) {
            continue;
        }

        const uint8_t nativeOwner = sway::mbtbOwner(slot.sourceSram);
        const SwayUtilityRow *mbtbNative = rowsByOwner[nativeOwner];
        const SwayUtilityRow *tageDonee = tageRows[table];
        if (!mbtbNative || !tageDonee) {
            continue;
        }
        decision.donor = nativeOwner;
        decision.donee = sway::tageOwner(table);
        decision.donorUtility = donorComponentUtility;
        decision.doneeUtility = doneeComponentUtility;
        decision.donorComponentUtility = donorComponentUtility;
        decision.doneeComponentUtility = doneeComponentUtility;
        decision.f1Utility = f1Utility;
        if ((doneeComponentUtility - donorComponentUtility) >
            tightHysteresisMargin) {
            decision.renewCooldown = true;
            return decision;
        }

        decision.valid = true;
        decision.action = sway::ControllerAction::ReturnMbtbTight;
        decision.donor = sway::tageOwner(table);
        decision.donee = nativeOwner;
        return decision;
    }

    for (const auto &slot : mbtbSlots) {
        if (slot.slotId == sway::InvalidIndex ||
            slot.slotId >= mbtbSlotRows.size() ||
            slot.sourceSram == sway::InvalidIndex) {
            continue;
        }
        const uint8_t nativeOwner = sway::mbtbOwner(slot.sourceSram);
        if (slot.owner != nativeOwner) {
            continue;
        }
        const SwayUtilityRow *row = rowsByOwner[nativeOwner];
        if (!row) {
            continue;
        }
        const auto &slotRow = mbtbSlotRows[slot.slotId];
        if (!availableMbtbDonorSlot ||
            slotUtility(slotRow) < slotUtility(*availableMbtbDonorSlot)) {
            availableMbtbDonorRow = row;
            availableMbtbDonorSlot = &slotRow;
        }
    }
    if (!availableMbtbDonorRow || !availableMbtbDonorSlot) {
        return decision;
    }

    const SwayUtilityRow *tageDonee = nullptr;
    double bestDoneeTableUtility = -std::numeric_limits<double>::infinity();
    uint8_t cooldownBlockedTable = sway::InvalidOwner;
    for (auto table : doneeTables.tableIds) {
        if (table >= tageRows.size() || !tageRows[table]) {
            continue;
        }
        if (activeMbtbDonee[table]) {
            continue;
        }
        if (doneeCoolingDown(table)) {
            if (cooldownBlockedTable == sway::InvalidOwner) {
                cooldownBlockedTable = table;
            }
            continue;
        }
        const double tableUtility = f1Utility.tageTable[table];
        if (!tageDonee || tableUtility > bestDoneeTableUtility) {
            tageDonee = tageRows[table];
            bestDoneeTableUtility = tableUtility;
        }
    }

    if (!tageDonee) {
        if (cooldownBlockedTable != sway::InvalidOwner) {
            decision.blockedByCooldown = true;
            decision.blockedDonee = cooldownBlockedTable;
        }
        return decision;
    }

    if ((doneeComponentUtility - donorComponentUtility) <=
        tightHysteresisMargin) {
        return decision;
    }

    decision.valid = true;
    decision.action = sway::ControllerAction::MbtbToTageTight;
    decision.donor = sway::ownerFromScope(availableMbtbDonorRow->scope);
    decision.donee = sway::ownerFromScope(tageDonee->scope);
    decision.donorUtility = donorComponentUtility;
    decision.doneeUtility = doneeComponentUtility;
    decision.donorComponentUtility = donorComponentUtility;
    decision.doneeComponentUtility = doneeComponentUtility;
    decision.f1Utility = f1Utility;
    return decision;
}

DecoupledBPUWithBTB::SwayF1Counters
DecoupledBPUWithBTB::collectSwayF1Counters() const
{
    SwayF1Counters counters;
    if (mbtb) {
        counters.mbtbPredMiss = mbtb->swayPredMissCount();
        counters.mbtbPredHit = mbtb->swayPredHitCount();
        counters.mbtbCondMiss = mbtb->swayCondMissCount();
        counters.mbtbCondHit = mbtb->swayCondHitCount();
    }
    if (ittage) {
        counters.ittagePredMiss = ittage->swayCommitMissCount();
        counters.ittagePredHit = ittage->swayCommitHitCount();
    }
    if (tage) {
        counters.tageUpdateMispred = tage->swayUpdateMispredCount();
        counters.tageUpdateFilteredEntries =
            tage->swayUpdateFilteredEntriesCount();
        const unsigned numTables = std::min<unsigned>(
            sway::NumTageTables, tage->swayNumPredictorTables());
        for (unsigned table = 0; table < numTables; ++table) {
            counters.tageTableMispreds[table] =
                tage->swayUpdateTableMispredCount(table);
        }
    }
    return counters;
}

unsigned
DecoupledBPUWithBTB::countSwayOwnedWays(uint8_t owner) const
{
    unsigned count = 0;
    if (mbtb) {
        count += mbtb->countSwayOwnedWays(owner);
    }
    if (tage) {
        count += tage->countSwayOwnedWays(owner);
    }
    return count;
}

unsigned
DecoupledBPUWithBTB::transferSwayWays(uint8_t donor, uint8_t donee,
                                      unsigned ways)
{
    unsigned moved = 0;
    if (mbtb) {
        moved += mbtb->transferSwayWays(donor, donee, ways - moved);
    }
    if (tage && moved < ways) {
        moved += tage->transferSwayWays(donor, donee, ways - moved);
    }
    return moved;
}

void
DecoupledBPUWithBTB::syncSwayBorrowedWayCounts()
{
    sway::ScopeCounts borrowed{};
    borrowed.fill(0);
    if (mbtb) {
        mbtb->addSwayBorrowedWayCounts(borrowed);
    }
    if (tage) {
        tage->addSwayBorrowedWayCounts(borrowed);
    }
    if (mbtb) {
        mbtb->syncSwayBorrowedWayCounts(borrowed);
    }
    if (tage) {
        tage->syncSwayBorrowedWayCounts(borrowed);
        if (mbtb) {
            tage->configureSwayMbtbBorrowedSlots(
                mbtb->getSwayMbtbTightSlotStates(), swayDoneeTables,
                mbtb->swayNumSets());
        }
    }
}

void
DecoupledBPUWithBTB::trySwayReallocForPhase(
    const std::vector<SwayUtilityRow>& phaseRows,
    const std::vector<SwayUtilityRow>& ittagePhaseRows,
    const std::array<MBTB::TightSlotPhaseSnapshot,
                     sway::NumMbtbTightSlots>& mbtbSlotRows)
{
    swayController.beginPhase();
    std::array<sway::MbtbTightSlotState, sway::NumMbtbTightSlots>
        mbtbSlots{};
    if (mbtb) {
        mbtbSlots = mbtb->getSwayMbtbTightSlotStates();
    }
    const auto f1Counters = collectSwayF1Counters();
    auto decision = swayController.chooseReallocation(phaseRows, mbtbSlots,
                                                      mbtbSlotRows,
                                                      f1Counters);
    if (!decision.valid) {
        if (decision.renewCooldown) {
            swayController.armCooldown(decision.donee,
                sway::ControllerAction::MbtbToTageTight);
        }
        if (decision.blockedByCooldown &&
            decision.blockedDonee < sway::NumTageTables) {
            swayStats.coolDownBlockedByDonee[decision.blockedDonee]++;
        }
        recordSwayPhaseDiag(phaseRows.empty() ? -1 : phaseRows.front().phaseID,
                            phaseRows, ittagePhaseRows, mbtbSlotRows,
                            decision, "not_valid", 0);
        return;
    }

    if (swayController.isQuiescing()) {
        recordSwayPhaseDiag(phaseRows.empty() ? -1 : phaseRows.front().phaseID,
                            phaseRows, ittagePhaseRows, mbtbSlotRows,
                            decision, "quiesce", 0);
        return;
    }

    constexpr unsigned minOwnedWays = 1;
    const unsigned donorOwned = countSwayOwnedWays(decision.donor);
    if (donorOwned <= minOwnedWays) {
        recordSwayPhaseDiag(phaseRows.empty() ? -1 : phaseRows.front().phaseID,
                            phaseRows, ittagePhaseRows, mbtbSlotRows,
                            decision, "min_owned", 0);
        return;
    }
    const unsigned requestWays = std::min(swayController.waysPerTransfer(),
                                          donorOwned - minOwnedWays);
    const unsigned moved = transferSwayWays(decision.donor, decision.donee,
                                            requestWays);
    if (moved == 0) {
        recordSwayPhaseDiag(phaseRows.empty() ? -1 : phaseRows.front().phaseID,
                            phaseRows, ittagePhaseRows, mbtbSlotRows,
                            decision, "moved_zero", 0);
        return;
    }

    syncSwayBorrowedWayCounts();
    swayStats.reallocCount++;
    swayStats.ownerOverrides[decision.donee] += moved;
    if (decision.action == sway::ControllerAction::MbtbToTageTight &&
        sway::isMbtbOwner(decision.donor)) {
        swayStats.slotOwnerTransitionsPerSlot[sway::mbtbSram(decision.donor)]++;
    } else if (decision.action == sway::ControllerAction::ReturnMbtbTight &&
               sway::isMbtbOwner(decision.donee)) {
        swayStats.slotOwnerTransitionsPerSlot[sway::mbtbSram(decision.donee)]++;
    }
    swayController.armCooldown(decision);
    swayController.startQuiesce();
    recordSwayPhaseDiag(phaseRows.empty() ? -1 : phaseRows.front().phaseID,
                        phaseRows, ittagePhaseRows, mbtbSlotRows,
                        decision, "executed", moved);
}

void
DecoupledBPUWithBTB::recordSwayPhaseDiag(
    int phaseID,
    const std::vector<SwayUtilityRow>& phaseRows,
    const std::vector<SwayUtilityRow>& ittagePhaseRows,
    const std::array<MBTB::TightSlotPhaseSnapshot,
                     sway::NumMbtbTightSlots>& mbtbSlotRows,
    const SwayController::Decision& decision,
    const std::string& execution,
    unsigned movedWays)
{
    SwayPhaseDiagRow row{};
    row.phaseID = phaseID;
    row.tightHysteresisMargin = swayReallocHysteresis > 0.0 ?
        swayReallocHysteresis : sway::TightHysteresisMargin;
    auto describeDecision = [&]() {
        if (decision.valid) {
            std::stringstream ss;
            switch (decision.action) {
              case sway::ControllerAction::MbtbToTageTight:
                ss << "transfer_" << ownerName(decision.donor)
                   << "_to_" << ownerName(decision.donee);
                return ss.str();
              case sway::ControllerAction::ReturnMbtbTight:
                ss << "return_" << ownerName(decision.donor)
                   << "_to_" << ownerName(decision.donee);
                return ss.str();
              case sway::ControllerAction::IttageToTageRelaxed:
                ss << "relaxed_transfer_" << ownerName(decision.donor)
                   << "_to_" << ownerName(decision.donee);
                return ss.str();
              case sway::ControllerAction::ReturnIttageRelaxed:
                ss << "relaxed_return_" << ownerName(decision.donor)
                   << "_to_" << ownerName(decision.donee);
                return ss.str();
              case sway::ControllerAction::None:
                break;
            }
        }
        if (decision.renewCooldown) {
            return std::string("cooldown_renew_") + ownerName(decision.donee);
        }
        if (decision.blockedByCooldown) {
            return std::string("cooldown_blocked_t") +
                std::to_string(decision.blockedDonee);
        }
        return std::string("hold");
    };
    row.decision = describeDecision();
    row.execution = execution;
    row.donor = decision.donor;
    row.donee = decision.donee;
    row.donorUtility = decision.donorUtility;
    row.doneeUtility = decision.doneeUtility;
    row.donorComponentUtility = decision.donorComponentUtility;
    row.doneeComponentUtility = decision.doneeComponentUtility;
    row.f1Utility = decision.f1Utility;
    row.movedWays = movedWays;
    row.reallocCount = swayStats.reallocCount.value();
    row.tageTableUtility.fill(0.0);
    row.mbtbSlotTotalWays.fill(0);
    row.mbtbSlotActiveWays.fill(0);
    row.mbtbSlotUtility.fill(0.0);
    row.mbtbSlotOwner.fill(sway::InvalidOwner);

    for (const auto &utilityRow : phaseRows) {
        const uint8_t owner = sway::ownerFromScope(utilityRow.scope);
        if (sway::isMbtbOwner(owner)) {
            row.mbtbTotalWays += utilityRow.totalWays;
            row.mbtbActiveWays += utilityRow.activeWays;
        } else if (sway::isTageOwner(owner)) {
            row.tageTotalWays += utilityRow.totalWays;
            row.tageActiveWays += utilityRow.activeWays;
            const unsigned table = sway::tageTable(owner);
            if (table < row.tageTableUtility.size()) {
                row.tageTableUtility[table] = utilityRow.utility;
            }
        }
    }
    row.mbtbUtility = utility(row.mbtbActiveWays, row.mbtbTotalWays);
    row.tageUtility = utility(row.tageActiveWays, row.tageTotalWays);

    for (const auto &utilityRow : ittagePhaseRows) {
        row.ittageTotalWays += utilityRow.totalWays;
        row.ittageActiveWays += utilityRow.activeWays;
    }
    row.ittageUtility = utility(row.ittageActiveWays, row.ittageTotalWays);

    for (const auto &slot : mbtbSlotRows) {
        if (slot.slotId >= sway::NumMbtbTightSlots) {
            continue;
        }
        row.mbtbSlotOwner[slot.slotId] = slot.owner;
        row.mbtbSlotTotalWays[slot.slotId] = slot.totalWays;
        row.mbtbSlotActiveWays[slot.slotId] = slot.activeWays;
        row.mbtbSlotUtility[slot.slotId] =
            utility(slot.activeWays, slot.totalWays);
    }

    swayPhaseDiagRows.push_back(row);
}

void
DecoupledBPUWithBTB::collectSwayWayVisitForPhase(int phaseID)
{
    std::vector<SwayUtilityRow> phaseRows;
    std::vector<SwayUtilityRow> ittagePhaseRows;
    std::array<MBTB::TightSlotPhaseSnapshot, sway::NumMbtbTightSlots>
        mbtbSlotRows{};
    auto append = [&](const std::string& scope, uint64_t totalWays,
                      uint64_t validWays, uint64_t activeWays) {
        swayController.collectPhaseScope(phaseID, scope, totalWays,
                                         activeWays);
        phaseRows.push_back({phaseID, scope, totalWays, activeWays,
                             totalWays == 0 ? 0.0 :
                             static_cast<double>(activeWays) /
                             static_cast<double>(totalWays)});
        swayStrandedByPhase.push_back({phaseID, scope, totalWays,
                                       validWays, activeWays});
    };

    if (mbtb) {
        mbtbSlotRows = mbtb->collectSwayMbtbTightSlotVisitCounts();
        for (const auto& snap : mbtb->collectAndResetWayVisitCounts()) {
            append(snap.scope, snap.totalWays, snap.validWays,
                   snap.activeWays);
        }
    }
    if (ittage) {
        for (const auto& snap : ittage->collectAndResetTableVisitCounts()) {
            ittagePhaseRows.push_back(
                {phaseID, snap.scope, snap.totalWays, snap.activeWays,
                 snap.totalWays == 0 ? 0.0 :
                 static_cast<double>(snap.activeWays) /
                 static_cast<double>(snap.totalWays)});
        }
    }
    if (tage) {
        for (const auto& snap : tage->collectAndResetWayVisitCounts()) {
            append(snap.scope, snap.totalWays, snap.validWays,
                   snap.activeWays);
        }
    }
    // NOTE: MicroTAGE (and other fast predictors) are intentionally out of
    // SWAY's reallocation scope — see memory:sway-scope-exclusions. After the
    // 2026-06 xs-dev merge, MicroTAGE no longer inherits from BTBTAGE and
    // therefore does not expose collectAndResetWayVisitCounts(); we skip it
    // here to keep the probe focused on MBTB + BTBTAGE.
    if (enableSwayRealloc) {
        trySwayReallocForPhase(phaseRows, ittagePhaseRows, mbtbSlotRows);
    }
}

void
DecoupledBPUWithBTB::dumpStats()
{
    // Helper function: create output file and write header
    auto createOutputFile = [](const std::string& filename, const std::string& header) {
        auto handle = simout.create(filename, false, true);
        *handle->stream() << header << std::endl;
        return handle;
    };

    // Generic sort function by key
    auto sortByKey = [](auto& data, auto keyFn) {
        std::sort(data.begin(), data.end(),
            [&keyFn](const auto& a, const auto& b) {
                return keyFn(a) > keyFn(b);
            });
    };

    // 1. Output top mispredictions
    auto outFile = createOutputFile("topMisPredicts.csv", "startPC,control_pc,count");
    // topMisPredPC: Vector of mispredict records (startPC, controlPC) -> count
    // startPC: Starting address of fetch block
    // controlPC: Address of branch instruction
    // count: Number of mispredictions
    std::vector<std::pair<std::pair<Addr, Addr>, int>> topMisPredPC(
        topMispredicts.begin(), topMispredicts.end());

    sortByKey(topMisPredPC, [](const auto& entry) { return entry.second; });
    for (const auto& entry : topMisPredPC) {
        *outFile->stream() << std::hex << entry.first.first << ","
            << entry.first.second << ","
            << std::dec << entry.second << std::endl;
    }
    simout.close(outFile);

    // 2. Output mispredictions by branch
    outFile = createOutputFile("topMispredictsByBranch.csv",
                "pc,type,mispredicts,total,misPermil,dirMiss,tgtMiss,noPredMiss");

    // topMisPredPCByBranch: Detailed misprediction records per branch
    std::vector<std::tuple<Addr, int, int, int, double, int, int, int>> topMisPredPCByBranch;
    for (const auto &it : topMispredictsByBranch) {
        const auto &stats = it.second;
        topMisPredPCByBranch.push_back(std::make_tuple(
            stats.pc, stats.branchType,
            stats.mispredCount, stats.totalCount,
            stats.getMispredRate(),
            stats.dirWrongCount, stats.targetWrongCount, stats.noPredCount));
    }

    sortByKey(topMisPredPCByBranch,
        [](const auto& entry) {
            return std::get<2>(entry);  // Sort by mispredCount
        });

    for (const auto& entry : topMisPredPCByBranch) {
        *outFile->stream()
            << std::hex << std::get<0>(entry) << ","
            << std::dec << std::get<1>(entry) << ","
            << std::get<2>(entry) << ","
            << std::get<3>(entry) << ","
            << std::get<4>(entry) << ","
            << std::get<5>(entry) << ","
            << std::get<6>(entry) << ","
            << std::get<7>(entry) << std::endl;
    }
    simout.close(outFile);

    // 3. Sort branches by misrate (per-mille)
    outFile = createOutputFile("topMisrateByBranch.csv",
                "pc,type,mispredicts,total,misPermil,dirMiss,tgtMiss,noPredMiss");

    // Reuse previous data, but sort by misrate
    int mispCntThres = 100;
    sortByKey(topMisPredPCByBranch, [](const auto& entry) { return std::get<4>(entry); });
    for (const auto& entry : topMisPredPCByBranch) {
        if (std::get<3>(entry) < mispCntThres) continue;

        *outFile->stream() << std::hex << std::get<0>(entry) << std::dec
            << "," << std::get<1>(entry)
            << "," << std::get<2>(entry)
            << "," << std::get<3>(entry)
            << "," << (int)std::get<4>(entry)
            << "," << (int)std::get<5>(entry)
            << "," << (int)std::get<6>(entry)
            << "," << (int)std::get<7>(entry) << std::endl;
    }
    simout.close(outFile);

    // Create CSV header for topN tables
    auto createTopNHeader = [](std::ostream& out, int outputTopN, const std::string& prefix) {
        out << prefix;
        for (int i = 0; i < outputTopN; i++) {
            out << ",topMispPC_" << i
                << ",type_" << i
                << ",misCnt_" << i;
        }
        out << std::endl;
    };

    // Output phase-classified mispredictions
    int outputTopN = 5;

    // 4. Phase-based mispredictions
    auto processPhaseData = [&](const std::string& filename,
                           const auto& dataByPhase,
                           const auto& takenBranches) {
        outFile = simout.create(filename, false, true);
        auto& out = *outFile->stream();

        // Write header
        createTopNHeader(out, outputTopN,
                        filename.find("Sub") != std::string::npos ?
                        "subPhaseID,numBranches,numEverTakenBranches,totalMispredicts" :
                        "phaseID,numBranches,numEverTakenBranches,totalMispredicts");

        int phaseID = 0;
        for (const auto& phaseData : dataByPhase) {
            int numStaticBranches = phaseData.size();
            int numEverTakenStaticBranches = takenBranches[phaseID].size();

            // Copy data and calculate total mispredictions
            std::vector<std::pair<BranchKey, BranchStats>> phaseRecords;
            for (const auto& record : phaseData) {
                phaseRecords.push_back(record);
            }

            // Calculate total mispredicts
            int totalMispredicts = 0;
            for (const auto& rec : phaseRecords) {
                totalMispredicts += rec.second.mispredCount;
            }

            // Output phase basic info
            out << phaseID << "," << numStaticBranches << ","
                << numEverTakenStaticBranches << "," << totalMispredicts;

            // Sort by misprediction count
            std::sort(phaseRecords.begin(), phaseRecords.end(),
                [](const auto& a, const auto& b) {
                    return a.second.mispredCount > b.second.mispredCount;
                });

            // Output top-N
            for (int i = 0; i < outputTopN && i < phaseRecords.size(); i++) {
                const auto& stats = phaseRecords[i].second;
                out << "," << std::hex << stats.pc // pc
                    << "," << std::dec << stats.branchType // type
                    << "," << stats.mispredCount; // count
            }
            out << std::dec << std::endl;
            phaseID++;
        }
        simout.close(outFile);
    };

    processPhaseData("topMispredictByPhase.csv",
                     topMispredictsByBranchByPhase,
                     takenBranchesByPhase);

    processPhaseData("topMispredictBySubPhase.csv",
                     topMispredictsByBranchBySubPhase,
                     takenBranchesBySubPhase);

    // 5. Output history misprediction data
    outFile = createOutputFile("topMisPredictHist.csv", "Hist,count");
    // Vector of (history pattern, count) pairs
    std::vector<std::pair<uint64_t, uint64_t>>
        topMisPredHistVec(topMispredHist.begin(), topMispredHist.end());

    sortByKey(topMisPredHistVec, [](const auto& entry) { return entry.second; });
    for (const auto& entry : topMisPredHistVec) {
        *outFile->stream() << std::hex << entry.first << ","
            << std::dec << entry.second << std::endl;
    }
    simout.close(outFile);

    // 6. Output indirect mispredictions
    outFile = createOutputFile("misPredIndirectStream.csv", "count,address");
    // Vector of (address, count) pairs for indirect branches
    std::vector<std::pair<Addr, unsigned>>
        indirectVec(topMispredIndirect.begin(), topMispredIndirect.end());

    sortByKey(indirectVec, [](const auto& entry) { return entry.second; });
    for (const auto& entry : indirectVec) {
        *outFile->stream() << std::oct << entry.second << ","
            << std::hex << entry.first << std::endl;
    }
    simout.close(outFile);

    // Process FSQ distribution data
    auto processFsqDistribution = [&](const std::string& filename,
                                  const auto& distByPhase) {
        outFile = simout.create(filename, false, true);
        auto& out = *outFile->stream();

        // Write header
        out << "phaseID";
        for (int i = 0; i <= maxInstsNum; i++) {
            out << "," << i;
        }
        out << ",average" << std::endl;

        // Write data for each phase
        int phaseID = 0;
        for (const auto& dist : distByPhase) {
            out << phaseID;

            int numFsqEntries = 0;
            for (int i = 0; i <= maxInstsNum; i++) {
                numFsqEntries += dist[i];
            }

            for (int i = 0; i <= maxInstsNum; i++) {
                out << "," << dist[i];
            }

            out << "," << (double)phaseSizeByInst / (double)numFsqEntries << std::endl;
            phaseID++;
        }

        simout.close(outFile);
    };

    // 7-8. Output FSQ distribution data
    processFsqDistribution("fsqEntryCommittedInstNumDistsByPhase.csv",
                          fsqEntryNumCommittedInstDistByPhase);

    processFsqDistribution("fsqEntryFetchedInstNumDistsByPhase.csv",
                          fsqEntryNumFetchedInstDistByPhase);

    // 9. Output BTB entries
    int outputTopNEntries = 1;
    std::stringstream headerSS;
    headerSS << "phaseID,numBTBEntries";
    for (int i = 0; i <= outputTopNEntries; i++) {
        headerSS << ",entry_" << i << "_pc,entry_" << i << "_type";
    }
    outFile = createOutputFile("btbEntriesByPhase.csv", headerSS.str());

    int phaseID = 0;
    for (auto& phase : BTBEntriesByPhase) {
        auto& out = *outFile->stream();
        out << std::dec << phaseID << "," << phase.size();

        // Vector of (PC, BTBEntry, count) tuples
        std::vector<std::tuple<Addr, BTBEntry, int>> btbEntries;
        for (auto& entry : phase) {
            btbEntries.push_back(std::make_tuple(
                entry.first, entry.second.first, entry.second.second));
        }

        std::sort(btbEntries.begin(), btbEntries.end(),
            [](const auto &a, const auto &b) {
                 return std::get<2>(a) > std::get<2>(b);
            });

        for (int i = 0; i <= outputTopNEntries && i < btbEntries.size(); i++) {
            const auto &entry = btbEntries[i];
            out << "," << std::hex << std::get<0>(entry);
            // BTBEntry.getType() is not a const method, need to create a copy
            BTBEntry btbEntry = std::get<1>(entry);
            out << "," << std::dec << btbEntry.getType();
        }

        out << std::endl;
        phaseID++;
    }
    simout.close(outFile);

    // 10. SWAY per-way stranded-capacity rows.
    {
        auto handle = createOutputFile(
            "sway_stranded_by_phase.csv",
            "phaseID,scope,total_ways,valid_ways,active_ways");
        auto& out = *handle->stream();
        for (const auto& row : swayStrandedByPhase) {
            out << row.phaseID << ',' << row.scope << ','
                << row.totalWays << ',' << row.validWays << ','
                << row.activeWays << '\n';
        }
        simout.close(handle);
    }

    // 11. SWAY measurement-only utility vector rows.
    {
        auto handle = createOutputFile(
            "sway_utility_by_phase.csv",
            "phaseID,scope,total_ways,active_ways,utility");
        auto& out = *handle->stream();
        for (const auto& row : swayController.rows()) {
            out << row.phaseID << ',' << row.scope << ','
                << row.totalWays << ',' << row.activeWays << ','
                << row.utility << '\n';
        }
        simout.close(handle);
    }

    // 12. SWAY Phase-A controller diagnosis rows.
    {
        std::stringstream header;
        header << "phaseID"
               << ",mbtb_total_ways,mbtb_active_ways,mbtb_utility"
               << ",ittage_total_ways,ittage_active_ways,ittage_utility"
               << ",tage_total_ways,tage_active_ways,tage_utility"
               << ",old_utility_mbtb,old_utility_ittage,old_utility_tage"
               << ",new_utility_mbtb,new_utility_mbtb_cond"
               << ",new_utility_ittage,new_utility_tage";
        for (unsigned table = 0; table < sway::NumTageTables; ++table) {
            header << ",new_utility_tage_t" << table;
        }
        for (unsigned table = 0; table < sway::NumTageTables; ++table) {
            header << ",tage_t" << table << "_utility";
        }
        for (unsigned slot = 0; slot < sway::NumMbtbTightSlots; ++slot) {
            header << ",mbtb_slot" << slot << "_owner"
                   << ",mbtb_slot" << slot << "_total_ways"
                   << ",mbtb_slot" << slot << "_active_ways"
                   << ",mbtb_slot" << slot << "_utility";
        }
        header << ",tight_hysteresis_margin,decision,execution"
               << ",donor,donee,donor_utility,donee_utility"
               << ",donor_component_utility,donee_component_utility"
               << ",moved_ways,reallocCount";

        auto handle = createOutputFile("sway_phase_a_diag.csv", header.str());
        auto& out = *handle->stream();
        for (const auto& row : swayPhaseDiagRows) {
            out << row.phaseID
                << ',' << row.mbtbTotalWays
                << ',' << row.mbtbActiveWays
                << ',' << row.mbtbUtility
                << ',' << row.ittageTotalWays
                << ',' << row.ittageActiveWays
                << ',' << row.ittageUtility
                << ',' << row.tageTotalWays
                << ',' << row.tageActiveWays
                << ',' << row.tageUtility
                << ',' << row.mbtbUtility
                << ',' << row.ittageUtility
                << ',' << row.tageUtility
                << ',' << row.f1Utility.mbtb
                << ',' << row.f1Utility.mbtbCond
                << ',' << row.f1Utility.ittage
                << ',' << row.f1Utility.tage;
            for (auto tableUtility : row.f1Utility.tageTable) {
                out << ',' << tableUtility;
            }
            for (auto tableUtility : row.tageTableUtility) {
                out << ',' << tableUtility;
            }
            for (unsigned slot = 0; slot < sway::NumMbtbTightSlots; ++slot) {
                out << ',' << ownerName(row.mbtbSlotOwner[slot])
                    << ',' << row.mbtbSlotTotalWays[slot]
                    << ',' << row.mbtbSlotActiveWays[slot]
                    << ',' << row.mbtbSlotUtility[slot];
            }
            out << ',' << row.tightHysteresisMargin
                << ',' << row.decision
                << ',' << row.execution
                << ',' << ownerName(row.donor)
                << ',' << ownerName(row.donee)
                << ',' << row.donorUtility
                << ',' << row.doneeUtility
                << ',' << row.donorComponentUtility
                << ',' << row.doneeComponentUtility
                << ',' << row.movedWays
                << ',' << row.reallocCount
                << '\n';
        }
        simout.close(handle);
    }

    // Save the database
    if (someDBenabled) {
        bpdb.save_db(simout.resolve("bp.db").c_str());
    }
}

DecoupledBPUWithBTB::BpTrace::BpTrace(uint64_t fsqId, FetchTarget &target, const DynInstPtr &inst, bool mispred)
{
    _tick = curTick();
    Addr pc = inst->pcState().instAddr();
    const auto &rv_pc = inst->pcState().as<RiscvISA::PCState>();
    Addr targetpc = rv_pc.npc();
    Addr fallThru = rv_pc.getFallThruPC();
    BranchInfo info(pc, targetpc, inst->staticInst, fallThru-pc);
    set(fsqId, target.startPC, pc, info.getType(), inst->branching(), mispred, fallThru, target.predSource, targetpc);
    // for (auto it = _uint64_data.begin(); it != _uint64_data.end(); it++) {
    //     printf("%s: %ld\n", it->first.c_str(), it->second);
    // }
}


namespace {

constexpr std::array<const char*, DecoupledBPUWithBTB::NumBranchClasses>
    BranchClassLabels = {
        "cond_branch",
        "direct_call",
        "indirect_call",
        "return",
        "direct_jump",
        "indirect_jump",
        "unknown"
    };

template <typename InstPtr>
DecoupledBPUWithBTB::BranchClass
classifyBranchImpl(const InstPtr &inst)
{
    using BranchClass = DecoupledBPUWithBTB::BranchClass;

    if (!inst || !inst->isControl()) {
        return BranchClass::Unknown;
    }

    if (inst->isReturn()) {
        return BranchClass::Return;
    }

    if (inst->isCall()) {
        return inst->isIndirectCtrl() ? BranchClass::IndirectCall
                                      : BranchClass::DirectCall;
    }

    if (inst->isCondCtrl()) {
        return BranchClass::CondBranch;
    }

    if (inst->isIndirectCtrl()) {
        return BranchClass::IndirectJump;
    }

    if (inst->isDirectCtrl() || inst->isUncondCtrl()) {
        return BranchClass::DirectJump;
    }

    return BranchClass::Unknown;
}

} // anonymous namespace

DecoupledBPUWithBTB::DBPBTBStats::DBPBTBStats(
    statistics::Group* parent, unsigned numStages, unsigned fsqSize, unsigned maxInstsNum):
    statistics::Group(parent),
    ADD_STAT(condNum, statistics::units::Count::get(), "the number of cond branches"),
    ADD_STAT(uncondNum, statistics::units::Count::get(), "the number of uncond branches"),
    ADD_STAT(returnNum, statistics::units::Count::get(), "the number of return branches"),
    ADD_STAT(otherNum, statistics::units::Count::get(), "the number of other branches"),
    ADD_STAT(condMiss, statistics::units::Count::get(), "the number of cond branch misses"),
    ADD_STAT(uncondMiss, statistics::units::Count::get(), "the number of uncond branch misses"),
    ADD_STAT(returnMiss, statistics::units::Count::get(), "the number of return branch misses"),
    ADD_STAT(otherMiss, statistics::units::Count::get(), "the number of other branch misses"),
    ADD_STAT(branchClassCounts, statistics::units::Count::get(), "branch counts by fine-grained class"),
    ADD_STAT(branchClassMisses, statistics::units::Count::get(), "branch mispredictions by fine-grained class"),
    ADD_STAT(branchClassCountsTotal, statistics::units::Count::get(), "total number of classified branches"),
    ADD_STAT(controlSquashByClass, statistics::units::Count::get(), "commit/resolve-path squashes by branch class"),
    ADD_STAT(staticBranchNum, statistics::units::Count::get(), "the number of all (different) static branches"),
    ADD_STAT(staticBranchNumEverTaken, statistics::units::Count::get(), "the number of all (different) static branches that are once taken"),
    ADD_STAT(predsOfEachStage, statistics::units::Count::get(), "the number of preds of each stage that account for final pred"),
    ADD_STAT(overrideBubbleNum,  statistics::units::Count::get(), "the number of override bubbles"),
    ADD_STAT(overrideCount, statistics::units::Count::get(), "the number of overrides"),
    ADD_STAT(commitPredsFromEachStage, statistics::units::Count::get(),
    "the number of preds of each stage that account for a committed target"),
    ADD_STAT(commitOverrideBubbleNum, statistics::units::Count::get(),
    "the number of override bubbles, on the commit path"),
    ADD_STAT(commitOverrideCount, statistics::units::Count::get(), "the number of overrides, on the commit path"),
    ADD_STAT(overrideFallThruMismatch, statistics::units::Count::get(),
    "Number of overrides due to validity mismatches, on commit path"),
    ADD_STAT(overrideControlAddrMismatch, statistics::units::Count::get(),
    "Number of overrides due to control address mismatches, on commit path"),
    ADD_STAT(overrideTargetMismatch, statistics::units::Count::get(),
    "Number of overrides due to target mismatches, on commit path"),
    ADD_STAT(overrideEndMismatch, statistics::units::Count::get(),
    "Number of overrides due to end address mismatches, on commit path"),
    ADD_STAT(overrideHistInfoMismatch, statistics::units::Count::get(),
    "Number of overrides due to history info mismatches, on commit path"),
    ADD_STAT(fsqEntryDist, statistics::units::Count::get(), "the distribution of number of entries in fsq"),
    ADD_STAT(fsqEntryEnqueued, statistics::units::Count::get(), "the number of fsq entries enqueued"),
    ADD_STAT(fsqEntryCommitted, statistics::units::Count::get(), "the number of fsq entries committed at last"),
    ADD_STAT(controlSquashFromDecode, statistics::units::Count::get(), "the number of control squashes in bpu from decode"),
    ADD_STAT(controlSquashFromCommit, statistics::units::Count::get(), "the number of control squashes in bpu from commit"),
    ADD_STAT(nonControlSquash, statistics::units::Count::get(), "the number of non-control squashes in bpu"),
    ADD_STAT(trapSquash, statistics::units::Count::get(), "the number of trap squashes in bpu"),
    ADD_STAT(ftqNotValid, statistics::units::Count::get(), "fetch needs ftq req but ftq not valid"),
    ADD_STAT(fsqNotValid, statistics::units::Count::get(), "ftq needs fsq req but fsq not valid"),
    ADD_STAT(fsqFullCannotEnq, statistics::units::Count::get(), "bpu has req but fsq full cannot enqueue"),
    ADD_STAT(ftqFullCannotEnq, statistics::units::Count::get(), "fsq has entry but ftq full cannot enqueue"),
    ADD_STAT(fsqFullFetchHungry, statistics::units::Count::get(), "fetch hungry when fsq full and bpu cannot enqueue"),
    ADD_STAT(fsqEmpty, statistics::units::Count::get(), "fsq is empty"),
    ADD_STAT(commitFsqEntryHasInsts, statistics::units::Count::get(), "number of insts that commit fsq entries have"),
    ADD_STAT(commitFsqEntryFetchedInsts, statistics::units::Count::get(), "number of insts that commit fsq entries fetched"),
    ADD_STAT(commitFsqEntryOnlyHasOneJump, statistics::units::Count::get(), "number of fsq entries with only one instruction (jump)"),
    ADD_STAT(btbHit, statistics::units::Count::get(), "btb hits (in predict block)"),
    ADD_STAT(btbMiss, statistics::units::Count::get(), "btb misses (in predict block)"),
    ADD_STAT(btbEntriesWithDifferentStart, statistics::units::Count::get(), "number of btb entries with different start PC"),
    ADD_STAT(btbEntriesWithOnlyOneJump, statistics::units::Count::get(), "number of btb entries with different start PC starting with a jump"),
    ADD_STAT(predFalseHit, statistics::units::Count::get(), "false hit detected at pred"),
    ADD_STAT(commitFalseHit, statistics::units::Count::get(), "false hit detected at commit"),
    ADD_STAT(predictionBlockedForUpdate, statistics::units::Count::get(), "prediction blocked for update priority"),
    ADD_STAT(predictionBlockedForRunahead, statistics::units::Count::get(),
             "prediction blocked by BPU runahead window"),
    ADD_STAT(commitCallsTotal, statistics::units::Count::get(),
             "calls to DecoupledBPUWithBTB::commit"),
    ADD_STAT(commitWithDoneFtqId, statistics::units::Count::get(),
             "commit calls with a non-zero done FTQ id"),
    ADD_STAT(updatePredictorComponentsTotal, statistics::units::Count::get(),
             "calls to commit-time predictor component update"),
    ADD_STAT(updatePredictorComponentsHitTaken,
             statistics::units::Count::get(),
             "commit-time component updates passing hit-or-taken gate"),
    ADD_STAT(prepareResolveUpdateEntriesTotal,
             statistics::units::Count::get(),
             "calls preparing resolved-update entries"),
    ADD_STAT(prepareResolveUpdateEntriesHitTaken,
             statistics::units::Count::get(),
             "resolved-update prepares passing hit-or-taken gate"),
    ADD_STAT(prepareResolveUpdateEntriesBTBEntries,
             statistics::units::Count::get(),
             "BTB entries prepared for resolved update"),
    ADD_STAT(markCFIResolvedCalls, statistics::units::Count::get(),
             "calls marking resolved CFI entries"),
    ADD_STAT(markCFIResolvedMatchedEntries, statistics::units::Count::get(),
             "resolved CFI marks matching a prepared BTB entry"),
    ADD_STAT(resolveUpdateTotal, statistics::units::Count::get(),
             "calls to DecoupledBPUWithBTB::resolveUpdate"),
    ADD_STAT(resolveUpdateMissingTarget, statistics::units::Count::get(),
             "resolved updates whose FTQ target is missing"),
    ADD_STAT(resolveUpdateSkippedNoHitTaken, statistics::units::Count::get(),
             "resolved updates skipped because target is neither hit nor "
             "taken"),
    ADD_STAT(resolveUpdateHitTaken, statistics::units::Count::get(),
             "resolved updates passing hit-or-taken gate"),
    ADD_STAT(resolveUpdateBlocked, statistics::units::Count::get(),
             "resolved updates blocked by a component readiness check"),
    ADD_STAT(resolveUpdateComponentUpdates, statistics::units::Count::get(),
             "resolved-update component update calls"),
    ADD_STAT(s1PredWrongFallthrough, statistics::units::Count::get(), "S1pred wrong full throughs"),
    ADD_STAT(s1PredWrongUbtb, statistics::units::Count::get(),"S1pred wrong using ubtb "),
    ADD_STAT(s1PredWrongAbtb, statistics::units::Count::get(), "S1pred wrong using abtb "),
    ADD_STAT(s3PredWrongMbtb, statistics::units::Count::get(), "S3pred wrong blame mbtb "),
    ADD_STAT(s3PredWrongTage, statistics::units::Count::get(), "S3pred wrong blame tage "),
    ADD_STAT(s3PredWrongIttage, statistics::units::Count::get(), "S3pred wrong blame ittage "),
    ADD_STAT(s3PredWrongRas, statistics::units::Count::get(), "S3pred wrong blame ras ")

{
    predsOfEachStage.init(numStages);
    commitPredsFromEachStage.init(numStages+1);
    commitOverrideBubbleNum = commitPredsFromEachStage[1] + 2 * commitPredsFromEachStage[2] ;
    commitOverrideCount = commitPredsFromEachStage[1] + commitPredsFromEachStage[2];
    fsqEntryDist.init(0, fsqSize, 20).flags(statistics::total);
    commitFsqEntryHasInsts.init(0, maxInstsNum >> 1, 1);
    commitFsqEntryFetchedInsts.init(0, maxInstsNum >> 1, 1);
    branchClassCounts.init(NumBranchClasses);
    branchClassMisses.init(NumBranchClasses);
    controlSquashByClass.init(NumBranchClasses);
    for (size_t i = 0; i < NumBranchClasses; ++i) {
        branchClassCounts.subname(i, BranchClassLabels[i]);
        branchClassMisses.subname(i, BranchClassLabels[i]);
        controlSquashByClass.subname(i, BranchClassLabels[i]);
    }
}

DecoupledBPUWithBTB::SwayStats::SwayStats(statistics::Group* parent):
    statistics::Group(parent, "sway"),
    ADD_STAT(reallocCount, statistics::units::Count::get(),
             "SWAY owner-register reallocation events"),
    ADD_STAT(reallocBlockedQuiesce, statistics::units::Count::get(),
             "cycles where SWAY quiesce blocks prediction after reallocation"),
    ADD_STAT(ownerOverrides, statistics::units::Count::get(),
             "SWAY transferred ways received by logical owner scope"),
    ADD_STAT(slotOwnerTransitionsPerSlot, statistics::units::Count::get(),
             "SWAY owner transitions grouped by D6 donor slot"),
    ADD_STAT(relaxedRedirectsTriggered, statistics::units::Count::get(),
             "SWAY relaxed-slot redirect events triggered by borrowed hits"),
    ADD_STAT(coolDownBlockedByDonee, statistics::units::Count::get(),
             "SWAY phase decisions blocked by donee table cooldown"),
    ADD_STAT(tableDonationActiveCycles, statistics::units::Count::get(),
             "cycles where each SWAY ITTAGE table-level donor slot is active")
{
    ownerOverrides.init(sway::NumScopes);
    for (uint8_t owner = 0; owner < sway::NumScopes; ++owner) {
        ownerOverrides.subname(owner, sway::scopeName(owner));
    }
    slotOwnerTransitionsPerSlot.init(sway::NumDonorSlots);
    slotOwnerTransitionsPerSlot.subname(0, "mbtb_sram0_way3");
    slotOwnerTransitionsPerSlot.subname(1, "mbtb_sram1_way3");
    slotOwnerTransitionsPerSlot.subname(2, "ittage_t2");
    slotOwnerTransitionsPerSlot.subname(3, "ittage_t3");
    coolDownBlockedByDonee.init(sway::NumTageTables);
    for (uint8_t table = 0; table < sway::NumTageTables; ++table) {
        coolDownBlockedByDonee.subname(table, "tage_t" + std::to_string(table));
    }
    tableDonationActiveCycles.init(sway::NumIttageTables);
    for (uint8_t table = 0; table < sway::NumIttageTables; ++table) {
        tableDonationActiveCycles.subname(table,
            "ittage_t" + std::to_string(table));
    }
}

void DecoupledBPUWithBTB::overrideStats(OverrideReason overrideReason)
{

        // Track specific override reasons for statistics
        switch (overrideReason) {
            case OverrideReason::FALL_THRU:
                dbpBtbStats.overrideFallThruMismatch++;
                break;
            case OverrideReason::CONTROL_ADDR:
                dbpBtbStats.overrideControlAddrMismatch++;
                break;
            case OverrideReason::TARGET:
                dbpBtbStats.overrideTargetMismatch++;
                break;
            case OverrideReason::END:
                dbpBtbStats.overrideEndMismatch++;
                break;
            case OverrideReason::HIST_INFO:
                dbpBtbStats.overrideHistInfoMismatch++;
                break;
            default:
                break;
        }
}

void
DecoupledBPUWithBTB::processFetchDistributions(std::vector<int> &currentPhaseCommittedDist,
                                              std::vector<int> &currentPhaseFetchedDist)
{
    // Initialize distributions with zeros
    currentPhaseCommittedDist.resize(maxInstsNum+1, 0);
    currentPhaseFetchedDist.resize(maxInstsNum+1, 0);

    // Calculate the difference between current and last phase values
    for (int i = 0; i <= maxInstsNum; i++) {
        currentPhaseCommittedDist[i] = commitFsqEntryHasInstsVector[i] -
                                     lastPhaseFsqEntryNumCommittedInstDist[i];
        lastPhaseFsqEntryNumCommittedInstDist[i] = commitFsqEntryHasInstsVector[i];

        currentPhaseFetchedDist[i] = commitFsqEntryFetchedInstsVector[i] -
                                   lastPhaseFsqEntryNumFetchedInstDist[i];
        lastPhaseFsqEntryNumFetchedInstDist[i] = commitFsqEntryFetchedInstsVector[i];
    }
}

std::unordered_map<Addr, std::pair<BTBEntry, int>>
DecoupledBPUWithBTB::processBTBEntries()
{
    std::unordered_map<Addr, std::pair<BTBEntry, int>> currentPhaseBTBEntries;

    // Process each BTB entry
    for (auto &it : totalBTBEntries) {
        auto &entry = it.second.first;
        auto visit_cnt = it.second.second;

        // Check if this entry was already present in last phase
        auto last_it = lastPhaseBTBEntries.find(it.first);
        if (last_it != lastPhaseBTBEntries.end()) {
            visit_cnt -= last_it->second.second;
        }

        // Only add entries with new visits in this phase
        if (visit_cnt > 0) {
            currentPhaseBTBEntries[it.first] = std::make_pair(entry, visit_cnt);
        }
    }

    // Update last phase BTB entries for next time
    lastPhaseBTBEntries = totalBTBEntries;

    return currentPhaseBTBEntries;
}

bool
DecoupledBPUWithBTB::processPhase(bool isSubPhase, int phaseID, int &phaseToDump,
                                BranchStatsMap &lastPhaseStats,
                                std::vector<BranchStatsMap> &phaseStatsList,
                                std::unordered_map<Addr, int> &currentPhaseBranches,
                                std::vector<std::unordered_map<Addr, int>> &phaseBranchesList)
{
    // Check if this phase should be processed
    if (phaseToDump > phaseID) {
        return false;
    }

    // Debug output
    DPRINTF(Profiling, "dump %s phase %d\n",
            isSubPhase ? "sub" : "main", phaseToDump);

    // Create map for current phase statistics
    BranchStatsMap currentPhaseStats;

    // Process each branch in the global statistics
    for (auto &it : topMispredictsByBranch) {
        const auto &key = it.first;
        const auto &stats = it.second;

        // Find stats from last phase
        auto lastIt = lastPhaseStats.find(key);

        // If branch exists in last phase, calculate difference
        if (lastIt != lastPhaseStats.end()) {
            const auto &lastStats = lastIt->second;

            // Skip branches with no new executions
            if (stats.totalCount <= lastStats.totalCount) {
                continue;
            }

            // Create stats for current phase (delta from last phase)
            BranchStats phaseStats(stats.pc, stats.branchType);
            phaseStats.totalCount = stats.totalCount - lastStats.totalCount;
            phaseStats.mispredCount = stats.mispredCount - lastStats.mispredCount;
            phaseStats.dirWrongCount = stats.dirWrongCount - lastStats.dirWrongCount;
            phaseStats.targetWrongCount = stats.targetWrongCount - lastStats.targetWrongCount;
            phaseStats.noPredCount = stats.noPredCount - lastStats.noPredCount;

            currentPhaseStats[key] = phaseStats;
        } else {
            // This is a new branch in this phase
            currentPhaseStats[key] = stats;
        }
    }

    // Store the processed data
    lastPhaseStats = topMispredictsByBranch;
    phaseStatsList.push_back(currentPhaseStats);

    // Handle taken branches map
    phaseBranchesList.push_back(currentPhaseBranches);
    currentPhaseBranches.clear();

    // Increment phase counter for next time
    phaseToDump++;

    return true;
}

void
DecoupledBPUWithBTB::dumpFsq(const char *when)
{
    // DPRINTF(DecoupleBPProbe, "dumping fsq entries %s...\n", when);
    // for (size_t i = 0; i < fetchTargetQueue.size(); ++i) {
    //     DPRINTFR(DecoupleBPProbe, "TargetID %lu, ",
    //              static_cast<uint64_t>(fetchTargetBaseId + i));
    //     printTarget(fetchTargetQueue[i]);
    // }
}

DecoupledBPUWithBTB::BranchClass
DecoupledBPUWithBTB::classifyBranch(const DynInstPtr &inst) const
{
    return classifyBranchImpl(inst);
}

DecoupledBPUWithBTB::BranchClass
DecoupledBPUWithBTB::classifyBranch(const StaticInstPtr &inst) const
{
    return classifyBranchImpl(inst);
}

const char *
DecoupledBPUWithBTB::branchClassName(BranchClass cls)
{
    auto idx = static_cast<size_t>(cls);
    if (idx < BranchClassLabels.size()) {
        return BranchClassLabels[idx];
    }
    return "invalid";
}

void
DecoupledBPUWithBTB::addBranchClassStat(BranchClass cls, bool mispred)
{
    auto idx = static_cast<size_t>(cls);
    if (idx >= NumBranchClasses) {
        DPRINTF(DBPBTBStats, "Skip invalid branch class stats update %d\n",
                static_cast<int>(cls));
        return;
    }

    dbpBtbStats.branchClassCounts[idx]++;
    if (mispred) {
        dbpBtbStats.branchClassMisses[idx]++;
        dbpBtbStats.branchClassCountsTotal++;
    }

    DPRINTF(DBPBTBStats, "Branch classified as %s, mispred=%d\n",
            branchClassName(cls), mispred);
}

void
DecoupledBPUWithBTB::addControlSquashCommitStat(BranchClass cls)
{
    auto idx = static_cast<size_t>(cls);
    if (idx >= NumBranchClasses) {
        DPRINTF(DBPBTBStats,
                "Skip invalid commit squash class stats update %d\n",
                static_cast<int>(cls));
        return;
    }

    dbpBtbStats.controlSquashByClass[idx]++;
    DPRINTF(DBPBTBStats, "Commit squash classified as %s\n",
            branchClassName(cls));
}

void
DecoupledBPUWithBTB::updateStatistics(const FetchTarget &target)
{
    // Check if this target was mispredicted
    bool miss_predicted = target.squashType == SQUASH_CTRL;
    // Track indirect mispredictions
    if (miss_predicted && target.exeBranchInfo.isIndirect) {
        topMispredIndirect[target.startPC]++;
    }

    // --- BTB Statistics ---
    if (target.isHit) {
        // Count BTB hits
        dbpBtbStats.btbHit++;
    } else {
        // Count BTB misses for taken branches
        if (target.exeTaken) {
            dbpBtbStats.btbMiss++;
            DPRINTF(BTB, "BTB miss detected when update, target start %#lx, predTick %lu, printing branch info:\n",
                    target.startPC, target.predTick);
            auto &slot = target.exeBranchInfo;
            DPRINTF(BTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d\n",
                slot.pc, slot.size, slot.target, slot.isCond, slot.isIndirect, slot.isCall, slot.isReturn);
        }

        // Count false hits
        if (target.falseHit) {
            dbpBtbStats.commitFalseHit++;
        }
    }

    if (target.isHit || target.exeTaken) {
        // Update BTB entry statistics
        auto it = totalBTBEntries.find(target.startPC);
        if (it == totalBTBEntries.end()) {
            auto &btb_entry = target.updateNewBTBEntry;
            totalBTBEntries[target.startPC] = std::make_pair(btb_entry, 1);
            dbpBtbStats.btbEntriesWithDifferentStart++;
        } else {
            it->second.second++;
            it->second.first = target.updateNewBTBEntry;
        }
    }

    // Track which predictor stage was used
    dbpBtbStats.commitPredsFromEachStage[target.predSource]++;
    overrideStats(target.overrideReason);

    // --- Instruction Statistics ---
    // Track committed instruction counts
    dbpBtbStats.commitFsqEntryHasInsts.sample(target.commitInstNum, 1);
    if (target.commitInstNum >= 0 && target.commitInstNum <= maxInstsNum) {
        commitFsqEntryHasInstsVector[target.commitInstNum]++;
        if (target.commitInstNum == 1 && target.exeBranchInfo.isUncond()) {
            dbpBtbStats.commitFsqEntryOnlyHasOneJump++;
        }
    }

    // Track fetched instruction counts
    dbpBtbStats.commitFsqEntryFetchedInsts.sample(target.fetchInstNum, 1);
    if (target.fetchInstNum >= 0 && target.fetchInstNum <= maxInstsNum) {
        commitFsqEntryFetchedInstsVector[target.fetchInstNum]++;
    }

    // --- Misprediction Statistics ---
    // Track control squashes (mispredictions)
    if (target.squashType == SQUASH_CTRL) {
        // Record mispredict pair (start PC, branch PC)
        auto find_it = topMispredicts.find(std::make_pair(target.startPC, target.exeBranchInfo.pc));
        if (find_it == topMispredicts.end()) {
            topMispredicts[std::make_pair(target.startPC, target.exeBranchInfo.pc)] = 1;
        } else {
            find_it->second++;
        }

        // Track history pattern for mispredictions
        auto hist(target.history);
        hist.resize(18);
        uint64_t pattern = hist.to_ulong();
        auto find_it_hist = topMispredHist.find(pattern);
        if (find_it_hist == topMispredHist.end()) {
            topMispredHist[pattern] = 1;
        } else {
            find_it_hist->second++;
        }
    }
}

void
DecoupledBPUWithBTB::commitBranch(const DynInstPtr &inst, bool mispred)
{
    // ---------- Update overall branch statistics ----------
    if (inst->isUncondCtrl()) {
        addCfi(UNCOND, mispred);
    }
    if (inst->isCondCtrl()) {
        addCfi(COND, mispred);
    }
    if (inst->isReturn()) {
        addCfi(RETURN, mispred);
    } else if (inst->isIndirectCtrl()) {
        addCfi(OTHER, mispred);
    }

    auto branchClass = classifyBranch(inst);
    addBranchClassStat(branchClass, mispred);

    // ---------- Find corresponding fetch target entry ----------
    auto entry = ftq.get(inst->ftqId, inst->threadNumber);

    // Record branch trace if enabled
    if (enableBranchTrace) {
        bptrace->write_record(BpTrace(inst->ftqId, entry, inst, mispred));
    }

    // ---------- Extract branch information ----------
    Addr branchAddr = inst->pcState().instAddr();
    const auto &rv_pc = inst->pcState().as<RiscvISA::PCState>();
    Addr targetAddr = inst->hasTraceBranchInfo() ?
        inst->traceBranchNextPC() : rv_pc.npc();
    Addr fallThruPC = rv_pc.getFallThruPC();
    BranchInfo info = makeBranchInfo(
        branchAddr, targetAddr, inst, inst->staticInst, fallThruPC-branchAddr);
    bool taken = inst->hasTraceBranchInfo() ?
        inst->traceBranchTaken() : (rv_pc.branching() || inst->isUncondCtrl());

    // ---------- Process misprediction and update statistics ----------
    processMisprediction(entry, branchAddr, info, taken, mispred);

    // ---------- Track taken branches for statistics ----------
    if (taken) {
        trackTakenBranch(branchAddr);
    }

    // ---------- Update predictor components ----------
    for (auto component : components) {
        component->commitBranch(entry, inst);
    }
    //here add final counter

    if (mispred) {
        commitPredWrongSource(entry);
    }

}

void
DecoupledBPUWithBTB::commitPredWrongSource(const FetchTarget &entry)
{
    int ubtbid = ubtb->getComponentIdx();
    int abtbid = abtb->getComponentIdx();
    int mbtbid = mbtb->getComponentIdx();
    int tageid = tage->getComponentIdx();
    int ittageid = ittage->getComponentIdx();
    int rasid = ras->getComponentIdx();

    int s1PredSource = entry.s1Source;
    int s3PredSource = entry.s3Source;

    auto exeBranchInfo = entry.exeBranchInfo;

    bool onlyDirectionWrong = entry.exeTaken != entry.predTaken;

    assert(s1PredSource < mbtbid);
    if (s1PredSource == ubtbid) {
        dbpBtbStats.s1PredWrongUbtb++;
    } else if (s1PredSource == abtbid) {
        dbpBtbStats.s1PredWrongAbtb++;
    }else {
        dbpBtbStats.s1PredWrongFallthrough++;
    }

    if (s3PredSource == rasid) {
        if (exeBranchInfo.isCond) {
            dbpBtbStats.s3PredWrongTage++;
        } else if (exeBranchInfo.isReturn) {
            dbpBtbStats.s3PredWrongRas++;
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    } else if (s3PredSource == ittageid) {
        if (exeBranchInfo.isIndirect) {
            dbpBtbStats.s3PredWrongIttage++;
        } else if (exeBranchInfo.isCond) {
            dbpBtbStats.s3PredWrongTage++;
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    } else if (s3PredSource == tageid) {
        if (exeBranchInfo.isCond) {
            if (onlyDirectionWrong) {
                dbpBtbStats.s3PredWrongTage++;
            } else {
                dbpBtbStats.s3PredWrongMbtb++;
            }
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    }else if (s3PredSource == mbtbid) {
        if (exeBranchInfo.isCond) {
            if (onlyDirectionWrong) {
                dbpBtbStats.s3PredWrongTage++;
            } else {
                dbpBtbStats.s3PredWrongMbtb++;
            }
        } else if (exeBranchInfo.isIndirect) {
            dbpBtbStats.s3PredWrongIttage++;
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    }else if (s3PredSource == -1) {
        dbpBtbStats.s3PredWrongMbtb++;
    }
}
/**
 * @brief Handle instruction commits and phase-based statistics
 *
 * This function is called whenever an instruction is committed. It updates
 * instruction counts and maintains phase-based statistics. When a phase
 * boundary is reached, it collects detailed statistics for the phase.
 */
void
DecoupledBPUWithBTB::notifyInstCommit(const DynInstPtr &inst)
{
    // Update committed instruction count for target
    ftq.get(inst->ftqId, inst->threadNumber).commitInstNum++;

    // Update global committed instruction count
    numInstCommitted++;

    DPRINTF(Profiling, "notifyInstCommit, inst=%s, commitInstNum=%d\n",
            inst->staticInst->disassemble(inst->pcState().instAddr()),
            ftq.get(inst->ftqId, inst->threadNumber).commitInstNum);

    // ----------------------- Main Phase Processing -------------------------
    if (numInstCommitted % phaseSizeByInst == 0) {
        int currentPhaseID = numInstCommitted / phaseSizeByInst;

        // Process main phase statistics if needed
        if (processPhase(false, currentPhaseID, phaseIdToDump,
                        lastPhaseTopMispredictsByBranch,
                        topMispredictsByBranchByPhase,
                        currentPhaseTakenBranches,
                        takenBranchesByPhase)) {

            // Process fetch instruction distributions
            std::vector<int> committedInstDist, fetchedInstDist;
            processFetchDistributions(committedInstDist, fetchedInstDist);

            // Store the distributions
            fsqEntryNumCommittedInstDistByPhase.push_back(committedInstDist);
            fsqEntryNumFetchedInstDistByPhase.push_back(fetchedInstDist);

            // Process BTB entries
            BTBEntriesByPhase.push_back(processBTBEntries());

            // SWAY: snapshot per-way visit counters across MBTB + TAGE/microtage.
            collectSwayWayVisitForPhase(currentPhaseID);
        }
    }

    // ---------------------- Sub-Phase Processing --------------------------
    if (numInstCommitted % subPhaseSizeByInst() == 0) {
        int currentSubPhaseID = numInstCommitted / subPhaseSizeByInst();

        // Process sub-phase statistics if needed
        processPhase(true, currentSubPhaseID, subPhaseIdToDump,
                    lastSubPhaseTopMispredictsByBranch,
                    topMispredictsByBranchBySubPhase,
                    currentSubPhaseTakenBranches,
                    takenBranchesBySubPhase);
    }
}


/**
 * @brief Process branch misprediction, determine type and update statistics
 *
 * @param entry The fetch target entry
 * @param branchAddr Branch instruction address
 * @param info Branch information
 * @param taken Whether the branch was taken
 * @param mispred Whether the branch was mispredicted
 */
void
DecoupledBPUWithBTB::processMisprediction(
    const FetchTarget &entry,
    Addr branchAddr,
    const BranchInfo &info,
    bool taken,
    bool mispred)
{
    MispredType mispredType = FAKE_LAST;

    // Determine misprediction type only if there was a misprediction
    if (mispred) {
        if (!taken) {
            // Only conditional branches can be not-taken
            assert(info.isCond);
            mispredType = DIR_WRONG; // Direction was wrong
        } else {
            // Check if this branch was in the predicted BTB entries
            bool predBranchInBTB = false;
            for (auto &e: entry.predBTBEntries) {
                if (e.pc == branchAddr) {
                    predBranchInBTB = true;
                    break;
                }
            }

            if (!predBranchInBTB) {
                mispredType = NO_PRED; // Branch wasn't predicted at all
            } else if (entry.predTaken && entry.predBranchInfo.pc == branchAddr) {
                mispredType = TARGET_WRONG; // Branch predicted taken but wrong target
            } else {
                // Branch predicted not taken or different branch predicted taken
                mispredType = DIR_WRONG;
            }
        }

        DPRINTF(Profiling, "branchAddr %#lx is mispredicted, taken %d, type %d, missType %d\n",
                branchAddr, taken, info.getType(), mispredType);
        assert(mispredType != FAKE_LAST);
    }

    // Create branch key for the statistics map
    auto branchKey = std::make_pair(branchAddr, info.getType());

    // Update branch statistics
    DPRINTF(Profiling, "lookup topMispredictsByBranch for branchAddr %#lx, type %d\n",
            branchAddr, info.getType());

    auto statsIt = topMispredictsByBranch.find(branchKey);

    if (statsIt == topMispredictsByBranch.end()) {
        // Create new statistics entry for this branch
        DPRINTF(Profiling, "not found, insert with mispred=%d\n", mispred);

        // Initialize new branch stats
        BranchStats stats(branchAddr, info.getType());
        stats.incrementTotal();  // Always increment total count

        // Only increment misprediction count if actually mispredicted
        if (mispred) {
            stats.incrementMispred(mispredType);
        }

        // Store in map
        topMispredictsByBranch[branchKey] = stats;
        dbpBtbStats.staticBranchNum++;
    } else {
        // Update existing statistics entry
        DPRINTF(Profiling, "found, total %d, miss %d\n",
                statsIt->second.totalCount, statsIt->second.mispredCount);

        // Always increment total count
        statsIt->second.incrementTotal();

        // Only increment misprediction count if actually mispredicted
        if (mispred) {
            statsIt->second.incrementMispred(mispredType);
        }
    }
}

/**
 * @brief Track statistics for taken branches
 *
 * @param branchAddr Branch instruction address
 */
void
DecoupledBPUWithBTB::trackTakenBranch(Addr branchAddr)
{
    // Helper function to update a branch map
    auto updateBranchMap = [branchAddr](std::unordered_map<Addr, int> &branchMap) {
        auto it = branchMap.find(branchAddr);
        if (it == branchMap.end()) {
            // Branch not found - add with count 1
            branchMap[branchAddr] = 1;
        } else {
            // Branch found - increment count
            it->second++;
        }
    };

    // Update all three branch maps
    updateBranchMap(takenBranches);
    updateBranchMap(currentPhaseTakenBranches);
    updateBranchMap(currentSubPhaseTakenBranches);
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
