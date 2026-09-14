#include <gtest/gtest.h>

#include "mem/cache/mshr_queue.hh"

namespace gem5
{
namespace
{

TEST(MSHRPartitionPolicyTest, EnforcesFourDemandAndTenInstPrefetchEntries)
{
    constexpr unsigned demand_limit = 4;
    constexpr unsigned inst_prefetch_limit = 10;
    MSHRPartitionOccupancy occupancy{3, 9};

    EXPECT_TRUE(MSHRPartitionPolicy::canAllocate(
        MSHR::AllocationOwner::Demand, occupancy, demand_limit,
        inst_prefetch_limit));
    EXPECT_TRUE(MSHRPartitionPolicy::canAllocate(
        MSHR::AllocationOwner::InstPrefetch, occupancy, demand_limit,
        inst_prefetch_limit));

    occupancy.demandOwned = demand_limit;
    EXPECT_FALSE(MSHRPartitionPolicy::canAllocate(
        MSHR::AllocationOwner::Demand, occupancy, demand_limit,
        inst_prefetch_limit));
    EXPECT_TRUE(MSHRPartitionPolicy::canAllocate(
        MSHR::AllocationOwner::InstPrefetch, occupancy, demand_limit,
        inst_prefetch_limit));

    occupancy.demandOwned = demand_limit - 1;
    occupancy.instPrefetchOwned = inst_prefetch_limit;
    EXPECT_TRUE(MSHRPartitionPolicy::canAllocate(
        MSHR::AllocationOwner::Demand, occupancy, demand_limit,
        inst_prefetch_limit));
    EXPECT_FALSE(MSHRPartitionPolicy::canAllocate(
        MSHR::AllocationOwner::InstPrefetch, occupancy, demand_limit,
        inst_prefetch_limit));
}

TEST(MSHRPartitionPolicyTest, MergePreservesAllocationOwner)
{
    EXPECT_EQ(MSHR::allocationOwnerAfterMerge(
                  MSHR::AllocationOwner::InstPrefetch,
                  MSHR::AllocationOwner::Demand),
              MSHR::AllocationOwner::InstPrefetch);
    EXPECT_EQ(MSHR::allocationOwnerAfterMerge(
                  MSHR::AllocationOwner::Demand,
                  MSHR::AllocationOwner::InstPrefetch),
              MSHR::AllocationOwner::Demand);
}

TEST(MSHRPartitionPolicyTest, ZeroLimitDisablesThatPartition)
{
    const MSHRPartitionOccupancy occupancy{100, 100};

    EXPECT_TRUE(MSHRPartitionPolicy::canAllocate(
        MSHR::AllocationOwner::Demand, occupancy, 0, 10));
    EXPECT_TRUE(MSHRPartitionPolicy::canAllocate(
        MSHR::AllocationOwner::InstPrefetch, occupancy, 4, 0));
    EXPECT_FALSE(MSHRPartitionPolicy::canAllocate(
        MSHR::AllocationOwner::Unallocated, occupancy, 0, 0));
}

} // namespace
} // namespace gem5
