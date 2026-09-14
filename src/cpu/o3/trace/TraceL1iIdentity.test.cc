#include <gtest/gtest.h>

#include "cpu/o3/trace/TraceL1iIdentity.hh"

namespace gem5
{
namespace o3
{
namespace
{

TEST(TraceL1iIdentityTest, RetryKeepsLogicalDemandAndClosesOnce)
{
    TraceL1iDemandIdentity demand{17, 29};

    EXPECT_EQ(demand.beginAttempt(), 0u);
    EXPECT_EQ(demand.beginAttempt(), 1u);
    EXPECT_EQ(demand.requestUid, 17u);
    EXPECT_EQ(demand.demandUid, 29u);
    EXPECT_TRUE(demand.close());
    EXPECT_FALSE(demand.close());
    EXPECT_THROW(demand.beginAttempt(), std::logic_error);
}

TEST(TraceL1iIdentityTest, SquashReplayAllocatesFreshIdentity)
{
    TraceL1iIdentityGenerator generator;
    const auto before_lookup = generator.allocateLookup(0);
    const auto before_request = generator.allocateRequest(0);
    generator.advanceEpoch(0);
    const auto replay_lookup = generator.allocateLookup(0);
    const auto replay_request = generator.allocateRequest(0);

    EXPECT_NE(before_request.requestUid, replay_request.requestUid);
    EXPECT_NE(before_lookup.lookupUid, replay_lookup.lookupUid);
    EXPECT_GT(replay_request.fetchEpoch, before_request.fetchEpoch);
    EXPECT_EQ(replay_lookup.fetchEpoch, replay_request.fetchEpoch);
}

TEST(TraceL1iIdentityTest, DiagnosticFtqReuseCannotAliasIdentity)
{
    TraceL1iIdentityGenerator generator;
    constexpr uint64_t reusedFtqId = 42;
    const auto first_lookup = generator.allocateLookup(0);
    const auto first_request = generator.allocateRequest(0);
    generator.advanceEpoch(0);
    const auto second_lookup = generator.allocateLookup(0);
    const auto second_request = generator.allocateRequest(0);

    EXPECT_EQ(reusedFtqId, 42u);
    EXPECT_NE(first_request.requestUid, second_request.requestUid);
    EXPECT_NE(first_lookup.lookupUid, second_lookup.lookupUid);
}

} // namespace
} // namespace o3
} // namespace gem5
