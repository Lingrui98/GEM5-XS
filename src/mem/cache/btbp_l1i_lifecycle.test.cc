#include <gtest/gtest.h>

#include "mem/cache/btbp_l1i_lifecycle.hh"
#include "mem/cache/cache_blk.hh"

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

TEST(BtbpL1iLifecycleTest, InstallConflictDoesNotMutateEitherMapping)
{
    Ledger ledger;
    const Ledger::LineKey key{0x2800, false};
    EXPECT_FALSE(ledger.install(key, {7, true, true, false}));

    const auto rejected = ledger.install(key, {8, false, false, false});
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->id, 7);
    EXPECT_TRUE(ledger.isCurrent(key, 7));
    EXPECT_FALSE(ledger.isCurrent(key, 8));
    EXPECT_TRUE(ledger.close(key, 7));
    EXPECT_FALSE(ledger.close(key, 8));

    EXPECT_FALSE(ledger.install(key, {8, false, false, false}));
    EXPECT_TRUE(ledger.isCurrent(key, 8));
    EXPECT_TRUE(ledger.close(key, 8));
}

TEST(BtbpL1iLifecycleTest, PrefetchMarkPreservesFillOwnedMetadata)
{
    CacheBlk block;
    Request::XsMetadata fill_metadata(PF_FDIP);
    fill_metadata.instPrefetchAttemptId = 11;
    fill_metadata.instPrefetchDecisionId = 12;
    fill_metadata.l1iResidencyId = 13;
    fill_metadata.btbpRoiOrigin = true;
    block.setXsMetadata(fill_metadata);

    block.setPrefetched();

    const auto marked_metadata = block.getXsMetadata();
    EXPECT_TRUE(block.wasPrefetched());
    EXPECT_EQ(marked_metadata.instPrefetchAttemptId, 11);
    EXPECT_EQ(marked_metadata.instPrefetchDecisionId, 12);
    EXPECT_EQ(marked_metadata.l1iResidencyId, 13);
    EXPECT_TRUE(marked_metadata.btbpRoiOrigin);
}

TEST(BtbpL1iLifecycleTest, CapacityBoundFailsClosed)
{
    Ledger ledger(2);
    EXPECT_FALSE(ledger.install({0x3000, false}, {1, false, false, false}));
    EXPECT_FALSE(ledger.install({0x4000, false}, {2, false, false, false}));
    EXPECT_ANY_THROW(
        ledger.install({0x5000, false}, {3, false, false, false}));
    EXPECT_EQ(ledger.size(), 2);
    EXPECT_TRUE(ledger.isCurrent({0x3000, false}, 1));
    EXPECT_TRUE(ledger.isCurrent({0x4000, false}, 2));
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
