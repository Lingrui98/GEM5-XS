#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "cpu/o3/entangling_prefetcher.hh"

namespace gem5
{

namespace o3
{

class EntanglingPrefetcherTestPeer
{
  public:
    static std::uint32_t
    format(std::uint64_t source, std::uint64_t destination)
    {
        return EntanglingPrefetcher::debugFormat(source, destination);
    }

    static std::uint64_t
    compress(std::uint64_t destination, std::uint32_t format)
    {
        return EntanglingPrefetcher::debugCompress(destination, format);
    }

    static std::uint64_t
    extend(
        std::uint64_t source, std::uint64_t destination,
        std::uint32_t format)
    {
        return EntanglingPrefetcher::debugExtend(
            source, destination, format);
    }

    static std::uint64_t
    hash(std::uint64_t line)
    {
        return EntanglingPrefetcher::debugHash(line);
    }

    static void
    addEntangled(
        EntanglingPrefetcher &prefetcher, std::uint64_t source,
        std::uint64_t destination)
    {
        prefetcher.debugAddEntangled(source, destination);
    }

    static void
    addBasicBlockSize(
        EntanglingPrefetcher &prefetcher, std::uint64_t line,
        std::uint32_t size)
    {
        prefetcher.debugAddBasicBlockSize(line, size);
    }

    static std::uint32_t
    confidence(
        const EntanglingPrefetcher &prefetcher, std::uint64_t source,
        std::uint64_t destination)
    {
        return prefetcher.debugConfidence(source, destination);
    }

    static std::uint32_t
    basicBlockSize(
        const EntanglingPrefetcher &prefetcher, std::uint64_t line)
    {
        return prefetcher.debugBasicBlockSize(line);
    }

    static std::uint32_t
    findHistory(
        const EntanglingPrefetcher &prefetcher, std::uint64_t line)
    {
        return prefetcher.debugFindHistory(line);
    }

    static std::uint32_t
    historyHead(const EntanglingPrefetcher &prefetcher)
    {
        return prefetcher.debugHistoryHead();
    }

    static std::uint64_t
    historyTag(
        const EntanglingPrefetcher &prefetcher, std::uint32_t index)
    {
        return prefetcher.debugHistoryTag(index);
    }

    static std::uint64_t
    historyTimeDiff(
        const EntanglingPrefetcher &prefetcher, std::uint32_t index)
    {
        return prefetcher.debugHistoryTimeDiff(index);
    }

    static std::uint64_t
    historyInstructionId(
        const EntanglingPrefetcher &prefetcher, std::uint32_t index)
    {
        return prefetcher.debugHistoryInstructionId(index);
    }

    static std::uint64_t
    historyEntangledSource(
        const EntanglingPrefetcher &prefetcher, std::uint32_t index)
    {
        return prefetcher.debugHistoryEntangledSource(index);
    }

    static std::uint32_t
    historyBasicBlockSize(
        const EntanglingPrefetcher &prefetcher, std::uint32_t index)
    {
        return prefetcher.debugHistoryBasicBlockSize(index);
    }

    static bool
    timingAccessed(
        const EntanglingPrefetcher &prefetcher, std::uint64_t line)
    {
        return prefetcher.debugTimingAccessed(line);
    }

    static std::uint32_t
    timingSourceWay(
        const EntanglingPrefetcher &prefetcher, std::uint64_t line)
    {
        return prefetcher.debugTimingSourceWay(line);
    }
};

namespace
{

using Version = EntanglingPrefetcher::AlgorithmVersion;

EntanglingPrefetcher::DemandAccess
access(
    std::uint64_t virtual_line, std::uint64_t cycle,
    std::uint64_t instruction_id, bool hit = true,
    std::uint64_t physical_line = 0)
{
    EntanglingPrefetcher::DemandAccess event;
    event.virtualLine = virtual_line;
    event.physicalLine = physical_line == 0 ?
        virtual_line + 0x10000 : physical_line;
    event.cycle = cycle;
    event.instructionId = instruction_id;
    event.cacheHit = hit;
    return event;
}

const EntanglingPrefetcher::AcceptCallback RejectAll =
    [](const auto &) { return false; };

} // anonymous namespace

TEST(EntanglingPrefetcherTest, FixedBundlesMatchOfficialSources)
{
    EntanglingPrefetcher tc(Version::Tc2024);
    const auto tc_bundle = tc.bundleInfo();
    EXPECT_EQ(tc_bundle.historyEntries, 32u);
    EXPECT_EQ(tc_bundle.historySourceSearches, 24u);
    EXPECT_EQ(tc_bundle.basicBlockMergeEntries, 3u);
    EXPECT_TRUE(tc_bundle.supportsWrongPathSquash);
    EXPECT_TRUE(tc_bundle.delaysTableInsertion);
    EXPECT_TRUE(tc_bundle.decrementsRedundantDestinations);

    EntanglingPrefetcher isca(Version::Isca2021);
    const auto isca_bundle = isca.bundleInfo();
    EXPECT_EQ(isca_bundle.historyEntries, 16u);
    EXPECT_EQ(isca_bundle.historySourceSearches, 16u);
    EXPECT_EQ(isca_bundle.basicBlockMergeEntries, 6u);
    EXPECT_FALSE(isca_bundle.supportsWrongPathSquash);
    EXPECT_FALSE(isca_bundle.delaysTableInsertion);
    EXPECT_FALSE(isca_bundle.decrementsRedundantDestinations);
}

TEST(EntanglingPrefetcherTest, CodecHashAndConfidenceMatchOfficialControl)
{
    constexpr std::uint64_t source = 0x123400;
    constexpr std::uint64_t destination = 0x12347f;
    EXPECT_EQ(EntanglingPrefetcherTestPeer::format(source, destination), 6u);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::compress(destination, 6), 0x7fULL);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::extend(source, 0x7f, 6),
        destination);

    constexpr std::uint64_t line = 0xdeadbeef;
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::hash(line),
        line ^ (line >> 2) ^ (line >> 5));

    EntanglingPrefetcher prefetcher(Version::Tc2024);
    EntanglingPrefetcherTestPeer::addEntangled(
        prefetcher, source, destination);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::confidence(
            prefetcher, source, destination),
        3u);

    std::vector<std::uint64_t> candidates;
    prefetcher.observeDemand(
        access(source, 1, 1),
        [&](const auto &candidate) {
            candidates.push_back(candidate.virtualLine);
            return true;
        });
    ASSERT_EQ(candidates, std::vector<std::uint64_t>{destination});
    EXPECT_LT(
        EntanglingPrefetcherTestPeer::timingSourceWay(
            prefetcher, destination),
        EntanglingPrefetcher::NoSourceWay);
    prefetcher.observeFill({destination, 0x9000, 2});
    EXPECT_TRUE(prefetcher.observeEvict({0x9000, 3}));
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::confidence(
            prefetcher, source, destination),
        2u);
}

TEST(EntanglingPrefetcherTest, TcSquashRepairsTimeAndDropsYoungerHistory)
{
    EntanglingPrefetcher prefetcher(Version::Tc2024);
    prefetcher.observeDemand(access(100, 10, 1), RejectAll);
    prefetcher.observeDemand(access(200, 20, 2), RejectAll);
    prefetcher.observeDemand(access(300, 30, 3), RejectAll);

    ASSERT_EQ(EntanglingPrefetcherTestPeer::findHistory(prefetcher, 200), 1u);
    ASSERT_EQ(EntanglingPrefetcherTestPeer::findHistory(prefetcher, 300), 2u);
    prefetcher.observeSquash({200, 2, 40});

    EXPECT_EQ(EntanglingPrefetcherTestPeer::historyHead(prefetcher), 2u);
    EXPECT_EQ(EntanglingPrefetcherTestPeer::findHistory(prefetcher, 300), 32u);
    EXPECT_EQ(EntanglingPrefetcherTestPeer::historyTag(prefetcher, 1), 200u);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::historyInstructionId(prefetcher, 1),
        2u);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::historyTimeDiff(prefetcher, 1), 20u);
    EXPECT_EQ(EntanglingPrefetcherTestPeer::historyTag(prefetcher, 2), 0u);
}

TEST(EntanglingPrefetcherTest, TcDelaysLearnedPairUntilHistoryRetirement)
{
    EntanglingPrefetcher prefetcher(Version::Tc2024);
    constexpr std::uint64_t source = 100;
    constexpr std::uint64_t destination = 200;

    prefetcher.observeDemand(access(source, 10, 1, false, 1100), RejectAll);
    EXPECT_TRUE(EntanglingPrefetcherTestPeer::timingAccessed(
        prefetcher, source));
    prefetcher.observeFill({source, 1100, 20});
    prefetcher.observeDemand(
        access(destination, 40, 2, false, 1200), RejectAll);
    prefetcher.observeFill({destination, 1200, 45});

    const auto destination_position =
        EntanglingPrefetcherTestPeer::findHistory(
            prefetcher, destination);
    ASSERT_LT(destination_position, 32u);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::historyEntangledSource(
            prefetcher, destination_position),
        source);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::confidence(
            prefetcher, source, destination),
        0u);

    std::vector<std::uint64_t> before_retirement;
    prefetcher.observeDemand(
        access(source, 50, 3, true, 1100),
        [&](const auto &candidate) {
            before_retirement.push_back(candidate.virtualLine);
            return false;
        });
    EXPECT_TRUE(before_retirement.empty());

    for (std::uint64_t i = 0; i < 32; ++i) {
        prefetcher.observeDemand(
            access(1000 + i * 2, 60 + i, 10 + i), RejectAll);
    }
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::confidence(
            prefetcher, source, destination),
        3u);
    EXPECT_TRUE(prefetcher.observeEvict({1200, 99}));

    std::vector<std::uint64_t> after_retirement;
    prefetcher.observeDemand(
        access(source, 100, 100, true, 1100),
        [&](const auto &candidate) {
            after_retirement.push_back(candidate.virtualLine);
            return false;
        });
    EXPECT_EQ(
        after_retirement, std::vector<std::uint64_t>{destination});
}

TEST(EntanglingPrefetcherTest, CandidateOrderAcceptanceAndCancellationAreExact)
{
    EntanglingPrefetcher prefetcher(Version::Tc2024);
    EntanglingPrefetcherTestPeer::addBasicBlockSize(prefetcher, 100, 2);
    EntanglingPrefetcherTestPeer::addEntangled(prefetcher, 100, 200);
    EntanglingPrefetcherTestPeer::addBasicBlockSize(prefetcher, 200, 1);

    std::vector<std::uint64_t> candidates;
    const auto result = prefetcher.observeDemand(
        access(100, 1, 1),
        [&](const auto &candidate) {
            candidates.push_back(candidate.virtualLine);
            return candidates.size() == 1;
        });
    EXPECT_EQ(
        candidates,
        (std::vector<std::uint64_t>{101, 102, 200, 201}));
    EXPECT_EQ(result.candidatesOffered, 4u);
    EXPECT_EQ(result.candidatesAccepted, 1u);
    EXPECT_EQ(prefetcher.inflightRequests(), 1u);

    const auto repeated = prefetcher.observeDemand(
        access(100, 2, 2),
        [&](const auto &) {
            ADD_FAILURE() << "same-line access retried a rejected candidate";
            return false;
        });
    EXPECT_TRUE(repeated.repeatedCurrentLine);
    EXPECT_EQ(repeated.candidatesOffered, 0u);

    EXPECT_TRUE(prefetcher.cancelAcceptedRequest(101));
    EXPECT_FALSE(prefetcher.cancelAcceptedRequest(101));
    EXPECT_EQ(prefetcher.inflightRequests(), 0u);
    EXPECT_EQ(prefetcher.stats().acceptedRequestsCanceled, 1u);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::confidence(prefetcher, 100, 200),
        3u);
}

TEST(EntanglingPrefetcherTest, OngoingDestinationCoalescesAndTracksSource)
{
    EntanglingPrefetcher prefetcher(Version::Tc2024);
    constexpr std::uint64_t destination = 300;
    EntanglingPrefetcherTestPeer::addEntangled(prefetcher, 100, destination);
    EntanglingPrefetcherTestPeer::addEntangled(prefetcher, 200, destination);

    std::uint32_t callback_count = 0;
    prefetcher.observeDemand(
        access(100, 1, 1),
        [&](const auto &) {
            ++callback_count;
            return true;
        });
    EXPECT_EQ(callback_count, 1u);
    EXPECT_EQ(prefetcher.inflightRequests(), 1u);

    const auto coalesced = prefetcher.observeDemand(
        access(200, 2, 2),
        [&](const auto &) {
            ++callback_count;
            return true;
        });
    EXPECT_EQ(coalesced.candidatesOffered, 0u);
    EXPECT_EQ(callback_count, 1u);
    EXPECT_EQ(prefetcher.inflightRequests(), 1u);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::confidence(
            prefetcher, 200, destination),
        2u);
}

TEST(EntanglingPrefetcherTest, ResidentCandidateDoesNotConsumeAcceptance)
{
    EntanglingPrefetcher prefetcher(Version::Tc2024);
    EntanglingPrefetcherTestPeer::addBasicBlockSize(prefetcher, 100, 2);
    prefetcher.observeFill({101, 0x10101, 0});

    EXPECT_TRUE(prefetcher.hasResidentVirtualLine(101));
    std::vector<std::uint64_t> callbacks;
    const auto result = prefetcher.observeDemand(
        access(100, 1, 1),
        [&](const auto &candidate) {
            callbacks.push_back(candidate.virtualLine);
            return true;
        });

    EXPECT_EQ(callbacks, (std::vector<std::uint64_t>{102}));
    EXPECT_EQ(result.candidatesOffered, 2u);
    EXPECT_EQ(result.candidatesAccepted, 1u);
    EXPECT_EQ(prefetcher.inflightRequests(), 1u);
    EXPECT_TRUE(prefetcher.hasResidentVirtualLine(101));
}

TEST(EntanglingPrefetcherTest, PhysicalResidencyOverwriteRetiresOldState)
{
    EntanglingPrefetcher prefetcher(Version::Tc2024);
    constexpr std::uint64_t source = 100;
    constexpr std::uint64_t destination = 200;
    constexpr std::uint64_t replacement = 300;
    constexpr std::uint64_t physical_line = 0x9000;

    EntanglingPrefetcherTestPeer::addEntangled(
        prefetcher, source, destination);
    prefetcher.observeDemand(
        access(source, 1, 1), [](const auto &) { return true; });
    prefetcher.observeFill({destination, physical_line, 2});
    ASSERT_TRUE(prefetcher.hasResidentVirtualLine(destination));
    ASSERT_EQ(
        EntanglingPrefetcherTestPeer::confidence(
            prefetcher, source, destination),
        3u);

    prefetcher.observeFill({replacement, physical_line, 3});

    EXPECT_FALSE(prefetcher.hasResidentVirtualLine(destination));
    EXPECT_TRUE(prefetcher.hasResidentVirtualLine(replacement));
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::confidence(
            prefetcher, source, destination),
        2u);
    EXPECT_EQ(prefetcher.stats().wrongPrefetches, 1u);
    EXPECT_EQ(prefetcher.stats().overwrittenResidencies, 1u);
}

TEST(EntanglingPrefetcherTest, PrefetchHitMarksResidencyUsed)
{
    EntanglingPrefetcher prefetcher(Version::Tc2024);
    constexpr std::uint64_t virtual_line = 200;
    constexpr std::uint64_t physical_line = 0x9000;

    prefetcher.observeFill({virtual_line, physical_line, 1});
    auto demand = access(
        virtual_line, 2, 1, true, physical_line);
    demand.prefetchHit = true;
    prefetcher.observeDemand(demand, RejectAll);

    EXPECT_EQ(prefetcher.stats().prefetchHits, 1u);
    EXPECT_TRUE(prefetcher.observeEvict({physical_line, 3}));
    EXPECT_EQ(prefetcher.stats().wrongPrefetches, 0u);
}

TEST(EntanglingPrefetcherTest, IscaKeepsImmediateAndLatePoliciesSeparate)
{
    EntanglingPrefetcher tc(Version::Tc2024);
    EntanglingPrefetcher isca(Version::Isca2021);
    for (auto *prefetcher : {&tc, &isca}) {
        prefetcher->observeDemand(access(100, 1, 1), RejectAll);
        prefetcher->observeDemand(access(101, 2, 2), RejectAll);
        prefetcher->observeDemand(access(102, 3, 3), RejectAll);
        prefetcher->observeDemand(access(200, 4, 4), RejectAll);
    }
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::basicBlockSize(tc, 100), 0u);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::basicBlockSize(isca, 100), 2u);

    const auto isca_before_squash = isca.stateDigest();
    isca.observeSquash({100, 1, 10});
    EXPECT_EQ(isca.stateDigest(), isca_before_squash);

    EntanglingPrefetcherTestPeer::addEntangled(tc, 300, 500);
    EntanglingPrefetcherTestPeer::addEntangled(isca, 300, 500);
    tc.observeDemand(access(300, 11, 11), [](const auto &) { return true; });
    isca.observeDemand(
        access(300, 11, 11), [](const auto &) { return true; });
    tc.observeDemand(access(500, 12, 12, false), RejectAll);
    isca.observeDemand(access(500, 12, 12, false), RejectAll);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::confidence(tc, 300, 500), 3u);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::confidence(isca, 300, 500), 2u);
    EXPECT_EQ(
        EntanglingPrefetcherTestPeer::timingSourceWay(isca, 500),
        EntanglingPrefetcher::NoSourceWay);
}

TEST(EntanglingPrefetcherTest, StateDigestIsDeterministic)
{
    EntanglingPrefetcher first(Version::Tc2024);
    EntanglingPrefetcher second(Version::Tc2024);
    for (auto *prefetcher : {&first, &second}) {
        EntanglingPrefetcherTestPeer::addEntangled(*prefetcher, 100, 200);
        prefetcher->observeDemand(
            access(100, 10, 1), [](const auto &) { return true; });
        prefetcher->observeDemand(access(200, 12, 2, false), RejectAll);
        prefetcher->observeFill({200, 9000, 20});
    }
    EXPECT_EQ(first.stateDigest(), second.stateDigest());
    EXPECT_NE(first.stateDigest(), 0u);
}

TEST(EntanglingPrefetcherTest, TcTransitionDigestMatchesOfficialSource)
{
    EntanglingPrefetcher prefetcher(Version::Tc2024);
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 17374552389484749603ULL);

    prefetcher.observeDemand(access(100, 10, 1, false, 100), RejectAll);
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 7870650028586887165ULL);
    prefetcher.observeFill({100, 100, 20});
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 16444222798330634007ULL);

    prefetcher.observeDemand(access(200, 40, 2, false, 200), RejectAll);
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 4200706649854397183ULL);
    prefetcher.observeFill({200, 200, 45});
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 7156193498429642455ULL);

    constexpr std::array<std::uint64_t, 32> retirement_digests{
        3755737265854177099ULL, 17264317500287668545ULL,
        18299267753083498870ULL, 14504190406976663430ULL,
        5370574934426429751ULL, 5977256089680407585ULL,
        11618927389476460010ULL, 6763763854455211694ULL,
        16852479195788915139ULL, 9405358883308146753ULL,
        10023601021195477438ULL, 5642506958193516054ULL,
        18174627961290929098ULL, 15541419628774301884ULL,
        16775101182381510547ULL, 1274553468715450335ULL,
        11570745364389577058ULL, 4716744075581282072ULL,
        5322079642919986899ULL, 1602948816018158947ULL,
        10588889759524025578ULL, 16658304910920680884ULL,
        14543293200417515539ULL, 16231305903603321943ULL,
        16550825385878619010ULL, 12568185768868111024ULL,
        13720006739019256451ULL, 78781844427603931ULL,
        4223707155610092266ULL, 8833703443882272908ULL,
        1201266318425476347ULL, 15055015301238263868ULL,
    };
    for (std::uint64_t i = 0; i < retirement_digests.size(); ++i) {
        SCOPED_TRACE(i);
        prefetcher.observeDemand(
            access(1000 + i * 2, 60 + i, 10 + i, false, 1000 + i * 2),
            RejectAll);
        EXPECT_EQ(
            prefetcher.algorithmStateDigest(), retirement_digests[i]);
    }

    prefetcher.observeDemand(access(400, 100, 50, false, 400), RejectAll);
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 12291625417327029925ULL);
    EXPECT_TRUE(prefetcher.observeEvict({200, 110}));
    prefetcher.observeFill({400, 400, 110});
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 6034433905362481002ULL);

    prefetcher.observeDemand(
        access(100, 120, 51, true, 100),
        [](const auto &) { return true; });
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 2748989931347012892ULL);
    prefetcher.observeFill({200, 200, 130});
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 12559151475085203732ULL);
    auto prefetch_hit = access(200, 140, 52, true, 200);
    prefetch_hit.prefetchHit = true;
    prefetcher.observeDemand(prefetch_hit, RejectAll);
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 6945484096052229216ULL);

    prefetcher.observeDemand(access(500, 150, 53, false, 500), RejectAll);
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 6664568469996159825ULL);
    EXPECT_TRUE(prefetcher.observeEvict({200, 160}));
    prefetcher.observeFill({500, 500, 160});
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 9028088886662984371ULL);

    constexpr std::uint64_t overflow_cycle =
        (std::uint64_t{1} << 20) + 200;
    prefetcher.observeDemand(
        access(600, overflow_cycle, 60, false, 600), RejectAll);
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 5408044522460731624ULL);
    prefetcher.observeDemand(
        access(700, overflow_cycle + 20, 62, false, 700), RejectAll);
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 18261071276354669841ULL);
    prefetcher.observeDemand(
        access(800, overflow_cycle + 30, 63, false, 800), RejectAll);
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 11254527131648869662ULL);
    prefetcher.observeSquash({700, 62, overflow_cycle + 40});
    EXPECT_EQ(prefetcher.algorithmStateDigest(), 6617206351880814840ULL);
}

} // namespace o3
} // namespace gem5
