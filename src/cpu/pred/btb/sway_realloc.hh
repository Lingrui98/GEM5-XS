#ifndef __CPU_PRED_BTB_SWAY_REALLOC_HH__
#define __CPU_PRED_BTB_SWAY_REALLOC_HH__

#include <array>
#include <cstdint>
#include <string>

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace sway
{

static constexpr uint8_t MbtbSram0 = 0;
static constexpr uint8_t MbtbSram1 = 1;
static constexpr uint8_t TageBase = 2;
static constexpr uint8_t NumTageTables = 8;
static constexpr uint8_t NumScopes = TageBase + NumTageTables;
static constexpr uint8_t InvalidOwner = 0xff;

using ScopeCounts = std::array<unsigned, NumScopes>;

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
