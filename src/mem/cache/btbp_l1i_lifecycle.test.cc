#include <gtest/gtest.h>

#include "mem/cache/btbp_l1i_lifecycle.hh"

namespace gem5
{
namespace
{

using Ledger = BtbpL1iLifecycleLedger;

TEST(BtbpL1iLifecycleTest, SameLineRefillClosesEverySourceCombination)
{
    for (const bool old_prefetch : {false, true}) {
        for (const bool new_prefetch : {false, true}) {
            Ledger ledger;
            const Ledger::LineKey key{0x1000, false};
            EXPECT_FALSE(ledger.install(
                key, {1, old_prefetch, old_prefetch, false}));
            EXPECT_TRUE(ledger.close(key, 1));
            EXPECT_FALSE(ledger.install(
                key, {2, new_prefetch, new_prefetch, false}));
            EXPECT_EQ(ledger.size(), 1);
            EXPECT_TRUE(ledger.close(key, 2));
            EXPECT_EQ(ledger.size(), 0);
        }
    }
}

TEST(BtbpL1iLifecycleTest, EvictionAndDemandUseRequireCurrentIdentity)
{
    Ledger ledger;
    const Ledger::LineKey key{0x2000, true};
    EXPECT_FALSE(ledger.install(key, {7, true, true, false}));
    EXPECT_FALSE(ledger.markDemandUse(key, 6));
    EXPECT_TRUE(ledger.markDemandUse(key, 7));
    EXPECT_FALSE(ledger.close(key, 6));
    EXPECT_TRUE(ledger.close(key, 7));
}

TEST(BtbpL1iLifecycleTest, DrainClosesOnlyRoiOriginPrefetchResidencies)
{
    Ledger ledger;
    EXPECT_FALSE(ledger.install({0x1000, false}, {1, true, true, false}));
    EXPECT_FALSE(ledger.install({0x2000, false}, {2, true, false, false}));
    EXPECT_FALSE(ledger.install({0x3000, false}, {3, false, true, false}));

    const auto drained = ledger.drainRoiPrefetch();
    ASSERT_EQ(drained.size(), 1);
    EXPECT_EQ(drained.front().second.id, 1);
    EXPECT_EQ(ledger.size(), 2);
    EXPECT_TRUE(ledger.close({0x2000, false}, 2));
    EXPECT_TRUE(ledger.close({0x3000, false}, 3));
}

} // namespace
} // namespace gem5
