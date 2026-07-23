#include <gtest/gtest.h>

#include "mem/cache/btbp_l1i_lifecycle.hh"
#include "mem/cache/cache_blk.hh"

namespace gem5
{
namespace
{

using Ledger = BtbpL1iLifecycleLedger;

Request::XsMetadata
makeMetadata(uint64_t residency_id, bool prefetch, bool roi_origin = false)
{
    Request::XsMetadata metadata;
    metadata.l1iResidencyId = residency_id;
    metadata.prefetchSource = prefetch ? PF_FDIP : PF_NONE;
    metadata.btbpRoiOrigin = roi_origin;
    return metadata;
}

TEST(BtbpL1iLifecycleTest, SameLineRefillClosesEverySourceCombination)
{
    for (const bool old_prefetch : {false, true}) {
        for (const bool new_prefetch : {false, true}) {
            Ledger ledger;
            const Ledger::LineKey key{0x1000, false};
            CacheBlk block;
            block.insert(key.lineAddr, key.secure);
            transitionBtbpL1iFillMetadata(
                ledger, key, block,
                makeMetadata(1, old_prefetch, old_prefetch), false,
                old_prefetch,
                [](const Request::XsMetadata &) { ADD_FAILURE(); });

            bool closed_old = false;
            transitionBtbpL1iFillMetadata(
                ledger, key, block,
                makeMetadata(2, new_prefetch, new_prefetch), true,
                new_prefetch,
                [&](const Request::XsMetadata &old_metadata) {
                    EXPECT_EQ(old_metadata.l1iResidencyId, 1);
                    EXPECT_EQ(
                        block.getXsMetadata().l1iResidencyId, 1);
                    closed_old = true;
                });
            EXPECT_TRUE(closed_old);
            EXPECT_EQ(ledger.size(), 1);
            EXPECT_TRUE(ledger.isCurrent(key, 2));
            EXPECT_EQ(block.getXsMetadata().l1iResidencyId, 2);
            EXPECT_TRUE(closeBtbpL1iBlockResidency(ledger, key, block));
            EXPECT_EQ(ledger.size(), 0);
            block.invalidate();
        }
    }
}

TEST(BtbpL1iLifecycleTest, EvictionAndDemandUseRequireCurrentIdentity)
{
    Ledger ledger;
    const Ledger::LineKey key{0x2000, true};
    CacheBlk block;
    block.insert(key.lineAddr, key.secure);
    transitionBtbpL1iFillMetadata(
        ledger, key, block, makeMetadata(7, true, true), false, true,
        [](const Request::XsMetadata &) { ADD_FAILURE(); });
    EXPECT_TRUE(markBtbpL1iDemandUse(ledger, key, block));
    EXPECT_TRUE(closeBtbpL1iBlockResidency(ledger, key, block));
    block.invalidate();
    EXPECT_EQ(block.getXsMetadata().l1iResidencyId, 0);
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
    Ledger ledger;
    const Ledger::LineKey key{0x2c00, false};
    CacheBlk block;
    block.insert(key.lineAddr, key.secure);
    Request::XsMetadata fill_metadata(PF_FDIP);
    fill_metadata.instPrefetchAttemptId = 11;
    fill_metadata.instPrefetchDecisionId = 12;
    fill_metadata.l1iResidencyId = 13;
    fill_metadata.btbpRoiOrigin = true;
    transitionBtbpL1iFillMetadata(
        ledger, key, block, fill_metadata, false, true,
        [](const Request::XsMetadata &) { ADD_FAILURE(); });

    completeBtbpL1iPrefetch(ledger, key, block, true);

    const auto marked_metadata = block.getXsMetadata();
    EXPECT_TRUE(block.wasPrefetched());
    EXPECT_EQ(marked_metadata.instPrefetchAttemptId, 11);
    EXPECT_EQ(marked_metadata.instPrefetchDecisionId, 12);
    EXPECT_EQ(marked_metadata.l1iResidencyId, 13);
    EXPECT_TRUE(marked_metadata.btbpRoiOrigin);
    EXPECT_TRUE(ledger.isCurrent(key, 13));
    EXPECT_TRUE(closeBtbpL1iBlockResidency(ledger, key, block));
    block.invalidate();
}

TEST(BtbpL1iLifecycleTest, CapacityBoundFailsClosed)
{
    Ledger ledger(2);
    CacheBlk first;
    CacheBlk second;
    CacheBlk rejected;
    first.insert(0x3000, false);
    second.insert(0x4000, false);
    rejected.insert(0x5000, false);
    const auto no_close = [](const Request::XsMetadata &) {
        ADD_FAILURE();
    };
    transitionBtbpL1iFillMetadata(
        ledger, {0x3000, false}, first, makeMetadata(1, false), false,
        false, no_close);
    transitionBtbpL1iFillMetadata(
        ledger, {0x4000, false}, second, makeMetadata(2, false), false,
        false, no_close);
    EXPECT_ANY_THROW(transitionBtbpL1iFillMetadata(
        ledger, {0x5000, false}, rejected, makeMetadata(3, false),
        false, false, no_close));
    EXPECT_EQ(ledger.size(), 2);
    EXPECT_TRUE(ledger.isCurrent({0x3000, false}, 1));
    EXPECT_TRUE(ledger.isCurrent({0x4000, false}, 2));
    EXPECT_EQ(rejected.getXsMetadata().l1iResidencyId, 0);
    EXPECT_TRUE(closeBtbpL1iBlockResidency(
        ledger, {0x3000, false}, first));
    EXPECT_TRUE(closeBtbpL1iBlockResidency(
        ledger, {0x4000, false}, second));
    first.invalidate();
    second.invalidate();
    rejected.invalidate();
}

TEST(BtbpL1iLifecycleTest, DrainClosesOnlyRoiOriginPrefetchResidencies)
{
    Ledger ledger;
    CacheBlk roi_prefetch;
    CacheBlk non_roi_prefetch;
    CacheBlk roi_demand;
    roi_prefetch.insert(0x1000, false);
    non_roi_prefetch.insert(0x2000, false);
    roi_demand.insert(0x3000, false);
    const auto no_close = [](const Request::XsMetadata &) {
        ADD_FAILURE();
    };
    transitionBtbpL1iFillMetadata(
        ledger, {0x1000, false}, roi_prefetch,
        makeMetadata(1, true, true), false, true, no_close);
    transitionBtbpL1iFillMetadata(
        ledger, {0x2000, false}, non_roi_prefetch,
        makeMetadata(2, true, false), false, true, no_close);
    transitionBtbpL1iFillMetadata(
        ledger, {0x3000, false}, roi_demand,
        makeMetadata(3, false, true), false, false, no_close);

    const auto drained = ledger.drainRoiPrefetch();
    ASSERT_EQ(drained.size(), 1);
    EXPECT_EQ(drained.front().second.id, 1);
    EXPECT_EQ(ledger.size(), 2);
    EXPECT_TRUE(closeBtbpL1iBlockResidency(
        ledger, {0x2000, false}, non_roi_prefetch));
    EXPECT_TRUE(closeBtbpL1iBlockResidency(
        ledger, {0x3000, false}, roi_demand));
    EXPECT_TRUE(ledger.drainRoiPrefetch().empty());
    roi_prefetch.invalidate();
    non_roi_prefetch.invalidate();
    roi_demand.invalidate();
}

} // namespace
} // namespace gem5
