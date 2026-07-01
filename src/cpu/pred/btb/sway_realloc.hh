#ifndef __CPU_PRED_BTB_SWAY_REALLOC_HH__
#define __CPU_PRED_BTB_SWAY_REALLOC_HH__

#include <array>
#include <cstdint>
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

static constexpr uint8_t NumComponents = 3;
static constexpr uint8_t NumDonorSlots = 4;
static constexpr uint8_t MbtbSram0 = 0;
static constexpr uint8_t MbtbSram1 = 1;
static constexpr uint8_t TageBase = 2;
static constexpr uint8_t NumTageTables = 8;
static constexpr uint8_t NumIttageTables = 5;
static constexpr uint8_t NumScopes = TageBase + NumTageTables;
static constexpr uint8_t InvalidOwner = 0xff;
static constexpr uint8_t InvalidIndex = 0xff;
static constexpr uint8_t NumMbtbTightSlots = 2;
static constexpr uint8_t MbtbTightSlotWay = 3;
static constexpr unsigned MaxSetRatio = 4;
static constexpr unsigned MbtbEntryBits = 70;
static constexpr unsigned TageEntryBits = 18;
static constexpr unsigned IttageEntryBits = 53;
static constexpr double TightHysteresisMargin = 0.10;
static constexpr double RelaxedHysteresisMargin = 0.20;
static constexpr double IttageColdUtilityThreshold = 0.30;
static constexpr double TageHotUtilityThreshold = 0.70;
static constexpr unsigned IttageColdPhaseThreshold = 5;

using ScopeCounts = std::array<unsigned, NumScopes>;

enum class Component : uint8_t
{
    MBTB = 0,
    ITTAGE = 1,
    TAGE = 2,
    Invalid = InvalidOwner
};

enum class DonorSlotKind : uint8_t
{
    WayLevel = 0,
    TableLevel = 1
};

enum class DonorLatencyClass : uint8_t
{
    Tight = 0,
    Relaxed = 1
};

enum class SlotOwner : uint8_t
{
    Native = 0,
    DoneeA = 1,
    DoneeB = 2,
    Invalid = 3
};

enum class ExtraBitSource : uint8_t
{
    GhrFoldHigh = 0,
    PathHistBit = 1,
    PcHighBit = 2,
    Disabled = 3
};

enum class IndexRule : uint8_t
{
    SameSets = 0,
    DonorLarger = 1,
    DoneeLarger = 2,
    Invalid = 3
};

enum class ControllerAction : uint8_t
{
    None = 0,
    MbtbToTageTight = 1,
    ReturnMbtbTight = 2,
    IttageToTageRelaxed = 3,
    ReturnIttageRelaxed = 4
};

struct BorrowedBankGeometry
{
    uint32_t donorSets = 0;
    uint32_t doneeSets = 0;
    uint16_t donorEntryBits = 0;
    uint16_t doneeEntryBits = 0;
    uint8_t extraTagBits = 0;
    uint8_t packingFactor = 0;
    IndexRule indexRule = IndexRule::Invalid;

    bool legal() const
    {
        return indexRule != IndexRule::Invalid && packingFactor > 0;
    }
};

struct DoneeTableSet
{
    std::vector<uint8_t> tableIds;

    bool empty() const { return tableIds.empty(); }
    size_t size() const { return tableIds.size(); }

    bool contains(uint8_t table) const
    {
        for (auto id : tableIds) {
            if (id == table) {
                return true;
            }
        }
        return false;
    }
};

struct DonorSlot
{
    DonorSlot()
    {
        extraBitPerDonee.fill(ExtraBitSource::Disabled);
    }

    uint8_t id = InvalidIndex;
    Component sourceComponent = Component::Invalid;
    uint8_t sourceIndex = InvalidIndex;
    uint8_t sourceWay = InvalidIndex;
    DonorSlotKind kind = DonorSlotKind::WayLevel;
    DonorLatencyClass latencyClass = DonorLatencyClass::Tight;
    uint32_t capacityBytes = 0;
    std::array<ExtraBitSource, NumTageTables> extraBitPerDonee{};
    SlotOwner owner = SlotOwner::Native;
};

inline bool
isMbtbOwner(uint8_t owner)
{
    return owner == MbtbSram0 || owner == MbtbSram1;
}

inline bool
isTageOwner(uint8_t owner)
{
    return owner >= TageBase && owner < NumScopes;
}

inline uint8_t
mbtbOwner(unsigned sram)
{
    return sram == 0 ? MbtbSram0 : MbtbSram1;
}

inline uint8_t
tageOwner(unsigned table)
{
    return TageBase + static_cast<uint8_t>(table);
}

inline unsigned
mbtbSram(uint8_t owner)
{
    return owner == MbtbSram1 ? 1 : 0;
}

inline unsigned
tageTable(uint8_t owner)
{
    return owner - TageBase;
}

inline bool
isTageTableId(unsigned table)
{
    return table < NumTageTables;
}

struct MbtbTightSlotState
{
    uint8_t slotId = InvalidIndex;
    uint8_t sourceSram = InvalidIndex;
    uint8_t sourceWay = InvalidIndex;
    uint8_t owner = InvalidOwner;

    bool donatedToTage() const
    {
        return isTageOwner(owner);
    }

    unsigned doneeTable() const
    {
        return tageTable(owner);
    }
};

inline bool
isPowerOfTwo(uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

inline uint8_t
log2PowerOfTwo(uint32_t value)
{
    uint8_t bits = 0;
    while (value > 1) {
        value >>= 1;
        ++bits;
    }
    return bits;
}

inline BorrowedBankGeometry
makeBorrowedBankGeometry(uint32_t donorSets, uint32_t doneeSets,
                         uint16_t donorEntryBits,
                         uint16_t doneeEntryBits)
{
    BorrowedBankGeometry geometry;
    geometry.donorSets = donorSets;
    geometry.doneeSets = doneeSets;
    geometry.donorEntryBits = donorEntryBits;
    geometry.doneeEntryBits = doneeEntryBits;

    if (!isPowerOfTwo(donorSets) || !isPowerOfTwo(doneeSets) ||
        donorEntryBits == 0 || doneeEntryBits == 0) {
        return geometry;
    }

    if (donorSets == doneeSets) {
        geometry.indexRule = IndexRule::SameSets;
    } else if (donorSets > doneeSets) {
        const uint32_t ratio = donorSets / doneeSets;
        if (donorSets % doneeSets != 0 || ratio > MaxSetRatio) {
            return geometry;
        }
        geometry.indexRule = IndexRule::DonorLarger;
    } else {
        const uint32_t ratio = doneeSets / donorSets;
        if (doneeSets % donorSets != 0 || ratio > MaxSetRatio) {
            return geometry;
        }
        geometry.indexRule = IndexRule::DoneeLarger;
        geometry.extraTagBits = log2PowerOfTwo(ratio);
    }

    const unsigned subEntryBits = doneeEntryBits + geometry.extraTagBits;
    geometry.packingFactor = subEntryBits == 0 ? 0 :
        donorEntryBits / subEntryBits;
    if (geometry.packingFactor == 0) {
        geometry.indexRule = IndexRule::Invalid;
    }
    return geometry;
}

inline BorrowedBankGeometry
makeMbtbToTageGeometry(uint32_t mbtbSets, uint32_t tageSets)
{
    return makeBorrowedBankGeometry(mbtbSets, tageSets, MbtbEntryBits,
                                    TageEntryBits);
}

inline uint32_t
borrowedIndexR2b(uint32_t nativeIndex, uint32_t donorSets)
{
    return nativeIndex & (donorSets - 1);
}

inline uint32_t
borrowedExtraTagR2b(uint32_t nativeIndex, uint32_t donorSets)
{
    return nativeIndex >> log2PowerOfTwo(donorSets);
}

inline unsigned
tageDoneeCooldownPhases(unsigned table)
{
    static constexpr std::array<unsigned, NumTageTables> phases = {
        4, 5, 6, 8, 10, 13, 16, 20
    };
    return table < phases.size() ? phases[table] : 1;
}

inline unsigned
ittageDoneeCooldownPhases(unsigned doneeTable)
{
    const unsigned doneeCooldown = tageDoneeCooldownPhases(doneeTable);
    return doneeCooldown > IttageColdPhaseThreshold ?
        doneeCooldown : IttageColdPhaseThreshold;
}

inline const char *
componentName(Component component)
{
    switch (component) {
      case Component::MBTB:
        return "MBTB";
      case Component::ITTAGE:
        return "ITTAGE";
      case Component::TAGE:
        return "TAGE";
      case Component::Invalid:
        return "invalid";
    }
    return "invalid";
}

inline const char *
scopeName(uint8_t owner)
{
    static constexpr std::array<const char *, NumScopes> names = {
        "mbtb_sram0", "mbtb_sram1",
        "tage_t0", "tage_t1", "tage_t2", "tage_t3",
        "tage_t4", "tage_t5", "tage_t6", "tage_t7"
    };
    return owner < names.size() ? names[owner] : "invalid";
}

inline uint8_t
ownerFromScope(const std::string &scope)
{
    for (uint8_t owner = 0; owner < NumScopes; ++owner) {
        if (scope == scopeName(owner)) {
            return owner;
        }
    }
    return InvalidOwner;
}

} // namespace sway

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_BTB_SWAY_REALLOC_HH__
