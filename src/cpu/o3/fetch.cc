/*
 * Copyright (c) 2010-2014 ARM Limited
 * Copyright (c) 2012-2013 AMD
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/o3/fetch.hh"

#include <algorithm>
#include <cstring>
#include <list>
#include <map>
#include <queue>

#include "arch/generic/tlb.hh"
#include "arch/riscv/decoder.hh"
#include "arch/riscv/pcstate.hh"
#include "base/debug_helper.hh"
#include "base/random.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/base.hh"
#include "mem/cache/base.hh"
#include "cpu/exetrace.hh"
#include "cpu/nop_static_inst.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/topdown_stats_helpers.hh"
#include "cpu/o3/trace/TraceFetch.hh"
#include "cpu/o3/trace/TraceL1iIdentity.hh"
#include "cpu/pred/btb/decoupled_bpred.hh"
#include "cpu/pred/btb/probe/btbp_trace_field_helpers.hh"
#include "cpu/valuepred/example_value_predictor_metadata.hh"
#include "debug/Activity.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/FetchFault.hh"
#include "debug/FetchVerbose.hh"
#include "debug/O3CPU.hh"
#include "debug/O3PipeView.hh"
#include "debug/TraceReader.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "sim/full_system.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

Fetch::IcachePort::IcachePort(Fetch *_fetch, CPU *_cpu) :
        RequestPort(_cpu->name() + ".icache_port"), fetch(_fetch)
{}


Fetch::Fetch(CPU *_cpu, const BaseO3CPUParams &params)
    : fetchPolicy(params.smtFetchPolicy),
      numPreDispatchThreads(params.smtNumPreDispatchThreads),
      smtDecodePolicy(params.smtDecodePolicy),
      smtBorrowThrottleHoldCycles(params.smtBorrowThrottleCycles),
      delayedSchedulerDelay(params.smtFetchDelayedSchedulerDelay),
      smtFetchBlockPolicy(params.smtFetchBlockPolicy),
      longLatencyThreshold(params.smtFetchBlockThreshold),
      cpu(_cpu),
      branchPred(nullptr),
      dbpbtb(nullptr),
      resolveQueueSize(params.resolveQueueSize),
      decodeToFetchDelay(params.decodeToFetchDelay),
      renameToFetchDelay(params.renameToFetchDelay),
      iewToFetchDelay(params.iewToFetchDelay),
      commitToFetchDelay(params.commitToFetchDelay),
      fetchWidth(params.fetchWidth),
      enableTwoFetch(params.enableTwoFetch),
      twoFetchMaxBytes(params.twoFetchMaxBytes),
      decodeWidth(params.decodeWidth),
      retryPkt(),
      cacheBlkSize(cpu->cacheLineSize()),
      fetchBufferSize(params.fetchBufferSize),
      fetchQueueSize(params.fetchQueueSize),
      numThreads(params.numThreads),
      numFetchingThreads(params.smtNumFetchingThreads),
      numFetchTargetThreads(params.smtNumFetchTargetThreads),
      enablePredecode(params.enablePredecode),
      icachePort(this, _cpu),
      finishTranslationEvents(), fetchStats(_cpu, this),
      valuePred(params.valuePred)
{
    if (numThreads > MaxThreads)
        fatal("numThreads (%d) is larger than compiled limit (%d),\n"
              "\tincrease MaxThreads in src/cpu/o3/limits.hh\n",
              numThreads, static_cast<int>(MaxThreads));
    if (fetchWidth > MaxWidth)
        fatal("fetchWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             fetchWidth, static_cast<int>(MaxWidth));
    panic_if(enableTwoFetch &&
             (twoFetchMaxBytes == 0 ||
              fetchBufferSize < 2 ||
              twoFetchMaxBytes > fetchBufferSize - 2),
             "twoFetchMaxBytes (%u) requires a fetch buffer of at least "
             "%u bytes, but fetchBufferSize is %u",
             twoFetchMaxBytes, twoFetchMaxBytes + 2, fetchBufferSize);
    panic_if(numFetchTargetThreads == 0 ||
             numFetchTargetThreads > numThreads ||
             numFetchTargetThreads > 2,
             "smtNumFetchTargetThreads (%u) must be in [1, min(2, "
             "numThreads (%u))]",
             numFetchTargetThreads, numThreads);
    panic_if(numFetchTargetThreads > 1 && numFetchingThreads > 1,
             "smtNumFetchTargetThreads and smtNumFetchingThreads cannot both "
             "exceed one because fetch() would be invoked multiple times");
    panic_if(numPreDispatchThreads == 0 ||
             numPreDispatchThreads > numThreads ||
             numPreDispatchThreads > 2,
             "smtNumPreDispatchThreads (%u) must be in [1, min(2, "
             "numThreads (%u))]",
             numPreDispatchThreads, numThreads);
    panic_if(decodeWidth * numPreDispatchThreads > MaxWidth,
             "aggregate SMT decode width (%u * %u) exceeds MaxWidth (%u)",
             decodeWidth, numPreDispatchThreads, MaxWidth);

    finishTranslationEvents.reserve(numThreads);
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        finishTranslationEvents.emplace_back(
            std::make_unique<FinishTranslationEvent>(this));
    }

    // IEW reports an early redirect before the formal Commit squash reaches
    // Fetch:
    //   T0                 IEW detects a wrong-path condition
    //   T0 + iewToFetch    Fetch sees redirectPending
    //   T0 + commitToFetch Commit squash reaches Fetch
    // Hold the hint only across that gap so SMT arbitration can avoid the
    // doomed thread without turning the hint into a sticky recovery state.
    const auto redirect_pending_gap =
        commitToFetchDelay > iewToFetchDelay ?
        static_cast<unsigned>(commitToFetchDelay - iewToFetchDelay) : 0;
    redirectPendingHoldCycles = redirect_pending_gap + 1;
    for (int i = 0; i < MaxThreads; i++) {
        fetchStatus[i] = Idle;
        setThreadStatus(i, Idle);
        decoder[i] = nullptr;
        threads[i].fetchpc.reset(params.isa[0]->newPCState());
        macroop[i] = nullptr;
        delayedCommit[i] = false;
        redirectPending[i] = false;
        redirectPendingCycles[i] = 0;
        lastIcacheStall[i] = 0;
        smtBorrowThrottleCycles[i] = 0;
        threadFetchBlocked[i] = false;
        blockStateHoldCycles[i] = 0;
        longLatencyStallCycles[i] = 0;
        lastLoadHeadSeqNum[i] = UINT64_MAX;
        deferVsetvlDecode[i] = false;
        postSquashFetchBatchSize[i] = 0;
        flushFromInitiated[i] = false;

    }
    smtLdstqHighWater = params.smtBorrowLdstqHighWater;
    if (smtLdstqHighWater == 0) {
        smtLdstqHighWater =
            (params.LQEntries +
             params.SQEntries * params.StoreQueueMultiple) *
            params.smtBorrowLdstqHighWaterPercent / 100;
    }

    branchPred = params.branchPred;

    // This fetch implementation only supports the decoupled frontend with the
    // decoupled BTB predictor. Fail fast to avoid silently using legacy paths.
    assert(branchPred);
    assert(branchPred->isDecoupled());
    assert(branchPred->isBTB());

    dbpbtb =
        dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB*>(
            branchPred);
    assert(dbpbtb);
    dbpbtb->setCpu(_cpu);
    fdipIcacheAccessor = params.fdipIcacheAccessor;
    eipIssueBandwidth = params.eipIssueBandwidth;
    eipMaxOutstanding = params.eipMaxOutstanding;

    if (params.eipAlgorithm != "disabled") {
        fatal_if(dbpbtb->fdipEnabled(),
            "FDIP and EIP cannot be enabled together");
        fatal_if(numThreads != 1,
            "The source-faithful EIP port supports one hardware thread");
        fatal_if(cacheBlkSize != 64,
            "The source-faithful EIP port requires 64-byte cache lines");
        fatal_if(eipIssueBandwidth != 1 || eipMaxOutstanding != 10,
            "Stage-A EIP requires issue bandwidth 1 and max outstanding 10");

        const auto version = params.eipAlgorithm == "tc24" ?
            EntanglingPrefetcher::AlgorithmVersion::Tc2024 :
            EntanglingPrefetcher::AlgorithmVersion::Isca2021;
        fatal_if(params.eipAlgorithm != "tc24" &&
                 params.eipAlgorithm != "isca21",
            "Unknown EIP algorithm bundle '%s'", params.eipAlgorithm.c_str());
        eipPrefetcher = std::make_unique<EntanglingPrefetcher>(version);
    }

    assert(params.decoder.size());
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        decoder[tid] = params.decoder[tid];
        // Set the size and allocate data for each fetch buffer instance
        threads[tid].size = fetchBufferSize;
        threads[tid].data = new uint8_t[fetchBufferSize];
    }

    initDecodeScheduler();

    // Get the size of an instruction.
    // stallReason size should be the same as decodeWidth,renameWidth,dispWidth
    stallReason.resize(decodeWidth, StallReason::NoStall);

    traceFetch = std::make_unique<TraceFetch>(*this, params);

    if (isTraceMode() && traceFetch && !traceFetch->allowDecoupledFrontend()) {
        fatal("Trace mode requires allowDecoupledFrontend=true for decoupled+BTB-only fetch\n");
    }
}

Fetch::~Fetch() = default;

Fetch::PredecodeEntry::PredecodeEntry() = default;
Fetch::PredecodeEntry::~PredecodeEntry() = default;
Fetch::PredecodeStage::~PredecodeStage() = default;

void
Fetch::PredecodeStage::clear()
{
    for (unsigned i = 0; i < size; ++i)
        entries[i].instruction = nullptr;
    size = 0;
}

void
Fetch::clearRedirectPending(ThreadID tid)
{
    redirectPending[tid] = false;
    redirectPendingCycles[tid] = 0;
    if (dbpbtb) {
        dbpbtb->setRedirectPending(tid, false);
    }
}

bool
Fetch::isTraceMode() const
{
    return traceFetch && traceFetch->enabled();
}

bool
Fetch::isTraceEOF() const
{
    return traceFetch && traceFetch->isEOF();
}

uint64_t
Fetch::getTraceReaderIndex() const
{
    return traceFetch ? traceFetch->getTraceReaderIndex() : 0;
}

std::string Fetch::name() const { return cpu->name() + ".fetch"; }

void
Fetch::regProbePoints()
{
    ppFetch = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(), "Fetch");
    ppFetchRequestSent = new ProbePointArg<RequestPtr>(cpu->getProbeManager(),
                                                       "FetchRequest");

}

Fetch::FetchStatGroup::FetchStatGroup(CPU *cpu, Fetch *fetch)
    : statistics::Group(cpu, "fetch"),
      cpu(cpu),
      fetch(fetch),
    ADD_STAT(icacheStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch is stalled on an Icache miss"),
    ADD_STAT(insts, statistics::units::Count::get(),
             "Number of instructions fetch has processed"),
    ADD_STAT(branches, statistics::units::Count::get(),
             "Number of branches that fetch encountered"),
    ADD_STAT(predictedBranches, statistics::units::Count::get(),
             "Number of branches that fetch has predicted taken"),
    ADD_STAT(predecodeRedirects, statistics::units::Count::get(),
             "Redirects issued by predecode"),
    ADD_STAT(cycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has run and was not squashing or "
             "blocked"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent squashing"),
    ADD_STAT(tlbCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting for tlb"),
    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch was idle"),
    ADD_STAT(smtidleCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch was idle per tid"),         
    ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked"),
    ADD_STAT(smtblockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked per tid"),         
    ADD_STAT(miscStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on interrupts, or bad "
             "addresses, or out of MSHRs"),
    ADD_STAT(pendingDrainCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on pipes to drain"),
    ADD_STAT(noActiveThreadStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to no active thread to fetch from"),
    ADD_STAT(pendingTrapStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending traps"),
    ADD_STAT(pendingQuiesceStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending quiesce instructions"),
    ADD_STAT(icacheWaitRetryStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to full MSHR"),
    ADD_STAT(cacheLines, statistics::units::Count::get(),
             "Number of cache lines fetched"),
    ADD_STAT(icacheSquashes, statistics::units::Count::get(),
             "Number of outstanding Icache misses that were squashed"),
    ADD_STAT(tlbSquashes, statistics::units::Count::get(),
             "Number of outstanding ITLB misses that were squashed"),
    ADD_STAT(nisnDist, statistics::units::Count::get(),
             "Number of instructions fetched each cycle (Total)"),
    ADD_STAT(decodeThreadsPerCycle, statistics::units::Count::get(),
             "Distinct SMT threads sent to Decode in one cycle"),
    ADD_STAT(instsSentToDecodePerCycle, statistics::units::Count::get(),
             "Instructions sent from fetch queues to Decode in one cycle"),
    ADD_STAT(idleRate, statistics::units::Ratio::get(),
             "Ratio of cycles fetch was idle",
             idleCycles / cpu->baseStats.numCycles),
    ADD_STAT(branchRate, statistics::units::Ratio::get(),
             "Number of branch fetches per cycle",
             branches / cpu->baseStats.numCycles),
    ADD_STAT(rate, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Number of inst fetches per cycle",
             insts / cpu->baseStats.numCycles),
    ADD_STAT(fetchStatusDist, statistics::units::Count::get(),
             "Distribution of fetch status"),
    ADD_STAT(decodeStalls, statistics::units::Count::get(),
             "Number of decode stalls"),
    ADD_STAT(smtdecodeStalls, statistics::units::Count::get(),
             "Number of decode stalls per tid"),  
    ADD_STAT(smtftqempty, statistics::units::Count::get(),
             "Number of ftq empty per tid"),                  
    ADD_STAT(decodeStallRate, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Number of decode stalls per cycle",
             decodeStalls / cpu->baseStats.numCycles),
    ADD_STAT(fetchBubbles, statistics::units::Count::get(),
             "Unutilized issue-pipeline slots while there is no backend-stall"),
    ADD_STAT(fetchBubbles_max, statistics::units::Count::get(),
             "Cycles that fetch 0 instruction while there is no backend-stall"),
    ADD_STAT(frontendBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Frontend Bound",
             fetchBubbles / (cpu->baseStats.numCycles * fetch->decodeWidth)),
    ADD_STAT(frontendLatencyBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Frontend Latency Bound",
             fetchBubbles_max * fetch->decodeWidth /
                 (cpu->baseStats.numCycles * fetch->decodeWidth)),
    ADD_STAT(frontendBandwidthBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Frontend Bandwidth Bound",
             frontendBound - frontendLatencyBound),
    ADD_STAT(btbMissResteers, statistics::units::Count::get(),
             "Decode redirects caused by a prediction-time BTB miss "
             "(fetch-time snapshot predBtbHit false: the branch PC hit "
             "no predBTBEntry of the supplying FetchTarget)"),
    ADD_STAT(btbMissResteerCycles, statistics::units::Cycle::get(),
             "Cycles from fetch consuming a BTB-miss decode redirect "
             "(decodeToFetchDelay = 1 after decode's selfSquash) to the "
             "first instruction delivered to decode for that thread.  The "
             "defined end point is the first DELIVERY (not queue refill): "
             "cycles where decode is backend-blocked while the queue is "
             "non-empty can extend an open window — documented "
             "approximation of this diagnostic proxy"),
    ADD_STAT(gem5BranchResteers, statistics::units::Ratio::get(),
             "gem5 proxy metric: btbMissResteerCycles / numCycles.  This "
             "is ONLY the BTB-miss subset of decode redirects as seen by "
             "this frontend; it is NOT the full Intel TMA Branch_Resteers",
             btbMissResteerCycles / cpu->baseStats.numCycles),
    ADD_STAT(frontendRecoveryCycles, statistics::units::Cycle::get(),
             "Cycles from fetch consuming a commit squash (recovery "
             "window start; commitToFetchDelay = 2 cycles after commit "
             "initiated it) to the first instruction delivered to decode "
             "for that thread"),
    ADD_STAT(frontendRecoverySlots, statistics::units::Count::get(),
             "Notional full-width decode slots lost inside frontend "
             "recovery windows (frontendRecoveryCycles x decodeWidth).  "
             "Raw component only: may overlap fetchBubbles on post-squash "
             "cycles and must never be summed into a composite numerator"),
    ADD_STAT(fetchQueueSquashedInsts, statistics::units::Count::get(),
             "Instructions dropped when a squash clears the fetch queue "
             "(Fetch::doSquash; the resetStage/clearStates clears are not "
             "squashes and are not counted).  Neutral naming: includes "
             "non-misprediction squash sources (trap/tc/squashAfter)"),
    ADD_STAT(resolveQueueFullEvents, statistics::units::Count::get(),
             "Number of events the resolve queue becomes full"),
    ADD_STAT(resolveEnqueueFailEvent, statistics::units::Count::get(),
             "Number of times an entry could not be enqueued to the resolve queue"),
    ADD_STAT(resolveSquashedEvents, statistics::units::Count::get(),
             "Number of resolved events discarded because of a squash"),
    ADD_STAT(resolveDequeueCount, statistics::units::Count::get(),
             "Number of times an entry is dequeued from the resolve queue"),
    ADD_STAT(resolveDequeueEventCount, statistics::units::Count::get(),
             "Number of individual resolved events consumed from the resolve queue"),
    ADD_STAT(resolveEnqueueCount, statistics::units::Count::get(),
             "Number of times an entry is enqueued to the resolve queue"),
    ADD_STAT(resolveQueueOccupancy, statistics::units::Count::get(),
             "Number of entries in the resolve queue"),
    ADD_STAT(redirectPendingFetchSkips, statistics::units::Count::get(),
             "Number of FTQ heads skipped because IEW reported a pending redirect"),
    ADD_STAT(redirectPendingOnlyFetchCycles, statistics::units::Count::get(),
             "Number of fetch attempts blocked because all FTQ heads were redirect-pending"),
    ADD_STAT(fetchTargetsStartedPerCycle, statistics::units::Count::get(),
             "Number of distinct SMT thread FTQ fetches started in one cycle"),
    ADD_STAT(fetchTargetsStartedByThread, statistics::units::Count::get(),
             "Number of FTQ fetches started for each SMT thread"),
    ADD_STAT(fetchLineRequestsCreatedPerCycle, statistics::units::Count::get(),
             "Number of cache-line requests created by FTQ fetches in one cycle"),
    ADD_STAT(fetchTargetThreadNotReady, statistics::units::Count::get(),
             "Selected FTQ heads whose thread state could not start a fetch"),
    ADD_STAT(fetchTargetRequestBlocked, statistics::units::Count::get(),
             "Prepared FTQ heads whose cache request could not be started"),
    ADD_STAT(twoFetchAttempts, statistics::units::Count::get(),
             "Predicted-taken FTQ boundaries considered for limited two-fetch"),
    ADD_STAT(twoFetchSuccesses, statistics::units::Count::get(),
             "FTQ boundaries crossed using the current fetch buffer"),
    ADD_STAT(traceMetaStores, statistics::units::Count::get(),
             "Number of stored trace metadata records (seqNum -> traceInst)"),
    ADD_STAT(traceMetaCleanupSquashCalls, statistics::units::Count::get(),
             "Number of times cleanup was called due to squash/rollback"),
    ADD_STAT(traceMetaCleanupSquashEntries, statistics::units::Count::get(),
             "Total entries erased by squash/rollback cleanups"),
    ADD_STAT(traceMetaCleanupCommitCalls, statistics::units::Count::get(),
             "Number of times cleanup was called on successful commit"),
    ADD_STAT(fdipIssuedLines, statistics::units::Count::get(),
             "Number of FDIP cacheline prefetches successfully sent"),
    ADD_STAT(fdipDropped, statistics::units::Count::get(),
             "Number of FDIP best-effort issue attempts that were dropped"),
    ADD_STAT(fdipFilteredFault, statistics::units::Count::get(),
             "Number of FDIP cachelines filtered by translation faults"),
    ADD_STAT(fdipFilteredUncacheable, statistics::units::Count::get(),
             "Number of FDIP cachelines filtered by uncacheable regions"),
    ADD_STAT(fdipFilteredRecentUnused, statistics::units::Count::get(),
             "Number of FDIP cachelines filtered by recent-unused suppression"),
    ADD_STAT(fdipCandidateLines, statistics::units::Count::get(),
             "Number of FDIP candidate line touches from predictor targets"),
    ADD_STAT(fdipUniqueCandidateLines, statistics::units::Count::get(),
             "Number of unique FDIP candidate lines seen across the run"),
    ADD_STAT(fdipRepeatedCandidateLines, statistics::units::Count::get(),
             "Number of repeated FDIP candidate line touches"),
    ADD_STAT(fdipDirectProbeHit, statistics::units::Count::get(),
             "Number of FDIP translated lines that hit via direct ICache probe"),
    ADD_STAT(fdipUniqueIssuedLines, statistics::units::Count::get(),
             "Number of unique physical lines issued by FDIP"),
    ADD_STAT(fdipRepeatedIssuedLines, statistics::units::Count::get(),
             "Number of repeated FDIP issued-line touches"),
    ADD_STAT(fdipWrongPathIssuedLines, statistics::units::Count::get(),
             "Number of FDIP issued lines that later belonged to squashed FTQs"),
    ADD_STAT(fdipWrongPathUniqueIssuedLines, statistics::units::Count::get(),
             "Number of unique physical lines later attributed to wrong-path FDIP"),
    ADD_STAT(fdipWrongPathDemandAccesses, statistics::units::Count::get(),
             "Number of demand fetch line accesses to previously squashed FDIP lines"),
    ADD_STAT(fdipWrongPathDemandReusedLines, statistics::units::Count::get(),
             "Number of unique squashed FDIP lines later revisited by demand"),
    ADD_STAT(fdipWayHintsInstalled, statistics::units::Count::get(),
             "Number of FDIP selected-way hints installed by direct probe hit"),
    ADD_STAT(fdipWayHintsConsumed, statistics::units::Count::get(),
             "Number of FDIP selected-way hints consumed by demand fetch"),
    ADD_STAT(fdipOutstandingMax, statistics::units::Count::get(),
             "Peak number of outstanding FDIP cacheline requests"),
    ADD_STAT(fdipEpochMismatch, statistics::units::Count::get(),
             "Number of stale FDIP translation/response events ignored"),
    ADD_STAT(instPrefetchDecisionsAccepted,
             statistics::units::Count::get(),
             "Instruction-prefetch decisions accepted at the common gate"),
    ADD_STAT(instPrefetchGateAttempts,
             statistics::units::Count::get(),
             "Instruction-prefetch common-gate attempts"),
    ADD_STAT(instPrefetchDecisionBandwidthRejects,
             statistics::units::Count::get(),
             "Instruction-prefetch decisions rejected by cycle bandwidth"),
    ADD_STAT(instPrefetchDecisionOutstandingRejects,
             statistics::units::Count::get(),
             "Instruction-prefetch decisions rejected by outstanding limit"),
    ADD_STAT(instPrefetchDecisionDrainRejects,
             statistics::units::Count::get(),
             "Instruction-prefetch decisions rejected during ROI drain"),
    ADD_STAT(instPrefetchOutstandingMax, statistics::units::Count::get(),
             "Peak accepted-but-incomplete instruction-prefetch requests"),
    ADD_STAT(eipDemandAccesses, statistics::units::Count::get(),
             "Demand tag checks observed by EIP"),
    ADD_STAT(eipCandidatesOffered, statistics::units::Count::get(),
             "EIP candidates offered by the paper-native core"),
    ADD_STAT(eipCandidatesAccepted, statistics::units::Count::get(),
             "EIP candidates accepted at the common gate"),
    ADD_STAT(eipFilteredFault, statistics::units::Count::get(),
             "EIP translations rejected by faults"),
    ADD_STAT(eipFilteredUncacheable, statistics::units::Count::get(),
             "EIP translations rejected as uncacheable"),
    ADD_STAT(eipIssuedLines, statistics::units::Count::get(),
             "EIP requests sent to the L1I port"),
    ADD_STAT(eipBackpressureEvents, statistics::units::Count::get(),
             "EIP send attempts blocked by L1I backpressure"),
    ADD_STAT(eipCompletedLines, statistics::units::Count::get(),
             "EIP request completions returned to Fetch"),
    ADD_STAT(eipFillEvents, statistics::units::Count::get(),
             "Real L1I fills observed by EIP"),
    ADD_STAT(eipEvictionEvents, statistics::units::Count::get(),
             "Real L1I evictions observed by EIP"),
    ADD_STAT(eipSquashEvents, statistics::units::Count::get(),
             "Wrong-path squash events observed by EIP"),
    ADD_STAT(fetchBlockState, statistics::units::Count::get(),
             "Block policy thread state combination per cycle "
             "(0=both unblocked, 1=tid0 blocked, 2=tid1 blocked, 3=both blocked)"),
    ADD_STAT(fetchThrottleState, statistics::units::Count::get(),
             "Decode policy thread state combination per cycle, Th means "
             "uncandidated or throttled. (0=both not Th, 1=tid0 Th, 2=tid1 Th, 3=both Th)"),
    ADD_STAT(fetchBlockHoldCycle, statistics::units::Count::get(),
             "Per-thread block/unblock state holding cycle distribution"),
    ADD_STAT(flushForFlushPolicy, statistics::units::Count::get(),
             "Number of long-latency load flush events per thread"),
    ADD_STAT(flushFromFirstUseFound, statistics::units::Count::get(),
             "FlushFromUse: first-use consumer found, squash initiated"),
    ADD_STAT(flushFromFirstUseNoConsumer, statistics::units::Count::get(),
             "FlushFromUse: no consumer found, squash from ROB tail")
{
        icacheStallCycles
            .prereq(icacheStallCycles);
        insts
            .prereq(insts);
        branches
            .prereq(branches);
        predictedBranches
            .prereq(predictedBranches);
        cycles
            .prereq(cycles);
        squashCycles
            .prereq(squashCycles);
        tlbCycles
            .prereq(tlbCycles);
        idleCycles
            .prereq(idleCycles);
        blockedCycles
            .prereq(blockedCycles);
        cacheLines
            .prereq(cacheLines);
        miscStallCycles
            .prereq(miscStallCycles);
        pendingDrainCycles
            .prereq(pendingDrainCycles);
        noActiveThreadStallCycles
            .prereq(noActiveThreadStallCycles);
        pendingTrapStallCycles
            .prereq(pendingTrapStallCycles);
        pendingQuiesceStallCycles
            .prereq(pendingQuiesceStallCycles);
        icacheWaitRetryStallCycles
            .prereq(icacheWaitRetryStallCycles);
        icacheSquashes
            .prereq(icacheSquashes);
        tlbSquashes
            .prereq(tlbSquashes);
        nisnDist
            .init(/* base value */ 0,
              /* last value */ fetch->fetchWidth,
              /* bucket size */ 1)
            .flags(statistics::pdf);
        decodeThreadsPerCycle
            .init(0, fetch->numPreDispatchThreads, 1)
            .flags(statistics::pdf);
        instsSentToDecodePerCycle
            .init(0, fetch->decodeWidth * fetch->numPreDispatchThreads, 1)
            .flags(statistics::pdf);
        idleRate
            .prereq(idleRate);
        branchRate
            .flags(statistics::total);
        rate
            .flags(statistics::total);
        fetchStatusDist
            .init(NumFetchStatus)
            .flags(statistics::pdf | statistics::total);

        for (int i = 0; i < NumFetchStatus; i++) {
            fetchStatusDist.subname(i, fetch->fetchStatusStr[static_cast<Fetch::ThreadStatus>(i)]);
        }
        decodeStalls
            .prereq(decodeStalls);
        smtdecodeStalls
            .init(fetch->numThreads)
            .flags(statistics::total);  
        smtftqempty
            .init(fetch->numThreads)
            .flags(statistics::total);
        smtidleCycles
            .init(fetch->numThreads)
            .flags(statistics::total);
        smtblockedCycles
            .init(fetch->numThreads)
            .flags(statistics::total);     
        fetchTargetsStartedPerCycle
            .init(0, fetch->numThreads, 1);
        fetchTargetsStartedByThread
            .init(fetch->numThreads)
            .flags(statistics::total);
        fetchLineRequestsCreatedPerCycle
            .init(0, 2 * fetch->numThreads, 1);
        decodeStallRate
            .flags(statistics::total);
        fetchBubbles
            .prereq(fetchBubbles);
        fetchBubbles_max
            .prereq(fetchBubbles_max);
        frontendBound
            .flags(statistics::total);
        frontendLatencyBound
            .flags(statistics::total);
        frontendBandwidthBound
            .flags(statistics::total);
        resolveEnqueueCount
            .init(1, 8, 1);
        resolveQueueOccupancy
            .init(0, 32, 1);
        redirectPendingFetchSkips
            .prereq(redirectPendingFetchSkips);
        redirectPendingOnlyFetchCycles
            .prereq(redirectPendingOnlyFetchCycles);
        traceMetaStores
            .prereq(traceMetaStores);
        traceMetaCleanupSquashCalls
            .prereq(traceMetaCleanupSquashCalls);
        traceMetaCleanupSquashEntries
            .prereq(traceMetaCleanupSquashEntries);
        traceMetaCleanupCommitCalls
            .prereq(traceMetaCleanupCommitCalls);
        fdipIssuedLines
            .prereq(fdipIssuedLines);
        fdipDropped
            .prereq(fdipDropped);
        fdipFilteredFault
            .prereq(fdipFilteredFault);
        fdipFilteredUncacheable
            .prereq(fdipFilteredUncacheable);
        fdipFilteredRecentUnused
            .prereq(fdipFilteredRecentUnused);
        fdipCandidateLines
            .prereq(fdipCandidateLines);
        fdipUniqueCandidateLines
            .prereq(fdipUniqueCandidateLines);
        fdipRepeatedCandidateLines
            .prereq(fdipRepeatedCandidateLines);
        fdipDirectProbeHit
            .prereq(fdipDirectProbeHit);
        fdipUniqueIssuedLines
            .prereq(fdipUniqueIssuedLines);
        fdipRepeatedIssuedLines
            .prereq(fdipRepeatedIssuedLines);
        fdipWrongPathIssuedLines
            .prereq(fdipWrongPathIssuedLines);
        fdipWrongPathUniqueIssuedLines
            .prereq(fdipWrongPathUniqueIssuedLines);
        fdipWrongPathDemandAccesses
            .prereq(fdipWrongPathDemandAccesses);
        fdipWrongPathDemandReusedLines
            .prereq(fdipWrongPathDemandReusedLines);
        fdipWayHintsInstalled
            .prereq(fdipWayHintsInstalled);
        fdipWayHintsConsumed
            .prereq(fdipWayHintsConsumed);
        fdipOutstandingMax
            .prereq(fdipOutstandingMax);
        fdipEpochMismatch
            .prereq(fdipEpochMismatch);
        instPrefetchDecisionsAccepted
            .prereq(instPrefetchDecisionsAccepted);
        instPrefetchGateAttempts.flags(statistics::nonan);
        instPrefetchDecisionBandwidthRejects.flags(statistics::nonan);
        instPrefetchDecisionOutstandingRejects.flags(statistics::nonan);
        instPrefetchDecisionDrainRejects.flags(statistics::nonan);
        instPrefetchOutstandingMax
            .prereq(instPrefetchOutstandingMax);
        eipDemandAccesses
            .prereq(eipDemandAccesses);
        eipCandidatesOffered
            .prereq(eipCandidatesOffered);
        eipCandidatesAccepted
            .prereq(eipCandidatesAccepted);
        eipFilteredFault
            .prereq(eipFilteredFault);
        eipFilteredUncacheable
            .prereq(eipFilteredUncacheable);
        eipIssuedLines
            .prereq(eipIssuedLines);
        eipBackpressureEvents
            .prereq(eipBackpressureEvents);
        eipCompletedLines
            .prereq(eipCompletedLines);
        eipFillEvents
            .prereq(eipFillEvents);
        eipEvictionEvents
            .prereq(eipEvictionEvents);
        eipSquashEvents
            .prereq(eipSquashEvents);
        fetchBlockState
            .init(1 << cpu->numThreads)
            .flags(statistics::total);
        fetchBlockState.subname(0, "BothUnBlocked");
        fetchBlockState.subname(1, "Tid0Blocked");
        fetchBlockState.subname(2, "Tid1Blocked");
        fetchBlockState.subname(3, "BothBlocked");
        fetchThrottleState
            .init(1 << cpu->numThreads)
            .flags(statistics::total);
        fetchThrottleState.subname(0, "BothUnThrottled");
        fetchThrottleState.subname(1, "Tid0Throttled");
        fetchThrottleState.subname(2, "Tid1Throttled");
        fetchThrottleState.subname(3, "BothThrottled");
        fetchBlockHoldCycle
            .init(2, 0, 999, 100)
            .flags(statistics::pdf);
        fetchBlockHoldCycle.subname(0, "Unblocked");
        fetchBlockHoldCycle.subname(1, "Blocked");
        flushForFlushPolicy
            .init(cpu->numThreads)
            .flags(statistics::total);
        flushForFlushPolicy
            .prereq(flushForFlushPolicy);
        flushFromFirstUseNoConsumer
            .prereq(flushFromFirstUseNoConsumer);
}

void
Fetch::setIEWStage(IEW *iew_stage) {
    iewStage = iew_stage;
}

// Window machines: the interval/overlap/discard rules live in the
// production helper (tdstats::RedirectWindow) that the unit tests link
// directly; these adapters bind them to the stats storages.

void
Fetch::FetchStatGroup::closeBtbMissWindow(ThreadID tid)
{
    // Half-open interval [start, curCycle): the opening cycle itself is
    // an empty-delivery cycle for this tid (the squash cleared the fetch
    // queue before sendInstructionsToDecode in the same tick), the
    // closing cycle delivered >= 1 instruction and is not charged.
    const uint64_t elapsed = btbMissWindow[tid].close(cpu->curCycle());
    if (elapsed > 0) {
        btbMissResteerCycles += elapsed;
    }
}

void
Fetch::FetchStatGroup::openBtbMissWindow(ThreadID tid)
{
    // Overlap rule (in the helper: close-then-open): a new redirect
    // closes any still-open window by its elapsed cycles first (the
    // charge is returned and bound here exactly like the old explicit
    // closeBtbMissWindow call), then opens a fresh one — windows never
    // double charge the same cycle.
    btbMissResteerCycles += btbMissWindow[tid].open(cpu->curCycle());
}

void
Fetch::FetchStatGroup::closeRecoveryWindow(ThreadID tid)
{
    // Same half-open interval semantics as the R3 window.
    const uint64_t elapsed = recoveryWindow[tid].close(cpu->curCycle());
    if (elapsed > 0) {
        frontendRecoveryCycles += elapsed;
        frontendRecoverySlots += elapsed * fetch->decodeWidth;
    }
}

void
Fetch::FetchStatGroup::openRecoveryWindow(ThreadID tid)
{
    // Overlap rule (same as R3, in the helper): the embedded close's
    // charge is returned and bound here exactly like the old explicit
    // closeRecoveryWindow call.
    const uint64_t elapsed = recoveryWindow[tid].open(cpu->curCycle());
    frontendRecoveryCycles += elapsed;
    frontendRecoverySlots += elapsed * fetch->decodeWidth;
}

void
Fetch::FetchStatGroup::resetStats()
{
    // Default behaviour first: wipe the stat storages.
    statistics::Group::resetStats();

    // D4-A ROI boundary: the stats reset (warmupInstCount / nextDumpInst-
    // Count dump+reset in CPU::instDone) only clears the statistics — this
    // custom window state machine survives it.  Truncate any open window
    // WITHOUT charging: a window open at the boundary belongs (by charge-
    // at-close) to the segment that was just dumped without ever being
    // billed there, so discarding it bills those cycles to neither segment
    // and no window is ever billed across the ROI boundary.  Adopted
    // semantics: only COMPLETE windows (closed inside a segment) are
    // charged; empty cycles after the boundary and before the next
    // delivery/redirect/squash event are attributed to no window by
    // design (the contract forbids billing any window across segments).
    for (auto &w : btbMissWindow) {
        w.discard();
    }
    for (auto &w : recoveryWindow) {
        w.discard();
    }
}

void
Fetch::setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer)
{
    timeBuffer = time_buffer;

    // Create wires to get information from proper places in time buffer.
    fromDecode = timeBuffer->getWire(-decodeToFetchDelay);
    fromRename = timeBuffer->getWire(-renameToFetchDelay);
    fromIEW = timeBuffer->getWire(-iewToFetchDelay);
    fromCommit = timeBuffer->getWire(-commitToFetchDelay);
}

void
Fetch::initDecodeScheduler()
{
     // Initialize counters (same as before)
    lsqCounter = new InstsCounter();
    iqCounter  = new InstsCounter();
    robCounter = new InstsCounter();
    DPRINTF(Fetch, "Initialized SMT Decode Scheduler: 0\n");

    for (ThreadID tid = 0; tid < numThreads; tid++) 
    {
        lsqCounter->setCounter(tid, 0);
        iqCounter->setCounter(tid, 0);
        robCounter->setCounter(tid, 0);
    }
    DPRINTF(Fetch, "Initialized SMT Decode Scheduler: 1\n");
    
    if (smtDecodePolicy == SMTDecodePolicy::ICount) {
        // Use ROB as default counter for icount
        decodeScheduler = new ICountScheduler(numThreads, robCounter);
    }
    else if (smtDecodePolicy == SMTDecodePolicy::DelayedICount) {
        decodeScheduler = new DelayedICountScheduler(numThreads, robCounter, delayedSchedulerDelay);
    }
    else if (smtDecodePolicy == SMTDecodePolicy::MultiPriority) {
        decodeScheduler = new MultiPrioritySched(numThreads, {lsqCounter, iqCounter, robCounter});
    }
    else if (smtDecodePolicy == SMTDecodePolicy::RoundRobin) {
        decodeScheduler = new RoundRobinScheduler(numThreads, robCounter);
    } else {
        panic("undifined smtDecodePolicy:%d\n", (int)smtDecodePolicy);
    }

    DPRINTF(Fetch, "Initialized SMT Decode Scheduler: %d\n", (int)smtDecodePolicy);
}

void
Fetch::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
Fetch::setFetchQueue(TimeBuffer<FetchStruct> *ftb_ptr)
{
    // Create wire to write information to proper place in fetch time buf.
    toDecode = ftb_ptr->getWire(0);

    // initialize to toDecode stall vector
    toDecode->fetchStallReason = stallReason;
}

void
Fetch::startupStage()
{
    assert(priorityList.empty());
    resetStage();

    // Fetch needs to start fetching instructions at the very beginning,
    // so it must start up in active state.
    switchToActive();

    if (isTraceMode() && !traceFetch->initTraceMode()) {
        fatal("Failed to initialize trace mode\n");
    }
}

void
Fetch::clearStates(ThreadID tid)
{
    clearResolveQueue(tid);
    setThreadStatus(tid, Running);
    set(threads[tid].fetchpc, cpu->pcState(tid));
    macroop[tid] = NULL;
    delayedCommit[tid] = false;
    closeOutstandingTraceFetch(
        tid, branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason::
                 ResetCanceled);
    clearRedirectPending(tid);
    threads[tid].cacheReq.reset();
    threads[tid].reset();
    pendingTraceInstructionOrdinal[tid] = 0;
    pendingTraceInstructionPc[tid] = 0;
    pendingTracePathWrong[tid] = false;
    pendingTraceSupplyValid[tid] = false;
    resetFdipPartialState(tid);
    resetFdipTracking(tid);
    if (eipEnabled()) {
        cancelAllEipRequests();
    }
    fdipEpoch[tid] = 0;
    fetchQueue[tid].clear();
    deferVsetvlDecode[tid] = false;
    postSquashFetchBatchSize[tid] = 0;
    clearPredecodePipeline(tid);


    // TODO not sure what to do with priorityList for now
    // priorityList.push_back(tid);
}

void
Fetch::resetStage()
{
    numInst = 0;
    interruptPending = false;
    for (auto *pkt : retryPkt) {
        delete pkt;
    }
    retryPkt.clear();
    cancelAllEipRequests();
    for (const auto &pending : fdipPendingReqs) {
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::
                IPrefetchTerminal,
            pending.tid, pending.req,
            branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason::
                ResetCanceled);
    }
    fdipPendingReqs.clear();
    instPrefetchOutstandingLines = 0;
    instPrefetchDecisionTick = MaxTick;
    instPrefetchDecisionsThisTick = 0;
    cacheBlocked = false;

    priorityList.clear();

    // Setup PC and nextPC with initial state.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        clearResolveQueue(tid);
        setThreadStatus(tid, Running);
        set(threads[tid].fetchpc, cpu->pcState(tid));
        macroop[tid] = NULL;

        delayedCommit[tid] = false;
        closeOutstandingTraceFetch(
            tid, branch_prediction::btb_pred::BtbpTraceEvent::
                     TerminalReason::ResetCanceled);
        clearRedirectPending(tid);
        threads[tid].cacheReq.reset();

        threads[tid].reset();
        pendingTraceInstructionOrdinal[tid] = 0;
        pendingTraceInstructionPc[tid] = 0;
        pendingTracePathWrong[tid] = false;
        pendingTraceSupplyValid[tid] = false;
        resetFdipPartialState(tid);
        resetFdipTracking(tid);
        fdipEpoch[tid] = 0;
        ftqEntryFetchedInsts[tid] = 0;

        fetchQueue[tid].clear();
        clearPredecodePipeline(tid);

        priorityList.push_back(tid);
        deferVsetvlDecode[tid] = false;
        postSquashFetchBatchSize[tid] = 0;
        waitForVsetvl[tid] = false;
        smtBorrowThrottleCycles[tid] = 0;
    }

    wroteToTimeBuffer = false;
    _status = Inactive;

    if (traceFetch) {
        traceFetch->resetStage();
    }

    assert(dbpbtb);
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        dbpbtb->resetPC(tid, threads[tid].fetchpc->instAddr());
    }
}

void
Fetch::resetFdipPartialState(ThreadID tid)
{
    for (const auto &pending : fdipPendingReqs) {
        if (pending.tid == tid) {
            emitInstPrefetchTrace(
                branch_prediction::btb_pred::BtbpTraceEvent::
                    IPrefetchTerminal,
                tid, pending.req,
                branch_prediction::btb_pred::BtbpTraceEvent::
                    TerminalReason::ResetCanceled);
        }
    }
    const auto summary = cleanupFdipPartialState(
        tid, fdipState[tid], fdipPendingReqs, fdipProbeHints[tid],
        instPrefetchOutstandingLines);
    instPrefetchOutstandingLines = summary.outstandingLines;
}

void
Fetch::resetFdipState(ThreadID tid)
{
    fdipState[tid].reset();

    for (auto it = fdipPendingReqs.begin(); it != fdipPendingReqs.end();) {
        if (it->tid == tid) {
            emitInstPrefetchTrace(
                branch_prediction::btb_pred::BtbpTraceEvent::
                    IPrefetchTerminal,
                tid, it->req,
                branch_prediction::btb_pred::BtbpTraceEvent::
                    TerminalReason::ResetCanceled);
            if (it->outstanding) {
                assert(instPrefetchOutstandingLines > 0);
                --instPrefetchOutstandingLines;
            }
            it = fdipPendingReqs.erase(it);
        } else {
            ++it;
        }
    }
}

void
Fetch::resetFdipProbeHints(ThreadID tid)
{
    fdipProbeHints[tid].clear();
}

void
Fetch::resetFdipTracking(ThreadID tid)
{
    fdipTrackedTargets[tid].clear();
    fdipCandidateLineSeen[tid].clear();
    fdipIssuedLineSeen[tid].clear();
    fdipWrongPathIssuedLineSeen[tid].clear();
    fdipWrongPathDemandReuseLineSeen[tid].clear();
}

bool
Fetch::reserveInstPrefetchDecision(
    unsigned maxOutstanding, unsigned issueBandwidth, uint64_t &attemptId,
    uint64_t &decisionId,
    branch_prediction::btb_pred::BtbpTraceEvent::GateOutcome &outcome)
{
    using GateOutcome = branch_prediction::btb_pred::BtbpTraceEvent::GateOutcome;

    attemptId = nextInstPrefetchAttemptId++;
    ++fetchStats.instPrefetchGateAttempts;
    if (btbpRoiDrainMode) {
        ++fetchStats.instPrefetchDecisionDrainRejects;
        outcome = GateOutcome::DrainRejected;
        return false;
    }

    if (maxOutstanding == 0 ||
        instPrefetchOutstandingLines >= maxOutstanding) {
        ++fetchStats.instPrefetchDecisionOutstandingRejects;
        outcome = GateOutcome::OutstandingRejected;
        return false;
    }

    const Tick now = curTick();
    if (instPrefetchDecisionTick != now) {
        instPrefetchDecisionTick = now;
        instPrefetchDecisionsThisTick = 0;
    }
    if (issueBandwidth == 0 ||
        instPrefetchDecisionsThisTick >= issueBandwidth) {
        ++fetchStats.instPrefetchDecisionBandwidthRejects;
        outcome = GateOutcome::BandwidthRejected;
        return false;
    }

    decisionId = nextInstPrefetchDecisionId++;
    ++instPrefetchDecisionsThisTick;
    ++instPrefetchOutstandingLines;
    ++fetchStats.instPrefetchDecisionsAccepted;
    outcome = GateOutcome::Accepted;
    if (instPrefetchOutstandingLines >
        fetchStats.instPrefetchOutstandingMax.value()) {
        fetchStats.instPrefetchOutstandingMax = instPrefetchOutstandingLines;
    }
    return true;
}

void
Fetch::emitInstPrefetchGateAttempt(
    ThreadID tid, PrefetchSourceType source, Addr virtual_line_addr,
    Addr trigger_pc, const Request::XsMetadata &trace_meta,
    uint64_t attempt_id, uint64_t decision_id,
    branch_prediction::btb_pred::BtbpTraceEvent::GateOutcome outcome)
{
    using Event = branch_prediction::btb_pred::BtbpTraceEvent;

    Event event;
    event.tick = curTick();
    event.eventType = Event::IPrefetchGateAttempt;
    event.threadId = tid;
    event.virtualLineAddr = virtual_line_addr;
    event.virtualLineAddrValid = true;
    event.triggerPc = trigger_pc;
    event.triggerPcValid = true;
    event.lineSize = cacheBlkSize;
    event.lineSizeValid = true;
    event.requestKind = Event::PrefetchRequest;
    event.requestKindValid = true;
    event.prefetchSource = static_cast<uint32_t>(source);
    event.prefetchSourceValid = true;
    event.prefetchAttemptId = attempt_id;
    event.prefetchAttemptIdValid = true;
    event.gateOutcome = static_cast<uint32_t>(outcome);
    event.gateOutcomeValid = true;
    if (decision_id != 0) {
        event.prefetchDecisionId = decision_id;
        event.prefetchDecisionIdValid = true;
    }
    if (trace_meta.traceIdentityValid) {
        event.addressSpaceId = trace_meta.traceAddressSpaceId;
        event.addressSpaceIdValid = true;
        event.asidHash = trace_meta.traceAsidHash;
        event.asidHashValid = true;
        event.ftqId = trace_meta.traceFtqId;
        event.ftqIdValid = true;
    }
    cpu->notifyBtbpTrace(event);
}

void
Fetch::releaseInstPrefetchDecision()
{
    assert(instPrefetchOutstandingLines > 0);
    --instPrefetchOutstandingLines;
}

void
Fetch::emitInstPrefetchTrace(
    branch_prediction::btb_pred::BtbpTraceEvent::EventType event_type,
    ThreadID tid, const RequestPtr &req,
    branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason reason)
{
    using Event = branch_prediction::btb_pred::BtbpTraceEvent;

    if (!req || !req->hasXsMetadata()) {
        return;
    }

    const auto xs_meta = req->getXsMetadata();
    Event event;
    event.tick = curTick();
    event.eventType = event_type;
    event.threadId = tid;
    event.lineSize = cacheBlkSize;
    event.lineSizeValid = true;
    event.requestKind = Event::PrefetchRequest;
    event.requestKindValid = true;

    if (req->hasVaddr()) {
        event.virtualLineAddr = req->getVaddr() -
            req->getVaddr() % cacheBlkSize;
        event.virtualLineAddrValid = true;
    }
    if (req->hasPaddr()) {
        event.lineAddr = req->getPaddr() -
            req->getPaddr() % cacheBlkSize;
        event.lineAddrValid = true;
    }
    if (xs_meta.instPrefetchDecisionId != 0) {
        event.prefetchDecisionId = xs_meta.instPrefetchDecisionId;
        event.prefetchDecisionIdValid = true;
    }
    if (xs_meta.instPrefetchAttemptId != 0) {
        event.prefetchAttemptId = xs_meta.instPrefetchAttemptId;
        event.prefetchAttemptIdValid = true;
    }
    if (xs_meta.isInstPrefetch()) {
        event.prefetchSource =
            static_cast<uint32_t>(xs_meta.prefetchSource);
        event.prefetchSourceValid = true;
    }
    if (xs_meta.traceIdentityValid) {
        event.addressSpaceId = xs_meta.traceAddressSpaceId;
        event.addressSpaceIdValid = true;
        event.asidHash = xs_meta.traceAsidHash;
        event.asidHashValid = true;
        event.ftqId = xs_meta.traceFtqId;
        event.ftqIdValid = true;
    }
    if (xs_meta.isFdip()) {
        event.fdipEpoch = xs_meta.fdipEpoch;
        event.fdipEpochValid = true;
        event.triggerPc = xs_meta.fdipStartPC;
        event.triggerPcValid = true;
    } else if (xs_meta.isEip()) {
        event.triggerPc = xs_meta.eipTriggerPC;
        event.triggerPcValid = true;
    }
    if (event_type == Event::IPrefetchTerminal) {
        event.terminalReason = static_cast<uint32_t>(reason);
        event.terminalReasonValid = true;
    }

    cpu->notifyBtbpTrace(event);
}

void
Fetch::emitTraceFetchRequest(
    branch_prediction::btb_pred::BtbpTraceEvent::EventType event_type,
    ThreadID tid,
    branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason reason)
{
    using Event = branch_prediction::btb_pred::BtbpTraceEvent;

    auto &cache_req = threads[tid].cacheReq;
    if (!isTraceMode() || cache_req.traceRequestUid == 0) {
        return;
    }

    Event event;
    event.tick = curTick();
    event.eventType = event_type;
    event.threadId = tid;
    event.requestUid = cache_req.traceRequestUid;
    event.requestUidValid = true;
    event.lookupUid = cache_req.traceLookupUid;
    event.lookupUidValid = cache_req.traceLookupUid != 0;
    event.fetchEpoch = cache_req.traceFetchEpoch;
    event.fetchEpochValid = cache_req.traceFetchEpoch != 0;
    event.ftqId = cache_req.traceFtqId;
    event.ftqIdValid = true;
    event.traceInstructionOrdinal = cache_req.traceInstructionOrdinal;
    event.traceInstructionOrdinalValid =
        cache_req.traceInstructionOrdinal != 0;
    event.virtualLineAddr = cache_req.baseAddr -
        cache_req.baseAddr % cacheBlkSize;
    event.virtualLineAddrValid = true;
    event.lineSize = cacheBlkSize;
    event.lineSizeValid = true;
    event.requestKind = Event::DemandRequest;
    event.requestKindValid = true;
    event.pathState = cache_req.tracePathWrong ?
        Event::WrongPath : Event::CorrectPath;
    event.pathStateValid = true;
    event.addressSpaceId = cache_req.traceAddressSpaceId;
    event.addressSpaceIdValid = true;
    event.asidHash = cache_req.traceAsidHash;
    event.asidHashValid = true;

    if (!cache_req.requests.empty() &&
        cache_req.requests.front()->hasXsMetadata()) {
        const auto meta = cache_req.requests.front()->getXsMetadata();
        event.addressSpaceId = meta.traceAddressSpaceId;
        event.addressSpaceIdValid = meta.traceIdentityValid;
        event.asidHash = meta.traceAsidHash;
        event.asidHashValid = meta.traceIdentityValid;
    }
    if (event_type == Event::FetchRequestTerminal) {
        event.terminalReason = static_cast<uint32_t>(reason);
        event.terminalReasonValid = true;
    }
    cpu->notifyBtbpTrace(event);
}

void
Fetch::emitTraceDemandEvent(
    branch_prediction::btb_pred::BtbpTraceEvent::EventType event_type,
    ThreadID tid, const RequestPtr &req,
    branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason reason)
{
    using Event = branch_prediction::btb_pred::BtbpTraceEvent;

    if (!isTraceMode() || !req || !req->hasXsMetadata()) {
        return;
    }
    const auto meta = req->getXsMetadata();
    if (meta.traceRequestUid == 0 || meta.eipDemandId == 0) {
        return;
    }

    Event event;
    event.tick = curTick();
    event.eventType = event_type;
    event.threadId = tid;
    event.requestKind = Event::DemandRequest;
    event.requestKindValid = true;
    event.requestUid = meta.traceRequestUid;
    event.requestUidValid = true;
    event.lookupUid = meta.traceLookupUid;
    event.lookupUidValid = meta.traceLookupUid != 0;
    event.fetchEpoch = meta.traceFetchEpoch;
    event.fetchEpochValid = meta.traceFetchEpoch != 0;
    event.demandUid = meta.eipDemandId;
    event.demandUidValid = true;
    event.ftqId = meta.traceFtqId;
    event.ftqIdValid = meta.traceIdentityValid;
    event.addressSpaceId = meta.traceAddressSpaceId;
    event.addressSpaceIdValid = meta.traceIdentityValid;
    event.asidHash = meta.traceAsidHash;
    event.asidHashValid = meta.traceIdentityValid;
    event.traceInstructionOrdinal = meta.traceInstructionOrdinal;
    event.traceInstructionOrdinalValid =
        meta.traceInstructionOrdinal != 0;
    event.pathState = meta.tracePathWrong ?
        Event::WrongPath : Event::CorrectPath;
    event.pathStateValid = true;
    event.lineSize = cacheBlkSize;
    event.lineSizeValid = true;
    if (req->hasVaddr()) {
        event.virtualLineAddr = req->getVaddr() -
            req->getVaddr() % cacheBlkSize;
        event.virtualLineAddrValid = true;
    }
    if (req->hasPaddr()) {
        event.lineAddr = req->getPaddr() - req->getPaddr() % cacheBlkSize;
        event.lineAddrValid = true;
    } else if (event.virtualLineAddrValid) {
        event.lineAddr = event.virtualLineAddr;
        event.lineAddrValid = true;
    }
    if (event_type == Event::L1IDemandAttempt &&
        meta.traceDemandAttemptOrdinal != 0) {
        event.demandAttemptOrdinal =
            meta.traceDemandAttemptOrdinal - 1;
        event.demandAttemptOrdinalValid = true;
    }
    if (event_type == Event::L1IDemandTerminal) {
        event.terminalReason = static_cast<uint32_t>(reason);
        event.terminalReasonValid = true;
        switch (reason) {
          case Event::TerminalReason::CacheHit:
            event.supplySource = Event::CacheHitSupply;
            event.supplySourceValid = true;
            break;
          case Event::TerminalReason::MergeWithPrefetch:
            event.supplySource = Event::PrefetchFillSupply;
            event.supplySourceValid = true;
            break;
          case Event::TerminalReason::Fill:
          case Event::TerminalReason::MergeWithDemand:
            event.supplySource = Event::DemandFillSupply;
            event.supplySourceValid = true;
            break;
          default:
            break;
        }
    }
    if (meta.traceL1iVisibilityTick != 0) {
        event.visibilityTick = meta.traceL1iVisibilityTick;
        event.visibilityTickValid = true;
    }
    if (meta.l1iResidencyId != 0) {
        event.residencyId = meta.l1iResidencyId;
        event.residencyIdValid = true;
    }
    if (meta.instPrefetchDecisionId != 0) {
        event.prefetchDecisionId = meta.instPrefetchDecisionId;
        event.prefetchDecisionIdValid = true;
        event.ownerPrefetchDecisionIds.push_back(
            meta.instPrefetchDecisionId);
    }
    cpu->notifyBtbpTrace(event);
}

uint32_t
Fetch::beginTraceDemandAttempt(ThreadID tid, const RequestPtr &req)
{
    using Event = branch_prediction::btb_pred::BtbpTraceEvent;

    if (!isTraceMode() || !req || !req->hasXsMetadata()) {
        return 0;
    }
    auto meta = req->getXsMetadata();
    o3::TraceL1iDemandIdentity identity{
        meta.traceRequestUid, meta.eipDemandId,
        meta.traceDemandAttemptOrdinal, meta.traceDemandTerminalEmitted};
    const uint32_t ordinal = identity.beginAttempt();
    meta.traceDemandAttemptOrdinal = identity.attempts;
    req->setXsMetadata(meta);
    emitTraceDemandEvent(Event::L1IDemandAttempt, tid, req);
    return ordinal;
}

void
Fetch::closeTraceDemand(
    ThreadID tid, const RequestPtr &req,
    branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason reason)
{
    using Event = branch_prediction::btb_pred::BtbpTraceEvent;

    if (!isTraceMode() || !req || !req->hasXsMetadata()) {
        return;
    }
    auto meta = req->getXsMetadata();
    o3::TraceL1iDemandIdentity identity{
        meta.traceRequestUid, meta.eipDemandId,
        meta.traceDemandAttemptOrdinal, meta.traceDemandTerminalEmitted};
    if (!identity.close()) {
        return;
    }
    meta.traceDemandTerminalEmitted = true;
    meta.traceDemandOutcome = static_cast<uint32_t>(reason);
    req->setXsMetadata(meta);
    emitTraceDemandEvent(Event::L1IDemandTerminal, tid, req, reason);
}

void
Fetch::closeTraceFetchRequest(
    ThreadID tid,
    branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason reason)
{
    using Event = branch_prediction::btb_pred::BtbpTraceEvent;

    auto &cache_req = threads[tid].cacheReq;
    if (!isTraceMode() || !cache_req.traceRequestOpened ||
        cache_req.traceRequestTerminalEmitted) {
        return;
    }
    emitTraceFetchRequest(Event::FetchRequestTerminal, tid, reason);
    cache_req.traceRequestTerminalEmitted = true;
}

void
Fetch::closeOutstandingTraceFetch(
    ThreadID tid,
    branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason reason)
{
    if (!isTraceMode()) {
        return;
    }
    for (const auto &req : threads[tid].cacheReq.requests) {
        closeTraceDemand(tid, req, reason);
    }
    closeTraceFetchRequest(tid, reason);
}

bool
Fetch::traceInstructionBytesReady(
    ThreadID tid, Addr instruction_pc, unsigned instruction_size) const
{
    const auto &buffer = threads[tid];
    const auto &cache_req = buffer.cacheReq;
    if (!buffer.valid || instruction_size == 0 ||
        instruction_size > fetchBufferSize ||
        cache_req.getOverallStatus() != AccessComplete ||
        instruction_pc < buffer.startPC ||
        instruction_pc - buffer.startPC > fetchBufferSize - instruction_size) {
        return false;
    }

    // Require completed demand responses for every instruction byte, including
    // an instruction spanning two cache lines. Synthetic decoder bytes carry
    // instruction semantics, but must not manufacture cache supply readiness.
    // The traceL1iVisibilityTick gate additionally requires the byte to have
    // become visible to fetch through the modelled L1I supply path.
    for (unsigned offset = 0; offset < instruction_size; ++offset) {
        const Addr byte = instruction_pc + offset;
        bool covered = false;
        for (size_t index = 0; index < cache_req.requests.size(); ++index) {
            const auto &req = cache_req.requests[index];
            if (!req || !req->hasVaddr() || !req->hasXsMetadata() ||
                index >= cache_req.requestStatus.size() ||
                cache_req.requestStatus[index] != AccessComplete) {
                continue;
            }
            const Addr begin = req->getVaddr();
            if (byte >= begin && byte - begin < req->getSize()) {
                const auto meta = req->getXsMetadata();
                if (meta.traceL1iVisibilityTick != 0) {
                    covered = true;
                    break;
                }
            }
        }
        if (!covered) {
            return false;
        }
    }
    return true;
}

void
Fetch::emitTraceDecodeConsume(
    ThreadID tid, Addr instruction_pc, unsigned instruction_size,
    uint64_t trace_instruction_ordinal, bool wrong_path)
{
    using Event = branch_prediction::btb_pred::BtbpTraceEvent;

    panic_if(!traceInstructionBytesReady(
                 tid, instruction_pc, instruction_size),
             "trace decode at %#llx lacks complete L1I supply provenance",
             static_cast<unsigned long long>(instruction_pc));

    unsigned emitted = 0;
    const Addr instruction_end = instruction_pc + instruction_size;
    for (const auto &req : threads[tid].cacheReq.requests) {
        if (!req || !req->hasVaddr() || !req->hasXsMetadata()) {
            continue;
        }
        const Addr request_begin = req->getVaddr();
        const Addr request_end = request_begin + req->getSize();
        if (instruction_end <= request_begin || instruction_pc >= request_end) {
            continue;
        }
        const auto meta = req->getXsMetadata();
        Event event;
        event.tick = curTick();
        event.eventType = Event::DecodeConsume;
        event.threadId = tid;
        event.requestKind = Event::DemandRequest;
        event.requestKindValid = true;
        event.requestUid = meta.traceRequestUid;
        event.requestUidValid = meta.traceRequestUid != 0;
        event.lookupUid = meta.traceLookupUid;
        event.lookupUidValid = meta.traceLookupUid != 0;
        event.fetchEpoch = meta.traceFetchEpoch;
        event.fetchEpochValid = meta.traceFetchEpoch != 0;
        event.demandUid = meta.eipDemandId;
        event.demandUidValid = meta.eipDemandId != 0;
        event.ftqId = meta.traceFtqId;
        event.ftqIdValid = meta.traceIdentityValid;
        event.addressSpaceId = meta.traceAddressSpaceId;
        event.addressSpaceIdValid = meta.traceIdentityValid;
        event.asidHash = meta.traceAsidHash;
        event.asidHashValid = meta.traceIdentityValid;
        event.traceInstructionOrdinal = trace_instruction_ordinal;
        event.traceInstructionOrdinalValid =
            trace_instruction_ordinal != 0;
        event.visibilityTick = meta.traceL1iVisibilityTick;
        event.visibilityTickValid = meta.traceL1iVisibilityTick != 0;
        event.supplySource = Event::FetchBufferSupply;
        event.supplySourceValid = true;
        event.pathState = wrong_path ? Event::WrongPath : Event::CorrectPath;
        event.pathStateValid = true;
        event.lineSize = cacheBlkSize;
        event.lineSizeValid = true;
        event.virtualLineAddr = request_begin - request_begin % cacheBlkSize;
        event.virtualLineAddrValid = true;
        if (req->hasPaddr()) {
            event.lineAddr = req->getPaddr() -
                req->getPaddr() % cacheBlkSize;
        } else {
            event.lineAddr = event.virtualLineAddr;
        }
        event.lineAddrValid = true;
        if (meta.l1iResidencyId != 0) {
            event.residencyId = meta.l1iResidencyId;
            event.residencyIdValid = true;
        }
        if (meta.instPrefetchDecisionId != 0) {
            event.prefetchDecisionId = meta.instPrefetchDecisionId;
            event.prefetchDecisionIdValid = true;
            event.ownerPrefetchDecisionIds.push_back(
                meta.instPrefetchDecisionId);
        }
        cpu->notifyBtbpTrace(event);
        ++emitted;
    }
    panic_if(emitted == 0,
             "trace decode at %#llx emitted no L1I supply event",
             static_cast<unsigned long long>(instruction_pc));
}

void
Fetch::noteFdipCandidateLine(ThreadID tid, Addr lineAddr)
{
    ++fetchStats.fdipCandidateLines;
    auto &count = fdipCandidateLineSeen[tid][lineAddr];
    if (count == 0) {
        ++fetchStats.fdipUniqueCandidateLines;
    } else {
        ++fetchStats.fdipRepeatedCandidateLines;
    }
    ++count;
}

void
Fetch::noteFdipIssuedLine(ThreadID tid,
                          branch_prediction::btb_pred::FetchTargetId ftqId,
                          Addr physLineAddr)
{
    auto &target = fdipTrackedTargets[tid][ftqId];
    ++target.issuedLines;
    target.issuedPhysLines.insert(physLineAddr);

    auto &count = fdipIssuedLineSeen[tid][physLineAddr];
    if (count == 0) {
        ++fetchStats.fdipUniqueIssuedLines;
    } else {
        ++fetchStats.fdipRepeatedIssuedLines;
    }
    ++count;
}

void
Fetch::noteFdipWrongPathTargets(
    ThreadID tid, branch_prediction::btb_pred::FetchTargetId squashFtqId)
{
    auto &tracked = fdipTrackedTargets[tid];
    auto &wrong_path_seen = fdipWrongPathIssuedLineSeen[tid];
    auto it = tracked.upper_bound(squashFtqId);
    while (it != tracked.end()) {
        fetchStats.fdipWrongPathIssuedLines += it->second.issuedLines;
        for (const Addr phys_line_addr : it->second.issuedPhysLines) {
            if (wrong_path_seen.insert(phys_line_addr).second) {
                ++fetchStats.fdipWrongPathUniqueIssuedLines;
            }
        }
        it = tracked.erase(it);
    }
}

void
Fetch::retireCommittedFdipTargets(
    ThreadID tid, branch_prediction::btb_pred::FetchTargetId doneFtqId)
{
    auto &tracked = fdipTrackedTargets[tid];
    auto end = tracked.upper_bound(doneFtqId);
    tracked.erase(tracked.begin(), end);
}

void
Fetch::noteDemandOnWrongPathFdipLine(ThreadID tid, Addr physLineAddr)
{
    if (!fdipWrongPathIssuedLineSeen[tid].count(physLineAddr)) {
        return;
    }

    ++fetchStats.fdipWrongPathDemandAccesses;
    if (fdipWrongPathDemandReuseLineSeen[tid].insert(physLineAddr).second) {
        ++fetchStats.fdipWrongPathDemandReusedLines;
    }
}

unsigned
Fetch::computeFdipLineAddrs(
    const branch_prediction::btb_pred::FetchTarget &target,
    std::array<Addr, 2> &lineAddrs) const
{
    const Addr first_line = target.startPC - (target.startPC % cacheBlkSize);
    lineAddrs[0] = first_line;
    lineAddrs[1] = 0;

    if (!dbpbtb->fdipCoverActualFetchRange()) {
        return 1;
    }

    const Addr last_line =
        branch_prediction::btb_pred::fetchCoverageLastLineAddr(
            target.startPC, target.predEndPC, fetchBufferSize, cacheBlkSize);

    if (last_line == first_line) {
        return 1;
    }

    lineAddrs[1] = last_line;
    return 2;
}

bool
Fetch::initFdipTargetState(ThreadID tid)
{
    if (!dbpbtb->ftqHasPrefetch(tid)) {
        return false;
    }

    const auto &target = dbpbtb->ftqPrefetchTarget(tid);
    auto &state = fdipState[tid];

    state.reset();
    state.valid = true;
    state.ftqId = dbpbtb->ftqPrefetchHeadId(tid);
    state.addressSpaceId = target.addressSpaceId;
    state.asidHash = target.asidHash;
    state.startPC = target.startPC;
    state.epoch = fdipEpoch[tid];
    std::array<Addr, 2> line_addrs{};
    state.lineCount = computeFdipLineAddrs(target, line_addrs);
    for (unsigned i = 0; i < state.lineCount; ++i) {
        state.lines[i].lineAddr = line_addrs[i];
        noteFdipCandidateLine(tid, line_addrs[i]);
    }

    DPRINTF(Fetch,
            "[tid:%i] FDIP init: ftqId=%lu startPC=%#lx lineCount=%u "
            "line0=%#lx line1=%#lx\n",
            tid, static_cast<unsigned long>(state.ftqId), state.startPC,
            state.lineCount, state.lineCount > 0 ? state.lines[0].lineAddr : 0,
            state.lineCount > 1 ? state.lines[1].lineAddr : 0);

    return true;
}

bool
Fetch::startFdipTranslation(ThreadID tid, unsigned lineIndex)
{
    auto &state = fdipState[tid];
    assert(state.valid);
    assert(lineIndex < state.lineCount);

    auto &line = state.lines[lineIndex];
    if (line.status != FdipIdle) {
        return false;
    }

    uint64_t attempt_id = 0;
    uint64_t decision_id = 0;
    branch_prediction::btb_pred::BtbpTraceEvent::GateOutcome gate_outcome;
    const bool accepted = reserveInstPrefetchDecision(
            dbpbtb->fdipMaxOutstanding(),
            dbpbtb->fdipIssueBandwidth(), attempt_id, decision_id,
            gate_outcome);
    Request::XsMetadata trace_meta;
    trace_meta.traceIdentityValid = true;
    trace_meta.traceAddressSpaceId = state.addressSpaceId;
    trace_meta.traceAsidHash = state.asidHash;
    trace_meta.traceFtqId = state.ftqId;
    emitInstPrefetchGateAttempt(
        tid, PF_FDIP, line.lineAddr, state.startPC, trace_meta,
        attempt_id, decision_id, gate_outcome);
    if (!accepted) {
        return false;
    }

    Request::Flags flags;
    flags.set(Request::INST_FETCH);
    flags.set(Request::PREFETCH);

    RequestPtr req = std::make_shared<Request>(
        line.lineAddr, cacheBlkSize, flags, cpu->instRequestorId(),
        state.startPC, cpu->thread[tid]->contextId());
    req->taskId(cpu->taskId());
    req->setPFSource(PF_FDIP);
    req->setPFDepth(0);
    Request::XsMetadata xsMeta(PF_FDIP, 0);
    xsMeta.fdipEpoch = state.epoch;
    xsMeta.fdipFtqId = state.ftqId;
    xsMeta.fdipStartPC = state.startPC;
    xsMeta.instPrefetchAttemptId = attempt_id;
    xsMeta.instPrefetchDecisionId = decision_id;
    xsMeta.btbpRoiOrigin = btbpRoiActive;
    xsMeta.traceIdentityValid = true;
    xsMeta.traceAddressSpaceId = state.addressSpaceId;
    xsMeta.traceAsidHash = state.asidHash;
    xsMeta.traceFtqId = state.ftqId;
    req->setXsMetadata(xsMeta);

    line.req = req;
    line.status = FdipTlbWait;

    FdipPendingRequest pending;
    pending.tid = tid;
    pending.ftqId = state.ftqId;
    pending.startPC = state.startPC;
    pending.lineAddr = line.lineAddr;
    pending.epoch = state.epoch;
    pending.lineIndex = lineIndex;
    pending.outstanding = true;
    pending.req = req;
    fdipPendingReqs.push_back(pending);
    emitInstPrefetchTrace(
        branch_prediction::btb_pred::BtbpTraceEvent::
            IPrefetchDecisionAccepted,
        tid, req);
    if (instPrefetchOutstandingLines >
        fetchStats.fdipOutstandingMax.value()) {
        fetchStats.fdipOutstandingMax = instPrefetchOutstandingLines;
    }

    DPRINTF(Fetch,
            "[tid:%i] FDIP translate: ftqId=%lu line[%u]=%#lx "
            "startPC=%#lx\n",
            tid, static_cast<unsigned long>(state.ftqId), lineIndex,
            line.lineAddr, state.startPC);

    auto *trans = new FdipTranslation(this);
    cpu->mmu->translateTiming(req, cpu->thread[tid]->getTC(), trans,
                              BaseMMU::Execute);
    return true;
}

bool
Fetch::issueFdipReadyLine(ThreadID tid, unsigned lineIndex,
                          unsigned &remainingBudget)
{
    auto &state = fdipState[tid];
    assert(state.valid);
    assert(lineIndex < state.lineCount);

    auto &line = state.lines[lineIndex];
    if (line.status != FdipReady || !line.req) {
        return false;
    }

    PacketPtr pkt = new Packet(line.req, Packet::makeReadCmd(line.req));
    pkt->allocate();
    pkt->setSendRightAway();

    if (!icachePort.sendTimingReq(pkt)) {
        DPRINTF(Fetch,
                "[tid:%i] FDIP issue dropped due to cache backpressure: "
                "ftqId=%lu line[%u]=%#lx\n",
                tid, static_cast<unsigned long>(state.ftqId), lineIndex,
                line.lineAddr);
        delete pkt;
        return false;
    }

    bool found_pending = false;
    for (auto &pending : fdipPendingReqs) {
        if (pending.req == line.req) {
            found_pending = true;
            break;
        }
    }
    assert(found_pending);

    if (line.req->hasPaddr()) {
        line.physLineAddr = line.req->getPaddr() -
                            (line.req->getPaddr() % cacheBlkSize);
    } else {
        line.physLineAddr = line.lineAddr;
    }

    line.status = FdipInflight;
    ++fetchStats.fdipIssuedLines;
    noteFdipIssuedLine(tid, state.ftqId, line.physLineAddr);

    branch_prediction::btb_pred::BtbpTraceEvent issue_event;
    issue_event.tick = curTick();
    issue_event.eventType =
        branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchIssue;
    issue_event.threadId = tid;
    issue_event.lineAddr = line.physLineAddr;
    issue_event.lineAddrValid = true;
    issue_event.virtualLineAddr = line.lineAddr;
    issue_event.virtualLineAddrValid = true;
    issue_event.lineSize = cacheBlkSize;
    issue_event.lineSizeValid = true;
    issue_event.triggerPc = state.startPC;
    issue_event.triggerPcValid = true;
    issue_event.addressSpaceId = state.addressSpaceId;
    issue_event.addressSpaceIdValid = true;
    issue_event.asidHash = state.asidHash;
    issue_event.asidHashValid = true;
    issue_event.ftqId = state.ftqId;
    issue_event.ftqIdValid = true;
    issue_event.fdipEpoch = state.epoch;
    issue_event.fdipEpochValid = true;
    issue_event.requestKind =
        branch_prediction::btb_pred::BtbpTraceEvent::PrefetchRequest;
    issue_event.requestKindValid = true;
    issue_event.prefetchDecisionId =
        line.req->getXsMetadata().instPrefetchDecisionId;
    issue_event.prefetchDecisionIdValid = true;
    issue_event.prefetchSource = static_cast<uint32_t>(PF_FDIP);
    issue_event.prefetchSourceValid = true;
    branch_prediction::btb_pred::applyFdipAttemptIdentity(
        issue_event, line.req->getXsMetadata());
    cpu->notifyBtbpTrace(issue_event);

    if (remainingBudget > 0) {
        --remainingBudget;
    }

    DPRINTF(Fetch,
            "[tid:%i] FDIP issued: ftqId=%lu line[%u]=%#lx outstanding=%u\n",
            tid, static_cast<unsigned long>(state.ftqId), lineIndex,
            line.lineAddr, instPrefetchOutstandingLines);

    return true;
}

bool
Fetch::finishFdipTargetIfReady(ThreadID tid)
{
    auto &state = fdipState[tid];
    if (!state.valid) {
        return false;
    }

    for (unsigned i = 0; i < state.lineCount; ++i) {
        if (!state.lines[i].isTerminal()) {
            return false;
        }
    }

    if (state.epoch == fdipEpoch[tid] && dbpbtb->ftqHasPrefetch(tid) &&
        dbpbtb->ftqPrefetchHeadId(tid) == state.ftqId) {
        const auto fetch_head = dbpbtb->ftqHeadId(tid);
        if (state.ftqId > fetch_head) {
            const auto distance = state.ftqId - fetch_head;
            if (distance < dbpbtb->fdipLookaheadEntries()) {
                DPRINTF(Fetch, "[tid:%i] FDIP consume ftqId=%lu\n", tid,
                        static_cast<unsigned long>(state.ftqId));
                dbpbtb->consumePrefetchTarget(tid);
                state.reset();
                return true;
            }

            return false;
        }
    }

    state.reset();
    return true;
}

bool
Fetch::tryCompleteFdipLineByDirectProbe(ThreadID tid, unsigned lineIndex,
                                        const RequestPtr &mem_req)
{
    if (!fdipIcacheAccessor || !mem_req->hasPaddr()) {
        return false;
    }

    auto &state = fdipState[tid];
    assert(state.valid);
    assert(lineIndex < state.lineCount);

    auto &line = state.lines[lineIndex];
    line.physLineAddr = mem_req->getPaddr() -
                        (mem_req->getPaddr() % cacheBlkSize);

    uint8_t hit_way = 0;
    if (!fdipIcacheAccessor->lookupHitWay(line.physLineAddr,
                                          mem_req->isSecure(), hit_way)) {
        return false;
    }

    installFdipProbeHint(tid, state.ftqId, state.epoch, line.physLineAddr,
                         mem_req->isSecure(), hit_way);
    line.directProbeHit = true;
    line.status = FdipDone;
    line.req.reset();
    ++fetchStats.fdipDirectProbeHit;

    DPRINTF(Fetch,
            "[tid:%i] FDIP direct probe hit: ftqId=%u line[%u] vaddr=%#lx "
            "paddr=%#lx\n",
            tid, state.ftqId, lineIndex, line.lineAddr, line.physLineAddr);

    finishFdipTargetIfReady(tid);
    return true;
}

void
Fetch::installFdipProbeHint(
    ThreadID tid, branch_prediction::btb_pred::FetchTargetId ftqId,
    uint64_t epoch, Addr physLineAddr, bool isSecure, uint8_t selectedWay)
{
    auto &hints = fdipProbeHints[tid];
    for (auto it = hints.begin(); it != hints.end();) {
        if (it->ftqId == ftqId && it->physLineAddr == physLineAddr &&
            it->isSecure == isSecure) {
            it = hints.erase(it);
        } else {
            ++it;
        }
    }

    FdipProbeHint hint;
    hint.epoch = epoch;
    hint.ftqId = ftqId;
    hint.physLineAddr = physLineAddr;
    hint.isSecure = isSecure;
    hint.selectedWay = selectedWay;
    hint.installTick = curTick();
    hints.push_back(hint);

    const size_t max_hints = std::max<size_t>(
        4, static_cast<size_t>(
               2 * std::max(1u, dbpbtb->fdipLookaheadEntries()) + 4));
    while (hints.size() > max_hints) {
        hints.pop_front();
    }

    ++fetchStats.fdipWayHintsInstalled;
}

bool
Fetch::lookupAndConsumeFdipProbeHint(
    ThreadID tid, branch_prediction::btb_pred::FetchTargetId ftqId,
    Addr physLineAddr, bool isSecure, FdipProbeHint &hint)
{
    auto &hints = fdipProbeHints[tid];
    for (auto it = hints.begin(); it != hints.end(); ++it) {
        if (it->epoch != fdipEpoch[tid] || it->ftqId != ftqId ||
            it->physLineAddr != physLineAddr || it->isSecure != isSecure) {
            continue;
        }

        hint = *it;
        hints.erase(it);
        ++fetchStats.fdipWayHintsConsumed;
        return true;
    }

    return false;
}

void
Fetch::finishFdipTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    using TerminalReason = branch_prediction::btb_pred::BtbpTraceEvent::
        TerminalReason;

    auto it = fdipPendingReqs.end();
    for (auto req_it = fdipPendingReqs.begin(); req_it != fdipPendingReqs.end();
         ++req_it) {
        if (req_it->req == mem_req) {
            it = req_it;
            break;
        }
    }

    if (it == fdipPendingReqs.end()) {
        return;
    }

    const FdipPendingRequest pending = *it;
    auto &state = fdipState[pending.tid];
    const bool state_matches =
        state.valid &&
        state.ftqId == pending.ftqId &&
        state.epoch == pending.epoch &&
        pending.lineIndex < state.lineCount &&
        state.lines[pending.lineIndex].req == mem_req;
    const auto release_pending = [&]() {
        if (it->outstanding) {
            releaseInstPrefetchDecision();
            it->outstanding = false;
        }
    };

    if (fault != NoFault) {
        ++fetchStats.fdipFilteredFault;
        if (state_matches) {
            state.lines[pending.lineIndex].status = FdipFilteredFault;
            state.lines[pending.lineIndex].req.reset();
            finishFdipTargetIfReady(pending.tid);
        }
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchTerminal,
            pending.tid, mem_req, TerminalReason::TranslationFault);
        release_pending();
        fdipPendingReqs.erase(it);
        return;
    }

    if (pending.epoch != fdipEpoch[pending.tid]) {
        ++fetchStats.fdipEpochMismatch;
        if (state_matches) {
            state.lines[pending.lineIndex].status = FdipDone;
            state.lines[pending.lineIndex].req.reset();
            finishFdipTargetIfReady(pending.tid);
        }
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchTerminal,
            pending.tid, mem_req, TerminalReason::ResetCanceled);
        release_pending();
        fdipPendingReqs.erase(it);
        return;
    }

    if (!cpu->system->isMemAddr(mem_req->getPaddr()) ||
        mem_req->isUncacheable() || mem_req->isStrictlyOrdered() ||
        mem_req->isLocalAccess()) {
        ++fetchStats.fdipFilteredUncacheable;
        if (state_matches) {
            state.lines[pending.lineIndex].status = FdipFilteredUncacheable;
            state.lines[pending.lineIndex].req.reset();
            finishFdipTargetIfReady(pending.tid);
        }
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchTerminal,
            pending.tid, mem_req, TerminalReason::Uncacheable);
        release_pending();
        fdipPendingReqs.erase(it);
        return;
    }

    if (!state_matches) {
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchTerminal,
            pending.tid, mem_req, TerminalReason::ResetCanceled);
        release_pending();
        fdipPendingReqs.erase(it);
        return;
    }

    if (tryCompleteFdipLineByDirectProbe(pending.tid, pending.lineIndex,
                                         mem_req)) {
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchTerminal,
            pending.tid, mem_req,
            TerminalReason::CacheOrMergeCompletion);
        release_pending();
        fdipPendingReqs.erase(it);
        return;
    }

    const auto recent_unused_cycles = dbpbtb->fdipRecentUnusedCycles();
    if (recent_unused_cycles > 0 && fdipIcacheAccessor &&
        mem_req->hasPaddr() &&
        fdipIcacheAccessor->shouldSuppressFdipLine(
            mem_req->getPaddr(), mem_req->isSecure(),
            recent_unused_cycles)) {
        ++fetchStats.fdipFilteredRecentUnused;
        state.lines[pending.lineIndex].status = FdipFilteredRecentUnused;
        state.lines[pending.lineIndex].req.reset();
        finishFdipTargetIfReady(pending.tid);
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchTerminal,
            pending.tid, mem_req, TerminalReason::PolicyFiltered);
        release_pending();
        fdipPendingReqs.erase(it);
        return;
    }

    state.lines[pending.lineIndex].status = FdipReady;
}

void
Fetch::processFdipCompletion(PacketPtr pkt)
{
    using TerminalReason = branch_prediction::btb_pred::BtbpTraceEvent::
        TerminalReason;

    auto it = fdipPendingReqs.end();
    for (auto req_it = fdipPendingReqs.begin(); req_it != fdipPendingReqs.end();
         ++req_it) {
        if (req_it->req == pkt->req) {
            it = req_it;
            break;
        }
    }

    if (it == fdipPendingReqs.end()) {
        delete pkt;
        return;
    }

    const FdipPendingRequest pending = *it;
    auto &state = fdipState[pending.tid];
    const bool state_matches =
        state.valid &&
        state.ftqId == pending.ftqId &&
        state.epoch == pending.epoch &&
        pending.lineIndex < state.lineCount &&
        state.lines[pending.lineIndex].req == pkt->req;

    emitInstPrefetchTrace(
        branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchTerminal,
        pending.tid, pkt->req,
        pkt->mshrArbFailed() ? TerminalReason::QueueFullCompletion :
                              TerminalReason::CacheOrMergeCompletion);

    if (it->outstanding) {
        releaseInstPrefetchDecision();
        it->outstanding = false;
    }

    if (pending.epoch != fdipEpoch[pending.tid]) {
        ++fetchStats.fdipEpochMismatch;
    } else if (state_matches) {
        state.lines[pending.lineIndex].status = FdipDone;
        state.lines[pending.lineIndex].req.reset();
        finishFdipTargetIfReady(pending.tid);
    }

    DPRINTF(Fetch,
            "[tid:%i] FDIP complete: ftqId=%lu line[%u]=%#lx "
            "outstanding=%u\n",
            pending.tid, static_cast<unsigned long>(pending.ftqId),
            pending.lineIndex, pending.lineAddr,
            instPrefetchOutstandingLines);

    delete pkt;
    fdipPendingReqs.erase(it);
}

void
Fetch::runFdip(ThreadID tid)
{
    if (!dbpbtb->fdipEnabled() || !dbpbtb->ftqHasFetching(tid)) {
        return;
    }

    while (dbpbtb->ftqHasPrefetch(tid) &&
           dbpbtb->ftqPrefetchHeadId(tid) == dbpbtb->ftqHeadId(tid)) {
        if (fdipState[tid].valid &&
            fdipState[tid].ftqId == dbpbtb->ftqPrefetchHeadId(tid)) {
            resetFdipState(tid);
        }
        DPRINTF(Fetch, "[tid:%i] FDIP skip current demand head ftqId=%lu\n",
                tid,
                static_cast<unsigned long>(dbpbtb->ftqPrefetchHeadId(tid)));
        dbpbtb->consumePrefetchTarget(tid);
    }

    if (!dbpbtb->ftqHasPrefetch(tid)) {
        return;
    }

    const branch_prediction::btb_pred::FetchTargetId fetch_head =
        dbpbtb->ftqHeadId(tid);
    const branch_prediction::btb_pred::FetchTargetId prefetch_head =
        dbpbtb->ftqPrefetchHeadId(tid);
    if (prefetch_head <= fetch_head) {
        return;
    }

    const branch_prediction::btb_pred::FetchTargetId distance =
        prefetch_head - fetch_head;
    if (distance > dbpbtb->fdipLookaheadEntries()) {
        return;
    }

    if (fdipState[tid].valid &&
        (fdipState[tid].epoch != fdipEpoch[tid] ||
         fdipState[tid].ftqId != prefetch_head)) {
        resetFdipState(tid);
    }

    if (!fdipState[tid].valid && !initFdipTargetState(tid)) {
        return;
    }

    auto &state = fdipState[tid];
    for (unsigned i = 0; i < state.lineCount; ++i) {
        if (state.lines[i].status == FdipIdle) {
            startFdipTranslation(tid, i);
            break;
        }
    }

    unsigned remaining_budget = dbpbtb->fdipIssueBandwidth();
    if (remaining_budget == 0) {
        return;
    }

    for (unsigned i = 0; i < state.lineCount && remaining_budget > 0; ++i) {
        if (state.lines[i].status == FdipReady) {
            issueFdipReadyLine(tid, i, remaining_budget);
        }
    }

    finishFdipTargetIfReady(tid);
}

void
Fetch::runFdip()
{
    if (!dbpbtb->fdipEnabled()) {
        return;
    }

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        runFdip(tid);
    }
}

bool
Fetch::acceptEipCandidate(
    ThreadID tid, const EntanglingPrefetcher::Candidate &candidate,
    const Request::XsMetadata &trigger_meta)
{
    if (!eipPrefetcher) {
        return false;
    }
    if (eipPrefetcher->hasResidentVirtualLine(candidate.virtualLine)) {
        return false;
    }

    uint64_t attempt_id = 0;
    uint64_t decision_id = 0;
    branch_prediction::btb_pred::BtbpTraceEvent::GateOutcome gate_outcome;
    const bool accepted = reserveInstPrefetchDecision(
        eipMaxOutstanding, eipIssueBandwidth, attempt_id, decision_id,
        gate_outcome);
    emitInstPrefetchGateAttempt(
        tid, PF_EIP, candidate.virtualLine << 6,
        candidate.triggerVirtualLine << 6, trigger_meta,
        attempt_id, decision_id, gate_outcome);
    if (!accepted) {
        return false;
    }

    Request::Flags flags;
    flags.set(Request::INST_FETCH);
    flags.set(Request::PREFETCH);

    const Addr virtual_addr = candidate.virtualLine << 6;
    const Addr trigger_pc = candidate.triggerVirtualLine << 6;
    RequestPtr req = std::make_shared<Request>(
        virtual_addr, cacheBlkSize, flags, cpu->instRequestorId(),
        trigger_pc, cpu->thread[tid]->contextId());
    req->taskId(cpu->taskId());
    req->setPFSource(PF_EIP);
    req->setPFDepth(0);

    Request::XsMetadata xs_meta(PF_EIP, 0);
    xs_meta.instPrefetchAttemptId = attempt_id;
    xs_meta.instPrefetchDecisionId = decision_id;
    xs_meta.eipTriggerPC = trigger_pc;
    xs_meta.eipContextId = cpu->thread[tid]->contextId();
    xs_meta.btbpRoiOrigin = btbpRoiActive;
    xs_meta.traceIdentityValid = trigger_meta.traceIdentityValid;
    xs_meta.traceAddressSpaceId = trigger_meta.traceAddressSpaceId;
    xs_meta.traceAsidHash = trigger_meta.traceAsidHash;
    xs_meta.traceFtqId = trigger_meta.traceFtqId;
    req->setXsMetadata(xs_meta);

    EipPendingRequest pending;
    pending.tid = tid;
    pending.virtualLine = candidate.virtualLine;
    pending.decisionId = decision_id;
    pending.status = EipAccepted;
    pending.req = req;
    eipPendingReqs.push_back(pending);
    emitInstPrefetchTrace(
        branch_prediction::btb_pred::BtbpTraceEvent::
            IPrefetchDecisionAccepted,
        tid, req);
    ++fetchStats.eipCandidatesAccepted;
    return true;
}

void
Fetch::finishEipTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    using TerminalReason = branch_prediction::btb_pred::BtbpTraceEvent::
        TerminalReason;

    auto it = std::find_if(
        eipPendingReqs.begin(), eipPendingReqs.end(),
        [&mem_req](const EipPendingRequest &pending) {
            return pending.req == mem_req;
        });
    if (it == eipPendingReqs.end()) {
        return;
    }

    if (fault != NoFault) {
        ++fetchStats.eipFilteredFault;
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchTerminal,
            it->tid, mem_req, TerminalReason::TranslationFault);
        eipPrefetcher->cancelAcceptedRequest(it->virtualLine);
        releaseInstPrefetchDecision();
        eipPendingReqs.erase(it);
        return;
    }

    if (!cpu->system->isMemAddr(mem_req->getPaddr()) ||
        mem_req->isUncacheable() || mem_req->isStrictlyOrdered() ||
        mem_req->isLocalAccess()) {
        ++fetchStats.eipFilteredUncacheable;
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchTerminal,
            it->tid, mem_req, TerminalReason::Uncacheable);
        eipPrefetcher->cancelAcceptedRequest(it->virtualLine);
        releaseInstPrefetchDecision();
        eipPendingReqs.erase(it);
        return;
    }

    it->status = EipReady;
}

void
Fetch::processEipCompletion(PacketPtr pkt)
{
    using TerminalReason = branch_prediction::btb_pred::BtbpTraceEvent::
        TerminalReason;

    auto it = std::find_if(
        eipPendingReqs.begin(), eipPendingReqs.end(),
        [pkt](const EipPendingRequest &pending) {
            return pending.req == pkt->req;
        });
    DPRINTF(Fetch,
            "BTBP EIP completion addr=%#llx decision=%llu match=%d "
            "pending=%zu\n",
            pkt->getAddr(),
            pkt->req->hasXsMetadata() ?
                pkt->req->getXsMetadata().instPrefetchDecisionId : 0,
            it != eipPendingReqs.end(), eipPendingReqs.size());
    if (it == eipPendingReqs.end()) {
        delete pkt;
        return;
    }

    // A fill removes the timing entry before the response returns. A hit or
    // coalesced completion has no distinct fill callback for this request.
    emitInstPrefetchTrace(
        branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchTerminal,
        it->tid, pkt->req,
        pkt->mshrArbFailed() ? TerminalReason::QueueFullCompletion :
                              TerminalReason::CacheOrMergeCompletion);
    eipPrefetcher->cancelAcceptedRequest(it->virtualLine);
    releaseInstPrefetchDecision();
    ++fetchStats.eipCompletedLines;
    delete pkt;
    eipPendingReqs.erase(it);
}

void
Fetch::runEip()
{
    if (!eipPrefetcher) {
        return;
    }

    // Delay translation until after observeDemand has installed the model's
    // timing entry. Some ITLB paths complete synchronously.
    for (size_t i = 0; i < eipPendingReqs.size(); ++i) {
        if (eipPendingReqs[i].status != EipAccepted) {
            continue;
        }
        RequestPtr req = eipPendingReqs[i].req;
        ThreadID tid = eipPendingReqs[i].tid;
        eipPendingReqs[i].status = EipTlbWait;
        auto *trans = new EipTranslation(this);
        cpu->mmu->translateTiming(
            req, cpu->thread[tid]->getTC(), trans, BaseMMU::Execute);
        break;
    }

    // Demand requests are issued before runEip() in this tick and retain
    // ownership of a retry cycle.
    if (cacheBlocked || !retryPkt.empty()) {
        return;
    }

    for (auto &pending : eipPendingReqs) {
        if (pending.status != EipReady) {
            continue;
        }

        PacketPtr pkt = new Packet(
            pending.req, Packet::makeReadCmd(pending.req));
        pkt->allocate();
        pkt->setSendRightAway();
        if (!icachePort.sendTimingReq(pkt)) {
            ++fetchStats.eipBackpressureEvents;
            delete pkt;
            break;
        }

        pending.status = EipInflight;
        ++fetchStats.eipIssuedLines;
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::IPrefetchIssue,
            pending.tid, pending.req);
        break;
    }
}

void
Fetch::cancelAllEipRequests()
{
    for (const auto &pending : eipPendingReqs) {
        emitInstPrefetchTrace(
            branch_prediction::btb_pred::BtbpTraceEvent::
                IPrefetchTerminal,
            pending.tid, pending.req,
            branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason::
                ResetCanceled);
        if (eipPrefetcher) {
            eipPrefetcher->cancelAcceptedRequest(pending.virtualLine);
        }
        releaseInstPrefetchDecision();
    }
    eipPendingReqs.clear();
}

void
Fetch::beginBtbpRoiTracking()
{
    btbpRoiActive = true;
    btbpRoiDrainMode = false;
    btbpRoiDrainExitRequested = false;
    if (fdipIcacheAccessor) {
        fdipIcacheAccessor->beginBtbpRoiTracking();
    }
}

void
Fetch::beginBtbpRoiDrain()
{
    btbpRoiDrainMode = true;
    btbpRoiDrainBeginTick = curTick();
}

uint64_t
Fetch::demandIdForPC(ThreadID tid, Addr pc) const
{
    const auto &cache_req = threads[tid].cacheReq;
    for (const auto &req : cache_req.requests) {
        if (!req || !req->hasXsMetadata() || !req->hasVaddr()) {
            continue;
        }
        const Addr request_start = req->getVaddr();
        const Addr request_end = request_start + req->getSize();
        if (pc >= request_start && pc < request_end) {
            return req->getXsMetadata().eipDemandId;
        }
    }
    return 0;
}

void
Fetch::notifyEipDemand(
    ContextID contextId, Addr virtualAddr, Addr physicalAddr,
    uint64_t demandId, bool cacheHit, bool prefetchHit, bool wrongPath,
    const Request::XsMetadata &request_meta)
{
    if (!eipPrefetcher) {
        return;
    }

    const ThreadID tid = cpu->contextToThread(contextId);
    EntanglingPrefetcher::DemandAccess access;
    access.virtualLine = virtualAddr >> 6;
    access.physicalLine = physicalAddr >> 6;
    access.cycle = static_cast<uint64_t>(cpu->curCycle());
    access.instructionId = demandId;
    access.cacheHit = cacheHit;
    access.prefetchHit = prefetchHit;
    access.wrongPath = wrongPath;

    ++fetchStats.eipDemandAccesses;
    const auto result = eipPrefetcher->observeDemand(
        access, [this, tid, request_meta](
                    const EntanglingPrefetcher::Candidate &candidate) {
            return acceptEipCandidate(tid, candidate, request_meta);
        });
    fetchStats.eipCandidatesOffered += result.candidatesOffered;
}

void
Fetch::notifyEipFill(
    ContextID, Addr virtualAddr, Addr physicalAddr)
{
    if (!eipPrefetcher) {
        return;
    }

    EntanglingPrefetcher::FillEvent event;
    event.virtualLine = virtualAddr >> 6;
    event.physicalLine = physicalAddr >> 6;
    event.cycle = static_cast<uint64_t>(cpu->curCycle());
    eipPrefetcher->observeFill(event);
    ++fetchStats.eipFillEvents;
}

void
Fetch::notifyEipEvict(ContextID, Addr physicalAddr)
{
    if (!eipPrefetcher) {
        return;
    }

    EntanglingPrefetcher::EvictEvent event;
    event.physicalLine = physicalAddr >> 6;
    event.cycle = static_cast<uint64_t>(cpu->curCycle());
    eipPrefetcher->observeEvict(event);
    ++fetchStats.eipEvictionEvents;
}

bool
Fetch::shouldDropFdipRefill(ContextID contextId,
                            const Request::XsMetadata &xsMeta) const
{
    if (!xsMeta.isFdip() || !dbpbtb->fdipDropRefillOnEpochMismatch()) {
        return false;
    }

    const ThreadID tid = cpu->contextToThread(contextId);
    return xsMeta.fdipEpoch != fdipEpoch[tid];
}

bool
Fetch::handleMultiCacheLineFetch(Addr vaddr, ThreadID tid, Addr pc)
{
    DPRINTF(Fetch, "[tid:%i] Handling multi-cacheline fetch for addr %#x, pc=%#lx\n", tid, vaddr, pc);
    // Transition to WaitingCache state when initiating cache access
    setThreadStatus(tid, WaitingCache);

    // Reset cache request state for this thread
    threads[tid].cacheReq.reset();
    threads[tid].usedForTwoFetch = false;
    threads[tid].cacheReq.baseAddr = vaddr;
    threads[tid].cacheReq.totalSize = fetchBufferSize;

    const auto &fetch_target = dbpbtb->ftqFetchingTarget(tid);
    if (isTraceMode()) {
        const auto request_identity =
            dbpbtb->claimTraceRequestIdentity(tid);
        panic_if(request_identity.fetchEpoch !=
                     fetch_target.traceFetchEpoch,
                 "trace fetch request epoch %llu differs from lookup epoch "
                 "%llu for FTQ %llu",
                 static_cast<unsigned long long>(
                     request_identity.fetchEpoch),
                 static_cast<unsigned long long>(
                     fetch_target.traceFetchEpoch),
                 static_cast<unsigned long long>(dbpbtb->ftqHeadId(tid)));
        auto &cache_req = threads[tid].cacheReq;
        cache_req.traceRequestUid = request_identity.requestUid;
        cache_req.traceLookupUid = request_identity.lookupUid;
        cache_req.traceFetchEpoch = request_identity.fetchEpoch;
        cache_req.traceInstructionOrdinal =
            pendingTraceInstructionOrdinal[tid];
        cache_req.traceFtqId = dbpbtb->ftqHeadId(tid);
        cache_req.traceAddressSpaceId = fetch_target.addressSpaceId;
        cache_req.traceAsidHash = fetch_target.asidHash;
        cache_req.tracePathWrong = pendingTracePathWrong[tid];
        cache_req.traceRequestOpened = true;
        emitTraceFetchRequest(
            branch_prediction::btb_pred::BtbpTraceEvent::FetchRequestOpen,
            tid);
    }
    Request::XsMetadata trace_meta;
    trace_meta.traceIdentityValid = true;
    trace_meta.traceAddressSpaceId = fetch_target.addressSpaceId;
    trace_meta.traceAsidHash = fetch_target.asidHash;
    trace_meta.traceFtqId = dbpbtb->ftqHeadId(tid);
    if (isTraceMode()) {
        trace_meta.traceRequestUid =
            threads[tid].cacheReq.traceRequestUid;
        trace_meta.traceLookupUid = fetch_target.traceLookupUid;
        trace_meta.traceFetchEpoch =
            threads[tid].cacheReq.traceFetchEpoch;
        trace_meta.traceInstructionOrdinal =
            pendingTraceInstructionOrdinal[tid];
        trace_meta.tracePathWrong = pendingTracePathWrong[tid];
    }

    Addr fetchPC = vaddr;
    unsigned fetchSize = cacheBlkSize - fetchPC % cacheBlkSize;  // Size for first cache line

    DPRINTF(Fetch, "[tid:%i] Creating first cache line request: addr=%#x, size=%d\n",
            tid, fetchPC, fetchSize);

    // Create and send first request (tail of first cache line)
    RequestPtr first_mem_req = std::make_shared<Request>(
        fetchPC, fetchSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    first_mem_req->taskId(cpu->taskId());
    Request::XsMetadata first_meta = trace_meta;
    first_meta.eipDemandId = nextL1iDemandUid++;
    first_meta.eipContextId = cpu->thread[tid]->contextId();
    first_mem_req->setXsMetadata(first_meta);
    first_mem_req->setMisalignedFetch();
    first_mem_req->setReqNum(1);

    threads[tid].cacheReq.addRequest(first_mem_req); // packet will be created later
    emitTraceDemandEvent(
        branch_prediction::btb_pred::BtbpTraceEvent::L1IDemandIssue,
        tid, first_mem_req);

    // Initiate translation for first request
    updateCacheRequestStatusByRequest(tid, first_mem_req, TlbWait);
    setAllFetchStalls(StallReason::ITlbStall);
    FetchTranslation *trans = new FetchTranslation(this);
    cpu->mmu->translateTiming(first_mem_req, cpu->thread[tid]->getTC(),
                              trans, BaseMMU::Execute);

    // Prepare second request (head of second cache line)
    fetchPC += fetchSize;  // Move to start of next cache line
    assert(fetchPC % cacheBlkSize == 0);
    fetchSize = fetchBufferSize - fetchSize;  // Remaining size

    DPRINTF(Fetch, "[tid:%i] Creating second cache line request: addr=%#x, size=%d\n",
            tid, fetchPC, fetchSize);

    // Create and send second request
    RequestPtr second_mem_req = std::make_shared<Request>(
        fetchPC, fetchSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    second_mem_req->taskId(cpu->taskId());
    Request::XsMetadata second_meta = trace_meta;
    second_meta.eipDemandId = nextL1iDemandUid++;
    second_meta.eipContextId = cpu->thread[tid]->contextId();
    second_mem_req->setXsMetadata(second_meta);
    second_mem_req->setMisalignedFetch();
    second_mem_req->setReqNum(2);

    threads[tid].cacheReq.addRequest(second_mem_req);  // Add second request to cache request
    emitTraceDemandEvent(
        branch_prediction::btb_pred::BtbpTraceEvent::L1IDemandIssue,
        tid, second_mem_req);

    DPRINTF(Fetch, "[tid:%i] Initiating translation for second cache line\n", tid);

    // Always initiate translation for second request, regardless of first request status
    updateCacheRequestStatusByRequest(tid, second_mem_req, TlbWait);
    setAllFetchStalls(StallReason::ITlbStall);
    FetchTranslation *trans2 = new FetchTranslation(this);
    cpu->mmu->translateTiming(second_mem_req, cpu->thread[tid]->getTC(),
                              trans2, BaseMMU::Execute);
    return true;
}

bool
Fetch::processMultiCacheLineCompletion(ThreadID tid, PacketPtr pkt)
{
    DPRINTF(Fetch, "[tid:%i] Processing dual cacheline fetch completion for addr %#lx.\n",
            tid, pkt->getAddr());

    // Mark this packet as completed in the cache request (this also stores the packet)
    bool found_packet = threads[tid].cacheReq.markCompletedAndStorePacket(pkt);
    if (!found_packet) {
        DPRINTF(Fetch, "[tid:%i] Packet doesn't match current requests, deleting pkt %#lx\n",
                tid, pkt->getAddr());
        DPRINTF(Fetch, "[tid:%i] Expected requests: ", tid);
        for (size_t i = 0; i < threads[tid].cacheReq.requests.size(); i++) {
            DPRINTF(Fetch, "req[%d]=0x%lx ", i, threads[tid].cacheReq.requests[i]->getVaddr());
        }
        DPRINTF(Fetch, "\n");
        delete pkt;
        return false;
    }

    if (isTraceMode() && pkt->req->hasXsMetadata()) {
        auto meta = pkt->req->getXsMetadata();
        meta.traceL1iVisibilityTick = curTick();
        pkt->req->setXsMetadata(meta);
        auto reason = static_cast<
            branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason>(
                meta.traceDemandOutcome);
        if (reason == branch_prediction::btb_pred::BtbpTraceEvent::
                TerminalReason::Unknown) {
            reason = branch_prediction::btb_pred::BtbpTraceEvent::
                TerminalReason::Fill;
        }
        closeTraceDemand(tid, pkt->req, reason);
    }

    DPRINTF(Fetch, "[tid:%i] Packet successfully matched and stored. Current status: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

    // Check if we're still waiting for other packets
    if (!threads[tid].cacheReq.allCompleted()) {
        DPRINTF(Fetch, "[tid:%i] Waiting for remaining packets. Completed: %d, Total: %d\n",
                tid, threads[tid].cacheReq.completedPackets, threads[tid].cacheReq.packets.size());

        bool waitingOnRetry = false;
        for (const auto status : threads[tid].cacheReq.requestStatus) {
            if (status == CacheWaitRetry) {
                waitingOnRetry = true;
                break;
            }
        }

        if (waitingOnRetry && cacheBlocked && !retryPkt.empty()) {
            PacketPtr queuedPkt = retryPkt.front();
            const ThreadID queuedTid =
                cpu->contextToThread(queuedPkt->req->contextId());
            const bool sameThreadRetry = queuedTid == tid &&
                threads[tid].cacheReq.findRequestIndex(queuedPkt->req) != SIZE_MAX;

            if (sameThreadRetry) {
                beginTraceDemandAttempt(tid, queuedPkt->req);
            }
            if (sameThreadRetry && icachePort.sendTimingReq(queuedPkt)) {
                DPRINTF(Fetch,
                        "[tid:%i] Retrying matching queued I-cache packet %#lx "
                        "after sibling response\n",
                        tid, queuedPkt->req->getVaddr());
                updateCacheRequestStatusByRequest(tid, queuedPkt->req,
                                                  CacheWaitResponse);
                ppFetchRequestSent->notify(queuedPkt->req);
                retryPkt.erase(retryPkt.begin());
                if (retryPkt.empty()) {
                    cacheBlocked = false;
                }
            }
        }

        return false;  // Return false to indicate we're still waiting
    }

    // All packets have arrived - merge them directly into fetchBuffer
    DPRINTF(Fetch, "[tid:%i] All packets arrived, merging data into fetchBuffer.\n", tid);

    // Find the packets by request number
    PacketPtr firstPkt = nullptr;
    PacketPtr secondPkt = nullptr;

    for (size_t i = 0; i < threads[tid].cacheReq.packets.size(); i++) {
        if (threads[tid].cacheReq.requests[i]->getReqNum() == 1) {
            firstPkt = threads[tid].cacheReq.packets[i];
        } else if (threads[tid].cacheReq.requests[i]->getReqNum() == 2) {
            secondPkt = threads[tid].cacheReq.packets[i];
        }
    }

    assert(firstPkt && secondPkt);

    // Copy merged data directly into fetchBuffer
    memcpy(threads[tid].data, firstPkt->getConstPtr<uint8_t>(), firstPkt->getSize());
    memcpy(threads[tid].data + firstPkt->getSize(), secondPkt->getConstPtr<uint8_t>(), secondPkt->getSize());
    threads[tid].usedForTwoFetch = false;
    threads[tid].valid = true;

    if (isTraceMode()) {
        using TerminalReason = branch_prediction::btb_pred::BtbpTraceEvent::
            TerminalReason;
        TerminalReason request_reason = TerminalReason::CacheHit;
        for (const auto &req : threads[tid].cacheReq.requests) {
            if (!req || !req->hasXsMetadata()) {
                request_reason = TerminalReason::Fill;
                break;
            }
            const auto reason = static_cast<TerminalReason>(
                req->getXsMetadata().traceDemandOutcome);
            if (reason == TerminalReason::MergeWithPrefetch) {
                request_reason = reason;
                break;
            }
            if (reason == TerminalReason::MergeWithDemand) {
                request_reason = reason;
            } else if (reason != TerminalReason::CacheHit &&
                       request_reason == TerminalReason::CacheHit) {
                request_reason = TerminalReason::Fill;
            }
        }
        closeTraceFetchRequest(tid, request_reason);
    }

    // Clean up the packets
    delete firstPkt;
    delete secondPkt;

    DPRINTF(Fetch, "[tid:%i] Dual cacheline fetch completion processed successfully.\n", tid);
    return true;
}

void
Fetch::processCacheCompletion(PacketPtr pkt)
{
    if (pkt->req->isPrefetch() && pkt->req->getPFSource() == PF_EIP) {
        processEipCompletion(pkt);
        return;
    }

    if (pkt->req->isPrefetch() && pkt->req->getPFSource() == PF_FDIP) {
        processFdipCompletion(pkt);
        return;
    }

    ThreadID tid = cpu->contextToThread(pkt->req->contextId());
    assert(pkt->req->isMisalignedFetch() && "Only multi-cacheline fetch is supported");

    bool allCompleted = processMultiCacheLineCompletion(tid, pkt);
    // If we're still waiting for another packet, return early
    if (!allCompleted) {
        return;
    }

    // Check if this completion should be processed
    // Either thread is waiting for cache, or cache just completed
    CacheRequestStatus cacheStatus = threads[tid].cacheReq.getOverallStatus();
    if (!hasPendingCacheRequests(tid) && cacheStatus != AccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Thread not waiting for cache and no completion, ignoring\n", tid);
        ++fetchStats.icacheSquashes;
        return;
    }

    // Data has been merged into fetchBuffer, we can proceed
    DPRINTF(Fetch, "[tid:%i] All misaligned packets received and merged.\n", tid);

    assert(!cpu->switchedOut());

    // Trace 按需消费：不在 icache 完成时写入 trace 指令码，避免批量消费。
    if (isTraceMode()) {
        DPRINTF(TraceReader,
                "[TRACE] Icache completion: keep timing only; no trace bytes injection\n");
    }

    // Verify fetchBufferPC alignment with the supplying FSQ entry.
    if (threads[tid].valid && dbpbtb->ftqHasFetching(tid)) {
        const auto prediction = dbpbtb->ftqFetchBlock(tid);
        if (threads[tid].startPC != prediction.startPC) {
            panic("fetchBufferPC %#x should be aligned with FSQ startPC %#x",
                  threads[tid].startPC, prediction.startPC);
        }
    }

    // Wake up the CPU (if it went to sleep and was waiting on
    // this completion event).
    cpu->wakeCPU();

    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache completion\n",
            tid);

    switchToActive();

    // Transition from WaitingCache back to Running when cache access completes
    setThreadStatus(tid, Running);
}

void
Fetch::drainResume()
{
}

void
Fetch::drainSanityCheck() const
{
    assert(isDrained());
    assert(retryPkt.size() == 0);
    assert(!cacheBlocked);
    assert(!interruptPending);

    for (ThreadID i = 0; i < numThreads; ++i) {
        assert(threads[i].cacheReq.packets.empty());
        assert(fetchStatus[i] == Idle);
    }

    branchPred->drainSanityCheck();
}

bool
Fetch::isDrained() const
{
    /* Make sure that threads are either idle of that the commit stage
     * has signaled that draining has completed by setting the drain
     * stall flag. This effectively forces the pipeline to be disabled
     * until the whole system is drained (simulation may continue to
     * drain other components).
     */
    for (ThreadID i = 0; i < numThreads; ++i) {
        // Verify fetch queues are drained
        if (!fetchQueue[i].empty())
            return false;

        // Return false if not idle or drain stalled
        if (fetchStatus[i] != Idle) {
            return false;
        }
    }

    /* The pipeline might start up again in the middle of the drain
     * cycle if the finish translation event is scheduled, so make
     * sure that's not the case.
     */
    return std::none_of(
        finishTranslationEvents.begin(), finishTranslationEvents.end(),
        [](const auto &event) { return event->scheduled(); });
}

void
Fetch::takeOverFrom()
{
    assert(cpu->getInstPort().isConnected());
    resetStage();

}

void
Fetch::drainStall(ThreadID tid)
{
}

void
Fetch::wakeFromQuiesce()
{
    DPRINTF(Fetch, "Waking up from quiesce\n");
    // Hopefully this is safe
    // @todo: Allow other threads to wake from quiesce.
    setThreadStatus(0, Running);
}

void
Fetch::switchToActive()
{
    if (_status == Inactive) {
        DPRINTF(Activity, "Activating stage.\n");

        cpu->activateStage(CPU::FetchIdx);

        _status = Active;
    }
}

void
Fetch::switchToInactive()
{
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);

        _status = Inactive;
    }
}

void
Fetch::deactivateThread(ThreadID tid)
{
    // Update priority list
    auto thread_it = std::find(priorityList.begin(), priorityList.end(), tid);
    if (thread_it != priorityList.end()) {
        priorityList.erase(thread_it);
    }
}

bool
Fetch::lookupAndUpdateNextPC(const DynInstPtr &inst, PCStateBase &next_pc,
                             bool allow_two_fetch,
                             bool &continued_to_next_target,
                             bool &false_hit)
{
    // Do branch prediction check here.
    // A bit of a misnomer...next_PC is actually the current PC until
    // this function updates it.
    bool predict_taken = false;
    continued_to_next_target = false;
    false_hit = false;

    // Decoupled+BTB-only: compute next PC directly from the supplying FSQ entry.
    ThreadID tid = inst->threadNumber;
    assert(dbpbtb);
    assert(dbpbtb->ftqHasFetching(tid));
    const auto prediction = dbpbtb->ftqFetchBlock(tid);

    const Addr curr_pc = next_pc.instAddr();
    assert(prediction.startPC <= curr_pc && curr_pc < prediction.endPC);

    // D4-A R3 statistics-only snapshot: whether this instruction's PC hit
    // an entry of the supplying FetchTarget's predBTBEntries at prediction
    // time (stricter than predict_taken: a hit entry whose conditional
    // branch is direction-predicted not-taken still counts as a hit).  The
    // only readers are Decode's selfSquash BTB-miss classification and
    // Fetch::handleDecodeSquash's redirect-window decision; no timing,
    // arbitration, supply or prediction logic ever reads it.
    const bool pred_btb_hit =
        tdstats::predBtbHitSnapshot(curr_pc, stream.predBTBEntries);
    inst->setPredBtbHit(pred_btb_hit);

    bool run_out = false;

    // Taken when the current PC matches the predicted control PC.
    predict_taken = prediction.taken && (curr_pc == prediction.controlPC);
    if (predict_taken) {
        auto &rpc = next_pc.as<GenericISA::PCStateWithNext>();
        rpc.pc(prediction.target);
        rpc.npc(prediction.target + 4);
        rpc.uReset();
        run_out = true;
    } else if (inst->staticInst->isMicroop()) {
        // Microops must advance uPC explicitly; they do not rely on decoder NPC.
        inst->staticInst->advancePC(next_pc);
        run_out = next_pc.instAddr() >= prediction.endPC;
    } else {
        // Sequential fetch: decoder already computed npc with correct inst size.
        auto &rpc = next_pc.as<RiscvISA::PCState>();
        const Addr fall_thru = rpc.npc();
        rpc.pc(fall_thru);
        // Placeholder; decoder will overwrite npc on the next decode.
        rpc.npc(fall_thru + 4);
        rpc.uReset();
        run_out = fall_thru >= prediction.endPC;
    }

    // Track how many dynamic instructions were fetched for this (legacy) FTQ/FSQ entry.
    ftqEntryFetchedInsts[tid]++;
    false_hit = run_out && prediction.taken && !predict_taken;
    if (false_hit) {
        DPRINTF(DecoupleBP,
                "False BTB hit at FTQ %lu: stream [%#lx, %#lx) "
                "predicted control %#lx -> %#lx, fetched through %#lx; "
                "redirect to fall-through %s\n",
                prediction.ftqId, prediction.startPC, prediction.endPC,
                prediction.controlPC, prediction.target,
                curr_pc, next_pc);
        dbpbtb->nonControlSquash(prediction.ftqId, next_pc,
                                 inst->seqNum, tid, currentLoopIter);
        ftqEntryFetchedInsts[tid] = 0;
        threads[tid].valid = false;
    } else if (run_out) {
        if (enableTwoFetch && !isTraceMode() &&
            allow_two_fetch && predict_taken) {
            ++fetchStats.twoFetchAttempts;

            if (dbpbtb->ftqHasNext(tid)) {
                const auto next_prediction = dbpbtb->ftqFetchBlock(tid, 1);
                const bool target_matches =
                    next_pc.instAddr() == next_prediction.startPC;
                const bool valid_range =
                    next_prediction.startPC >= prediction.startPC &&
                    next_prediction.endPC >= next_prediction.startPC;
                const bool fits_window = valid_range &&
                    next_prediction.endPC - prediction.startPC <=
                        twoFetchMaxBytes;
                const bool has_fetch_capacity =
                    numInst < fetchWidth &&
                    fetchQueue[tid].size() < fetchQueueSize;
                const bool can_cross_boundary =
                    !redirectPending[tid] &&
                    (!checkInterrupt(next_pc.instAddr()) ||
                     delayedCommit[tid]);

                continued_to_next_target = target_matches && fits_window &&
                    has_fetch_capacity && can_cross_boundary;
            }
        }

        dbpbtb->consumeFetchTarget(ftqEntryFetchedInsts[tid], tid);
        ftqEntryFetchedInsts[tid] = 0;
        if (continued_to_next_target) {
            threads[tid].usedForTwoFetch = true;
            ++fetchStats.twoFetchSuccesses;
            DPRINTF(DecoupleBP,
                    "2Fetch: continue with FTQ %lu in the current buffer.\n",
                    dbpbtb->ftqFetchBlock(tid).ftqId);
        } else {
            threads[tid].valid = false;
            DPRINTF(DecoupleBP, "Used up fetch targets.\n");
        }
    }

    inst->setLoopIteration(currentLoopIter);

    // For decoupled frontend, the instruction type is predicted with BTB
    if (!predict_taken) {
        inst->setPredTarg(next_pc);
        inst->setPredTaken(false);
        return false;
    }

    DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x predicted to be taken to %s\n",
            tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
            "predicted to go to %s\n",
            tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    inst->setPredTarg(next_pc);
    inst->setPredTaken(predict_taken);

    ++fetchStats.branches;

    if (predict_taken) {
        ++fetchStats.predictedBranches;
    }

    return predict_taken;
}

bool
Fetch::fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc)
{
    assert(!cpu->switchedOut());

    // Check for blocking conditions
    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, cache blocked\n", tid);
        setAllFetchStalls(StallReason::IcacheStall);
        return false;
    } else if (checkInterrupt(pc) && !delayedCommit[tid]) {
        // Hold off fetch from getting new instructions when:
        // Cache is blocked, or
        // while an interrupt is pending and we're not in PAL mode, or
        // fetch is switched out.
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, interrupt pending\n", tid);
        setAllFetchStalls(StallReason::IntStall);
        return false;
    }

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for addr %#x, pc=%#lx\n",
            tid, vaddr, vaddr, pc);

    // With 66-byte fetchBufferSize, we always need to access 2 cache lines
    return handleMultiCacheLineFetch(vaddr, tid, pc);
}

bool
Fetch::validateTranslationRequest(ThreadID tid, const RequestPtr &mem_req)
{
    // Check if this request belongs to current cache request
    bool isExpectedReq = false;
    for (size_t i = 0; i < threads[tid].cacheReq.requests.size(); i++) {
        if (mem_req == threads[tid].cacheReq.requests[i]) {
            isExpectedReq = true;
            break;
        }
    }

    // Check if request should be processed using new state system
    if (!isExpectedReq || !hasPendingCacheRequests(tid)) {
        DPRINTF(Fetch, "[tid:%i] Ignoring translation completed after squash or unexpected request\n", tid);
        DPRINTF(Fetch, "[tid:%i] Ignoring req addr=%#lx\n", tid, mem_req->getVaddr());
        ++fetchStats.tlbSquashes;
        return false;
    }

    return true;
}

void
Fetch::handleSuccessfulTranslation(ThreadID tid, const RequestPtr &mem_req, Addr fetchPC)
{
    // Check that we're not going off into random memory
    if (!cpu->system->isMemAddr(mem_req->getPaddr())) {
        DPRINTF(Fetch, "Address %#x is outside of physical memory, stopping fetch, %lu\n",
                mem_req->getPaddr(), curTick());

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, AccessFailed);
        setAllFetchStalls(StallReason::OtherFetchStall);
        // Note: Don't reset here, let the caller handle cleanup based on overall status
        return;
    }

    // Build packet here.
    PacketPtr data_pkt = new Packet(mem_req, MemCmd::ReadReq);
    data_pkt->dataDynamic(new uint8_t[fetchBufferSize]);
    // All requests are multi-cacheline, always set send right away
    data_pkt->setSendRightAway();

    DPRINTF(Fetch, "[tid:%i] Fetching data for addr %#x, pc=%#lx\n",
                tid, mem_req->getVaddr(), fetchPC);

    threads[tid].startPC = fetchPC;
    threads[tid].valid = false;
    DPRINTF(Fetch, "Fetch: Doing instruction read.\n");

    fetchStats.cacheLines++;

    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] I-cache port already waiting for retry, queueing %#lx\n",
                tid, mem_req->getVaddr());

        updateCacheRequestStatusByRequest(tid, mem_req, CacheWaitRetry);
        setAllFetchStalls(StallReason::IcacheStall);
        retryPkt.push_back(data_pkt);
        return;
    }

    // Access the cache. Each sendTimingReq call is one attempt of the same
    // logical demand; retries keep the request and demand UIDs.
    beginTraceDemandAttempt(tid, mem_req);
    if (!icachePort.sendTimingReq(data_pkt)) {
        DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, CacheWaitRetry);
        data_pkt->setRetriedPkt();
        DPRINTF(Fetch, "[tid:%i] mem_req.addr=%#lx needs retry.\n", tid,
                mem_req->getVaddr());
        setAllFetchStalls(StallReason::IcacheStall);
        retryPkt.push_back(data_pkt);
        cacheBlocked = true;
    } else {
        DPRINTF(Fetch, "[tid:%i] Doing Icache access.\n", tid);
        DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache response.\n", tid);
        lastIcacheStall[tid] = curTick();

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, CacheWaitResponse);
        setAllFetchStalls(StallReason::IcacheStall);
        // Notify Fetch Request probe when a packet containing a fetch request is successfully sent
        ppFetchRequestSent->notify(mem_req);
    }
}

void
Fetch::handleTranslationFault(ThreadID tid, const RequestPtr &mem_req, const Fault &fault)
{
    DPRINTF(FetchFault, "fault, mem_req.addr=%#lx\n", mem_req->getVaddr());

    // Don't send an instruction to decode if we can't handle it.
    if (!(numInst < fetchWidth) || !(fetchQueue[tid].size() < fetchQueueSize)) {
        auto &finish_event = *finishTranslationEvents[tid];
        if (finish_event.scheduled() && finish_event.getReq() != mem_req) {
            DPRINTF(FetchFault,
                    "fault, finish_event.getReq().addr=%#lx, "
                    "mem_req.addr=%#lx\n",
                    finish_event.getReq()->getVaddr(), mem_req->getVaddr());
            return;
        }
        assert(!finish_event.scheduled());
        finish_event.setFault(fault);
        finish_event.setReq(mem_req);
        cpu->schedule(finish_event, cpu->clockEdge(Cycles(1)));
        return;
    }

    DPRINTF(Fetch, "[tid:%i] Got back req with addr %#x but expected base addr %#x\n",
            tid, mem_req->getVaddr(), threads[tid].cacheReq.baseAddr);

    // Update new cache request status system
    updateCacheRequestStatusByRequest(tid, mem_req, AccessFailed);

    // Translation faulted, icache request won't be sent.
    if (isTraceMode()) {
        using TerminalReason = branch_prediction::btb_pred::BtbpTraceEvent::
            TerminalReason;
        for (const auto &req : threads[tid].cacheReq.requests) {
            closeTraceDemand(
                tid, req, req == mem_req ? TerminalReason::TranslationFault :
                                            TerminalReason::Dropped);
        }
        closeTraceFetchRequest(tid, TerminalReason::TranslationFault);
    }
    threads[tid].cacheReq.reset();
    pendingTraceInstructionOrdinal[tid] = 0;
    pendingTraceInstructionPc[tid] = 0;
    pendingTracePathWrong[tid] = false;
    pendingTraceSupplyValid[tid] = false;

    // Send the fault to commit.  This thread will not do anything
    // until commit handles the fault.  The only other way it can
    // wake up is if a squash comes along and changes the PC.
    const PCStateBase &fetch_pc = *threads[tid].fetchpc;

    DPRINTF(Fetch, "[tid:%i] Translation faulted, building noop.\n", tid);
    // We will use a nop in order to carry the fault.
    DynInstPtr instruction = buildInst(tid, nopStaticInstPtr, nullptr,
            fetch_pc, fetch_pc, false);
    instruction->setVersion(localSquashVer[tid]);
    instruction->setNotAnInst();

    instruction->setPredTarg(fetch_pc);
    instruction->fault = fault;
    std::unique_ptr<PCStateBase> next_pc(fetch_pc.clone());
    instruction->staticInst->advancePC(*next_pc);
    set(instruction->predPC, next_pc);

    wroteToTimeBuffer = true;

    DPRINTF(Activity, "Activity this cycle.\n");
    cpu->activityThisCycle();

    setThreadStatus(tid, TrapPending);
    setAllFetchStalls(StallReason::TrapStall);

    DPRINTF(Fetch, "[tid:%i] Blocked, need to handle the trap.\n", tid);
    DPRINTF(Fetch, "[tid:%i] fault (%s) detected @ PC %s.\n",
            tid, fault->name(), *threads[tid].fetchpc);
}

void
Fetch::finishTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());

    // For multi-cacheline fetch, use the stored base address
    // Both requests should use the same fetchBufferPC
    Addr fetchPC = threads[tid].cacheReq.baseAddr;

    assert(!cpu->switchedOut());

    // Wake up CPU if it was idle
    cpu->wakeCPU();

    DPRINTF(Fetch, "[tid:%i] Translation completed for addr %#lx\n",
            tid, mem_req->getVaddr());

    // Validate if this request should be processed
    if (!validateTranslationRequest(tid, mem_req)) {
        return;
    }

    // Handle translation result
    if (fault == NoFault) {
        handleSuccessfulTranslation(tid, mem_req, fetchPC);
    } else {
        handleTranslationFault(tid, mem_req, fault);
    }

    _status = updateFetchStatus();
}

void
Fetch::doSquash(PCStateBase &new_pc, const DynInstPtr squashInst, const InstSeqNum seqNum,
        ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing, setting PC to: %s. seqNum: %lu\n",
            tid, new_pc, seqNum);
    squashResolveQueue(tid, seqNum);
    if (squashInst) {
        DPRINTF(Fetch, "[tid:%i] Squash caused by inst at PC: %s, seqNum: %lu\n",
                tid, squashInst->pcState(), squashInst->seqNum);
    }

    if (eipEnabled()) {
        uint64_t demand_id = 0;
        Addr squash_pc = new_pc.instAddr();
        if (squashInst && squashInst->xsMeta) {
            demand_id = squashInst->xsMeta->eipDemandId;
            squash_pc = squashInst->pcState().instAddr();
        }
        if (demand_id == 0) {
            InstSeqNum newest_seq = 0;
            for (const auto &inst : cpu->instList) {
                if (inst->threadNumber != tid || inst->isSquashed() ||
                    inst->seqNum > seqNum || !inst->xsMeta ||
                    inst->xsMeta->eipDemandId == 0 ||
                    inst->seqNum < newest_seq) {
                    continue;
                }
                newest_seq = inst->seqNum;
                demand_id = inst->xsMeta->eipDemandId;
                squash_pc = inst->pcState().instAddr();
            }
        }
        if (demand_id != 0) {
            EntanglingPrefetcher::SquashEvent event;
            event.virtualLine = squash_pc >> 6;
            event.instructionId = demand_id;
            event.cycle = static_cast<uint64_t>(cpu->curCycle());
            eipPrefetcher->observeSquash(event);
            ++fetchStats.eipSquashEvents;
        }
    }

    // restore vtype
    uint8_t restored_vtype = cpu->readMiscReg(RiscvISA::MISCREG_VTYPE, tid);
    for (auto& it : cpu->instList) {
        if (!it->isSquashed() &&
            it->seqNum <= seqNum &&
            it->staticInst->isVectorConfig()) {
            auto vset = static_cast<RiscvISA::VConfOp*>(it->staticInst.get());
            if (vset->vtypeIsImm) {
                restored_vtype = vset->earlyVtype;
            }
        }
    }
    decoder[tid]->as<RiscvISA::Decoder>().setVtype(restored_vtype);

    // align PC to 2 bytes
    // This handles cases where PC might be odd due to speculative execution,
    // but no need to throw INST_ADDR_MISALIGNED fault here
    if (new_pc.instAddr() % 2 != 0) {
        // Modify new_pc directly to make it 2-byte aligned
        auto& riscv_pc = new_pc.as<RiscvISA::PCState>();
        riscv_pc.set(new_pc.instAddr() & ~1);
        set(threads[tid].fetchpc, new_pc);
        DPRINTF(Fetch, "[tid:%i] pc is misaligned, aligned to %#lx\n", tid, new_pc.instAddr());
    } else {
        set(threads[tid].fetchpc, new_pc);
    }
    if (squashInst && squashInst->pcState().instAddr() == new_pc.instAddr())
        macroop[tid] = squashInst->macroop;
    else
        macroop[tid] = NULL;
    decoder[tid]->reset();
    clearRedirectPending(tid);

    // Clear the icache miss if it's outstanding.
    DPRINTF(Fetch, "[tid:%i] Squash: clear cacheReq, current fetchStatus[tid]=%d\n", tid, fetchStatus[tid]);

    // Cancel all active cache requests in new status system
    closeOutstandingTraceFetch(
        tid,
        branch_prediction::btb_pred::BtbpTraceEvent::TerminalReason::Squash);
    threads[tid].cacheReq.cancelAllRequests();
    DPRINTF(Fetch, "[tid:%i] Squash: cancelled all cache requests, status: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

    // Reset the cache request after cancelling
    threads[tid].cacheReq.reset();
    pendingTraceInstructionOrdinal[tid] = 0;
    pendingTraceInstructionPc[tid] = 0;
    pendingTracePathWrong[tid] = false;
    pendingTraceSupplyValid[tid] = false;
    ++fdipEpoch[tid];
    resetFdipPartialState(tid);

    // Drop any retry packets that belong to this squashed thread.
    for (auto it = retryPkt.begin(); it != retryPkt.end();) {
        if (cpu->contextToThread((*it)->req->contextId()) == tid) {
            delete *it;
            it = retryPkt.erase(it);
        } else {
            ++it;
        }
    }
    if (retryPkt.empty()) {
        cacheBlocked = false;
    }

    if (squashInst && !squashInst->isControl()) {
        // csrrw satp need to flush all fetch targets
        threads[tid].valid = false;
    }

    setThreadStatus(tid, Squashing);
    setAllFetchStalls(StallReason::BpStall); // may caused by other stages like load and store

    // Empty fetch queue
    // D4-A R5: count the instructions dropped by this squash clear
    // (statistics-only; the resetStage/clearStates queue clears are NOT
    // squashes and are never counted here).
    fetchStats.fetchQueueSquashedInsts += fetchQueue[tid].size();
    fetchQueue[tid].clear();
    deferVsetvlDecode[tid] = true;
    postSquashFetchBatchSize[tid] = 0;
    clearPredecodePipeline(tid);


    // microops are being squashed, it is not known wheather the
    // youngest non-squashed microop was  marked delayed commit
    // or not. Setting the flag to true ensures that the
    // interrupts are not handled when they cannot be, though
    // some opportunities to handle interrupts may be missed.
    delayedCommit[tid] = true;

    // Force a new I-cache request for the next FTQ head after squash.
    threads[tid].valid = false;
    ftqEntryFetchedInsts[tid] = 0;

    if (traceFetch) {
        traceFetch->handleTraceSquash(tid, new_pc, squashInst, seqNum);
    }

    ++fetchStats.squashCycles;
}

void
Fetch::flushFetchBuffer()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        threads[i].valid = false;
    }
}

Addr
Fetch::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    assert(dbpbtb);
    return dbpbtb->getPreservedReturnAddr(dynInst);
}

void
Fetch::squashFromDecode(PCStateBase &new_pc, const DynInstPtr squashInst,
        const InstSeqNum seq_num, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing from decode.\n", tid);

    doSquash(new_pc, squashInst, seq_num, tid);

    // Tell the CPU to remove any instructions that are in flight between
    // fetch and decode.
    cpu->removeInstsUntil(seq_num, tid);
}

Fetch::FetchStatus
Fetch::updateFetchStatus()
{
    //Check Running
    std::list<ThreadID>::iterator act_tid = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (act_tid != end) {
        ThreadID tid = *act_tid++;

        if (canFetchInstructions(tid) || fetchStatus[tid] == Squashing ||
            threads[tid].cacheReq.getOverallStatus() == AccessComplete) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);

                if (threads[tid].cacheReq.getOverallStatus() == AccessComplete) {
                    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache"
                            "completion\n",tid);
                }

                cpu->activateStage(CPU::FetchIdx);
            }

            return Active;
        }
    }

    // Stage is switching from active to inactive, notify CPU of it.
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);
    }

    return Inactive;
}

void
Fetch::squash(PCStateBase &new_pc, const InstSeqNum seq_num,
        DynInstPtr squashInst, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squash from commit.\n", tid);

    doSquash(new_pc, squashInst, seq_num, tid);
    assert(new_pc.instAddr() % 2 == 0 && "squash PC should be 2-byte aligned");

    // Tell the CPU to remove any instructions that are not in the ROB.
    cpu->removeInstsNotInROB(tid);
}

void
Fetch::tick()
{
    // Initialize state for this tick cycle
    bool status_change = initializeTickState();

    // Simple decoupled+BTB ordering:
    // - first consume incoming squashes/redirects (in initializeTickState())
    // - then advance predictor pipeline + try to supply an FTQ head
    // - then run fetch using the supplied FTQ entry (if any)
    assert(dbpbtb);
    dbpbtb->tick();

    // Perform fetch operations and instruction delivery
    fetchAndProcessInstructions(status_change);
    runFdip();
    runEip();

    if (btbpRoiDrainMode && !btbpRoiDrainExitRequested &&
        curTick() > btbpRoiDrainBeginTick &&
        instPrefetchOutstandingLines == 0) {
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            closeOutstandingTraceFetch(
                tid, branch_prediction::btb_pred::BtbpTraceEvent::
                         TerminalReason::RetainedAtDrain);
            dbpbtb->closeTraceLookupsAtRoiEnd(tid);
        }
        const unsigned closed = fdipIcacheAccessor ?
            fdipIcacheAccessor->closeBtbpRoiResidencies() : 0;
        inform("BTBP ROI_DRAIN_CLOSED_RESIDENCIES count=%u", closed);
        btbpRoiDrainExitRequested = true;
        exitSimLoop("BTBP ROI drain complete");
    }
}

bool
Fetch::initializeTickState()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    bool status_change = false;

    wroteToTimeBuffer = false;
    setAllFetchStalls(StallReason::NoStall);

    // D4-B d4bDiag evidence (statistics-only; reset at tick start).
    ledgerDeliveredToDecode_ = 0;
    ledgerRobSquashingAtFetch_ = false;

    // get the distribution of fetch status
    fetchStats.fetchStatusDist[fetchStatus[0]]++;

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (!redirectPending[tid]) {
            continue;
        }
        if (redirectPendingCycles[tid] == 0) {
            clearRedirectPending(tid);
        } else {
            --redirectPendingCycles[tid];
        }
    }

    // === Block Policy: consume IEW long-latency signals ===
    checkLongLatencyLoads();

    // Check signal updates for all active threads
    while (threads != end) {
        ThreadID tid = *threads++;

        // Check the signals for each thread to determine the proper status
        // for each thread.
        bool updated_status = checkSignalsAndUpdate(tid);
        status_change =  status_change || updated_status;
        if (fromCommit->commitInfo[tid].emptyROB) {
            waitForVsetvl[tid] = false;
        }
        // D4-B d4bDiag evidence: the fetch-side view of robSquashing this
        // tick (lagged wire; H1/H4 overlap diagnostics).
        ledgerRobSquashingAtFetch_ = ledgerRobSquashingAtFetch_ ||
            fromCommit->commitInfo[tid].robSquashing;
    }

    handleIEWSignals();

    DPRINTF(Fetch, "Running stage.\n");
    return status_change;
}

void
Fetch::fetchAndProcessInstructions(bool status_change)
{
    advancePredecodePipeline();

    // Fetch instructions from active threads
    for (threadFetched = 0; threadFetched < numFetchingThreads;
         threadFetched++) {
        // Fetch each of the actively fetching threads.
        fetch(status_change);
    }

    // Pass stall reasons to decode stage
    toDecode->fetchStallReason = stallReason;

    // Record number of instructions fetched this cycle for distribution.
    fetchStats.nisnDist.sample(numInst);

    if (status_change) {
        // Change the fetch stage status if there was a status change.
        _status = updateFetchStatus();
    }

    // Handle interrupt processing in full system mode
    handleInterrupts();

    // Send instructions to decode stage, update stall reasons and measure frontend bubbles.
    sendInstructionsToDecode();
}

void
Fetch::handleInterrupts()
{
    if (FullSystem) {
        if (fromCommit->commitInfo[0].interruptPending) {
            DPRINTF(Fetch, "Set interrupt pending.\n");
            interruptPending = true;
        }

        if (fromCommit->commitInfo[0].clearInterrupt) {
            DPRINTF(Fetch, "Clear interrupt pending.\n");
            interruptPending = false;
        }
    }
}

ThreadID
Fetch::selectUnstalledThread()
{
    ThreadID selected = InvalidThreadID;
    bool has_candidate = false;
    bool has_unthrottled_candidate = false;
    bool candidate[MaxThreads];
    bool throttled[MaxThreads];

    // update smtBorrowThrottleCycles and check whether has candidate
    const bool block_active = isBlockPolicyActive();

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        candidate[tid] = true;
        throttled[tid] = false;

        // update throttle status
        //
        // Policy differences:
        //   BlockStallPolicy (Hold):
        //     - throttle_now uses BASE throttle (ROB/IQ full, memory pressure)
        //     - block is an ADDITIONAL throttle source: when isBlockPolicyActive()
        //       and thread is blocked, throttled[tid] is set directly (OR with base)
        //   BlockThrottlePolicy (Drive):
        //     - throttle_now = threadFetchBlocked[tid] (block REPLACES base throttle)
        //     - the block signal continuously refreshes smtBorrowThrottleCycles hold counter
        //   FlushFromLoadPolicy / FlushFromUsePolicy:
        //     - same throttle behavior as BlockThrottlePolicy (block drives throttle)
        //     - additionally initiates a squash to free pipeline resources
        {
            bool throttle_now = false;
            if (smtFetchBlockPolicy == SMTFetchBlockPolicy::BlockThrottlePolicy) {
                // BlockThrottlePolicy completely replaces base throttle policy
                throttle_now = threadFetchBlocked[tid];
            } else {
                // base throttle policy
                throttle_now =
                    smtHasBorrowThrottleStall(fromIEW->iewInfo[tid]) ||
                    smtHasMemoryPressure(fromIEW->iewInfo[tid], smtLdstqHighWater);
            }
            if (throttle_now) {
                smtBorrowThrottleCycles[tid] = smtBorrowThrottleHoldCycles;
            } else if (smtBorrowThrottleCycles[tid] > 0) {
                --smtBorrowThrottleCycles[tid];
            }
            // BlockStallPolicy and Flush*Policy will directly set throttled[tid]
            if ((smtFetchBlockPolicy == SMTFetchBlockPolicy::BlockStallPolicy ||
                 isFlushFromPolicy()) &&
                block_active && threadFetchBlocked[tid]) {
                throttled[tid] = true;
            }
            throttled[tid] |= smtBorrowThrottleCycles[tid] > 0;
        }

        // thread can not candidate has lowest priv, should not be chosen
        if (stallSig->blockFetch[tid] || fetchQueue[tid].empty()) {
            smtBorrowThrottleCycles[tid] = 0;
            lsqCounter->setCounter(tid, UINT64_MAX);
            iqCounter->setCounter(tid, UINT64_MAX);
            robCounter->setCounter(tid, UINT64_MAX);
            candidate[tid] = false;
            continue;
        }
        // flush policy: should not pipedown insts
        if (isFlushFromPolicy() && block_active && threadFetchBlocked[tid]) {
            smtBorrowThrottleCycles[tid] = 0;
            lsqCounter->setCounter(tid, UINT64_MAX);
            iqCounter->setCounter(tid, UINT64_MAX);
            robCounter->setCounter(tid, UINT64_MAX);
            candidate[tid] = false;
            continue;
        }
        has_candidate = true;
    }
    if (has_candidate) {
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            if (!candidate[tid]) continue;
            lsqCounter->setCounter(tid, fromIEW->iewInfo[tid].ldstqCount);
            iqCounter->setCounter(tid, fromIEW->iewInfo[tid].iqCount);
            robCounter->setCounter(tid, fromIEW->iewInfo[tid].robCount);
            if (throttled[tid]) {
                lsqCounter->setCounter(tid, UINT64_MAX - 1);
                iqCounter->setCounter(tid, UINT64_MAX - 1);
                robCounter->setCounter(tid, UINT64_MAX - 1);
            } else {
                has_unthrottled_candidate = true;
            }
            DPRINTF(Fetch,
                    "[tid:%i] block=%u mem_pressure=%u hold=%u throttled=%u lsq=%u iq=%u rob=%u\n",
                    tid, threadFetchBlocked[tid],
                    smtHasMemoryPressure(fromIEW->iewInfo[tid], smtLdstqHighWater),
                    smtBorrowThrottleCycles[tid], throttled[tid], fromIEW->iewInfo[tid].ldstqCount,
                    fromIEW->iewInfo[tid].iqCount, fromIEW->iewInfo[tid].robCount);
        }
        // while both threads are throttled, select policy as both threads are not throttle
        if (!has_unthrottled_candidate) {
            for (ThreadID tid = 0; tid < numThreads; ++tid) {
                if (!candidate[tid]) continue;
                lsqCounter->setCounter(tid, fromIEW->iewInfo[tid].ldstqCount);
                iqCounter->setCounter(tid, fromIEW->iewInfo[tid].iqCount);
                robCounter->setCounter(tid, fromIEW->iewInfo[tid].robCount);
            }
        }
        int throttleState = 0;
        for (ThreadID tid = numThreads - 1; tid >= 0; tid--) {
            throttleState = (throttleState << 1) + (int)(!candidate[tid] || throttled[tid]);
        }
        fetchStats.fetchThrottleState[throttleState]++;
        selected = decodeScheduler->getThread();
    } else {
        DPRINTF(Fetch, "No candidate thread at this tick!\n");
    }
    return selected;
}

bool
Fetch::isBlockPolicyActive() const
{
    if (smtFetchBlockPolicy != SMTFetchBlockPolicy::BlockStallPolicy &&
        smtFetchBlockPolicy != SMTFetchBlockPolicy::BlockThrottlePolicy &&
        smtFetchBlockPolicy != SMTFetchBlockPolicy::FlushFromLoadPolicy &&
        smtFetchBlockPolicy != SMTFetchBlockPolicy::FlushFromUsePolicy) {
        return false;
    }
    int blocked_count = 0;
    int unblocked_count = 0;

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (threadFetchBlocked[tid])
            blocked_count++;
        else
            unblocked_count++;
    }

    // Only active when both blocked and unblocked candidates exist
    return (blocked_count > 0 && unblocked_count > 0);
}

void
Fetch::sendInstructionsToDecode()
{

    // Reset the number of instructions we've fetched
    numInst = 0;

    bool any_thread_active = false;
    for (int i = 0; i < numThreads; i++) {
        if (!stallSig->blockFetch[i]) {
            any_thread_active = true;
            //break;
        }else{
            fetchStats.smtdecodeStalls[i]++; 
        }

        // === Flush Policy: initiate flush after asymmetric check passes ===
        if (isFlushFromPolicy() && isBlockPolicyActive() &&
            threadFetchBlocked[i] && !flushFromInitiated[i]) {
            InstSeqNum loadSeqNum = iewStage->ldstQueue.getLoadHeadSeqNum(i);
            DynInstPtr loadInst = iewStage->findRobInst(i, loadSeqNum);
            if (loadInst && loadInst->isLoad()) {
                bool fromUse = (smtFetchBlockPolicy == SMTFetchBlockPolicy::FlushFromUsePolicy);
                flushFromInitiateFlush(loadInst, i, fromUse);
            }
        }
    }

    if (!any_thread_active) {
        // All threads are blocked, no instructions to send
        ThreadID blocked_tid = InvalidThreadID;
        for (int i = 0; i < numThreads; i++) {
            if (stallSig->blockFetch[i]) {
                blocked_tid = i;
                break;
            }
        }

        if (blocked_tid != InvalidThreadID) {
            setAllFetchStalls(stallSig->fetchBlockReason[blocked_tid]);
        }

        toDecode->fetchStallReason = stallReason;

        for (int i = 0; i < numThreads; i++) {
            measureFrontendBubbles(0, i);
        }
        fetchStats.decodeThreadsPerCycle.sample(0);
        fetchStats.instsSentToDecodePerCycle.sample(0);
        return;
    }

    const ThreadID primary_tid = selectUnstalledThread();

    if (primary_tid == InvalidThreadID) {
        DPRINTF(Fetch, "All threads are stalled, no thread selected.\n");
        // Reached only when some thread is not backend-blocked
        // (any_thread_active) yet no thread was selected: every unblocked
        // thread has an empty fetch queue.  Account the frontend bubble for
        // the whole cycle before returning (at most one full-width bubble
        // per cycle, never per thread). Upstream's per-thread
        // measureFrontendBubbles(0, i) loop is superseded here by the
        // full-width aggregate to avoid double-charging the same cycle.
        measureFrontendBubblesEmptyQueueCycle();
        fetchStats.decodeThreadsPerCycle.sample(0);
        fetchStats.instsSentToDecodePerCycle.sample(0);
        return;
    }

    std::vector<ThreadID> selected_tids{primary_tid};
    const bool block_policy_active = isBlockPolicyActive();
    for (ThreadID tid = 0;
         tid < numThreads && selected_tids.size() < numPreDispatchThreads;
         ++tid) {
        if (tid != primary_tid && !stallSig->blockFetch[tid] &&
            !fetchQueue[tid].empty() &&
            !(block_policy_active && threadFetchBlocked[tid])) {
            selected_tids.push_back(tid);
        }
    }

    unsigned total_insts_to_decode = 0;
    for (const ThreadID tid : selected_tids) {
        DPRINTF(Fetch, "select Unstalled [tid:%i]\n", tid);

        unsigned thread_insts = 0;
        auto &insts = fetchQueue[tid];

        if (deferVsetvlDecode[tid] && postSquashFetchBatchSize[tid] > 0) {
            const auto batch_end = insts.begin() +
                std::min<size_t>(postSquashFetchBatchSize[tid], insts.size());
            const bool has_vsetvli = std::any_of(
                    insts.begin(), batch_end,
                    [](const DynInstPtr& inst) {
                        if (!inst->staticInst->isVectorConfig())
                            return false;
                        const auto *vset = static_cast<const RiscvISA::VConfOp *>(
                            inst->staticInst.get());
                        return vset->vtypeIsImm;
                    });
            deferVsetvlDecode[tid] = false;
            postSquashFetchBatchSize[tid] = 0;
            if (has_vsetvli) {
                DPRINTF(Fetch,
                        "[tid:%i] Deferring post-squash vsetvli/vsetivli batch "
                        "in fetch queue for one cycle.\n",
                        tid);
                measureFrontendBubbles(0, tid);
                continue;
            }
        }

        while (!insts.empty() && thread_insts < decodeWidth) {
            assert(toDecode->size < MaxWidth);
            const auto &inst = insts.front();
            toDecode->insts[toDecode->size++] = inst;
            DPRINTF(Fetch,
                    "[tid:%i] [sn:%llu] Sending instruction to decode "
                    "from fetch queue. Fetch queue size: %i.\n",
                    tid, inst->seqNum, insts.size());

            wroteToTimeBuffer = true;
            insts.pop_front();
            ++thread_insts;
        }

        total_insts_to_decode += thread_insts;
        measureFrontendBubbles(thread_insts, tid);
    }

    // Legacy stall-reason vectors describe one logical decode lane. Keep
    // that attribution while the explicit distributions below expose the
    // widened aggregate SMT transfer.
    updateStallReasons(total_insts_to_decode, primary_tid);
    fetchStats.decodeThreadsPerCycle.sample(selected_tids.size());
    fetchStats.instsSentToDecodePerCycle.sample(total_insts_to_decode);

    // D4-B d4bDiag evidence: instructions delivered to decode this tick
    // (the H1/H3/H2 overlap diagnostics' delivery signal).
    ledgerDeliveredToDecode_ = static_cast<unsigned>(total_insts_to_decode);

    // D4-A R3/R4: delivering >= 1 instruction to decode for this tid is
    // the end condition of any open frontend window (BTB-miss redirect
    // window / commit-squash recovery window) — the frontend is supplying
    // the currently-known correct path again.  Cycles in which decode is
    // backend-blocked (blockFetch) while the queue is non-empty can
    // extend an open window past the queue refill; that is a documented
    // approximation of this diagnostic proxy (delivery, not refill, is
    // the defined end point).
    if (insts_to_decode >= 1) {
        fetchStats.closeBtbMissWindow(tid);
        fetchStats.closeRecoveryWindow(tid);
    }

    // If there was activity this cycle, inform the CPU of it
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

void
Fetch::updateStallReasons(unsigned insts_to_decode, ThreadID tid)
{
    if (stallSig->blockFetch[tid]) {
        setAllFetchStalls(stallSig->fetchBlockReason[tid]);
    } else if (insts_to_decode == 0) {
        // fetch stalled
        if (stallReason[0] != StallReason::NoStall) {
            // previously set stall reason
            setAllFetchStalls(stallReason[0]);
        } else {
            setAllFetchStalls(StallReason::OtherFetchStall);
        }
    } else {
        // fetch partially stalled or no stall
        for (int i = 0; i < stallReason.size(); i++) {
            if (i < insts_to_decode)
                stallReason[i] = StallReason::NoStall;
            else {
                stallReason[i] = StallReason::FetchFragStall;
            }
        }
    }

    toDecode->fetchStallReason = stallReason;
}

void
Fetch::measureFrontendBubbles(unsigned insts_to_decode, ThreadID tid)
{
    // Intel TopDown method for measuring frontend bubbles
    // Count unutilized issue slots when backend is not stalled (decode not stalled)
    // For N-wide machine, if frontend supplies 0 instructions:
    // - fetchBubbles += N (count total empty slots)
    // - fetchBubbles_max += 1 (count occurrence of all slots being empty)
    const tdstats::FrontendThreadCycleState t{
        stallSig->blockFetch[tid],
        fromCommit->commitInfo[tid].robSquashing};
    const tdstats::FrontendBubbleDelta d =
        tdstats::accountFrontendBubblesPerThread(t, insts_to_decode,
                                                 decodeWidth);
    fetchStats.fetchBubbles += d.fetchBubbles;
    fetchStats.fetchBubbles_max += d.fetchBubblesMax;

    if (stallSig->blockFetch[tid]) {
        fetchStats.decodeStalls++;
        //fetchStats.smtdecodeStalls[tid]++;
    }
}

void
Fetch::measureFrontendBubblesEmptyQueueCycle()
{
    // Intel TopDown method for measuring frontend bubbles — empty-queue
    // early-out path (no thread selected in sendInstructionsToDecode).
    //
    // Reached only when some thread is not backend-blocked (any_thread_active)
    // but no thread is a selection candidate, i.e. every unblocked thread has
    // an empty fetch queue.  This is a *cycle-level* aggregate: the bubble is
    // charged at most once per cycle at full decode width, never per thread,
    // and no tid == -1 is ever passed to a per-thread indexed stats function.
    //
    // Gate: at least one unblocked thread must also be outside squash
    // recovery (robSquashing).  If every unblocked thread is robSquashing the
    // cycle belongs to bad-speculation recovery, not the frontend.
    //
    // SMT note: the bubble is charged once at full decode width regardless of
    // how many threads have an empty queue (single-thread configs are
    // unaffected); see the commit message for this SMT policy decision.
    tdstats::FrontendThreadCycleState states[MaxThreads];
    for (ThreadID i = 0; i < numThreads; ++i) {
        states[i] = tdstats::FrontendThreadCycleState{
            stallSig->blockFetch[i],
            fromCommit->commitInfo[i].robSquashing};
    }
    const tdstats::FrontendBubbleDelta d =
        tdstats::accountFrontendBubblesEmptyQueueCycle(
            states, numThreads, decodeWidth);
    fetchStats.fetchBubbles += d.fetchBubbles;
    fetchStats.fetchBubbles_max += d.fetchBubblesMax;
}

bool
Fetch::checkSignalsAndUpdate(ThreadID tid)
{
    // Check squash signals from commit.
    bool commitSquashed = handleCommitSignals(tid);

    if (commitSquashed) {
        return true;
    }

    if (handleDecodeSquash(tid)) {
        return true;
    }

    if (fetchStatus[tid] == Blocked ||
        fetchStatus[tid] == Squashing) {
        // Switch status to running if fetch isn't being told to block or
        // squash this cycle.
        DPRINTF(Fetch, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        setThreadStatus(tid, Running);

        return true;
    }

    // Handle WaitingCache state: check if cache request is complete
    if (fetchStatus[tid] == WaitingCache &&
        threads[tid].cacheReq.getOverallStatus() == AccessComplete) {
        // Cache access completed, transition to Running
        setThreadStatus(tid, Running);
        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause fetch to change its status.  Fetch remains the same as before.
    return false;
}



bool
Fetch::isFlushFromPolicy() const
{
    return smtFetchBlockPolicy == SMTFetchBlockPolicy::FlushFromLoadPolicy ||
           smtFetchBlockPolicy == SMTFetchBlockPolicy::FlushFromUsePolicy;
}

DynInstPtr
Fetch::findFirstUse(const DynInstPtr &loadInst, ThreadID tid)
{
    if (loadInst->numDestRegs() == 0) return nullptr;

    std::vector<RegIndex> destPhysRegs;
    for (int d = 0; d < loadInst->numDestRegs(); d++) {
        PhysRegIdPtr r = loadInst->renamedDestIdx(d);
        if (r) destPhysRegs.push_back(r->flatIndex());
    }
    if (destPhysRegs.empty()) return nullptr;

    auto& instList = iewStage->getRobInstList(tid);
    bool foundLoad = false;
    DynInstPtr lastNonControl = loadInst;  // fallback: load itself
    for (auto it = instList.begin(); it != instList.end(); ++it) {
        if (!foundLoad) {
            if ((*it)->seqNum == loadInst->seqNum) foundLoad = true;
            continue;
        }
        const auto &candidate = *it;
        if (!candidate->isControl()) {
            lastNonControl = candidate;
        }
        for (int s = 0; s < candidate->numSrcRegs(); s++) {
            PhysRegIdPtr srcReg = candidate->renamedSrcIdx(s);
            if (srcReg) {
                RegIndex srcFlat = srcReg->flatIndex();
                for (RegIndex destFlat : destPhysRegs) {
                    if (srcFlat == destFlat) {
                        DPRINTF(Fetch, "[tid:%i] FlushFromUse: first use PC=%#x "
                                "[sn:%llu] reads phys reg %u from load [sn:%llu]\n",
                                tid, candidate->pcState().instAddr(),
                                candidate->seqNum, srcFlat, loadInst->seqNum);
                        // Avoid selecting a control instruction as squash boundary
                        // to prevent predictor state corruption at commit.
                        if (candidate->isControl()) {
                            DPRINTF(Fetch, "[tid:%i] FlushFromUse: first use is control, "
                                    "falling back to last non-control before it\n", tid);
                        }
                        assert(foundLoad);
                        return lastNonControl;
                    }
                }
            }
        }
    }
    DPRINTF(Fetch, "[tid:%i] FlushFromUse: no consumer found for load [sn:%llu], "
            "no consumer, will squash from ROB tail\n", tid, loadInst->seqNum);
    assert(foundLoad);
    return nullptr;
}

void
Fetch::flushFromInitiateFlush(const DynInstPtr &loadInst, ThreadID tid, bool fromUse)
{
    DynInstPtr squashFromInst = nullptr;
    bool includeSquashInst = true;

    if (!fromUse) {
        // FlushFromLoad: squash all instructions after the load (including pipeline in-flight)
        squashFromInst = loadInst;
        includeSquashInst = false;  // do not include the load itself
        DPRINTF(Fetch, "[tid:%i] FlushFromLoad: load PC=%#x [sn:%llu], "
                "squash all after load\n",
                tid, loadInst->pcState().instAddr(), loadInst->seqNum);
    } else {
        // FlushFromUse: try to find first consumer and flush from there
        squashFromInst = findFirstUse(loadInst, tid);
        if (squashFromInst) {
            if (squashFromInst == loadInst) {
                squashFromInst = loadInst;
                includeSquashInst = false;  // do not include the load itself
                DPRINTF(Fetch, "[tid:%i] FlushFromUse: load PC=%#x [sn:%llu], "
                        "squash all after load\n",
                        tid, loadInst->pcState().instAddr(), loadInst->seqNum);
            } else {
                includeSquashInst = true;
                DPRINTF(Fetch, "[tid:%i] FlushFromUse: load PC=%#x [sn:%llu], "
                        "squash from first-use PC=%#x [sn:%llu]\n",
                        tid, loadInst->pcState().instAddr(), loadInst->seqNum,
                        squashFromInst->pcState().instAddr(), squashFromInst->seqNum);
            }
            fetchStats.flushFromFirstUseFound++;
        } else {
            // Find the last non-control instruction in ROB (after load)
            // to avoid predictor state corruption when preserving a branch.
            DynInstPtr safeTail = nullptr;
            auto& instList2 = iewStage->getRobInstList(tid);
            for (auto it = instList2.rbegin(); it != instList2.rend(); ++it) {
                if ((*it)->seqNum <= loadInst->seqNum) break;
                if (!(*it)->isControl()) {
                    safeTail = *it;
                    break;
                }
            }
            if (!safeTail) {
                // All instructions after load are control, or ROB only has load
                squashFromInst = loadInst;
                includeSquashInst = false;
                DPRINTF(Fetch, "[tid:%i] FlushFromUse: no consumer, no non-control "
                        "inst after load, fallback squash from load [sn:%llu]\n",
                        tid, loadInst->seqNum);
            } else {
                squashFromInst = safeTail;
                includeSquashInst = false;
                DPRINTF(Fetch, "[tid:%i] FlushFromUse: no consumer, "
                        "squash from non-control [sn:%llu] next (preserve ROB)\n",
                        tid, safeTail->seqNum);
            }
            fetchStats.flushFromFirstUseNoConsumer++;
        }
    }

    flushFromInitiated[tid] = true;
    fetchStats.flushForFlushPolicy[tid]++;

    iewStage->squashDueToLongLatencyLoad(loadInst, squashFromInst, tid,
                                          includeSquashInst);
}

void
Fetch::checkLongLatencyLoads()
{
    if (smtFetchBlockPolicy == SMTFetchBlockPolicy::BaseLine) {
        return;
    }
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        blockStateHoldCycles[tid]++;

        // Get LQ head stall reason for this thread
        StallReason lqReason = iewStage->ldstQueue.lqEmpty(tid)
            ? StallReason::NoStall
            : iewStage->checkLsqStall(tid, true);

        // Check if LQ head is stuck on a long-latency load
        bool is_long_latency =
            (lqReason == StallReason::LoadL2Bound ||
            lqReason == StallReason::LoadL3Bound ||
            lqReason == StallReason::LoadMemBound);

        if (is_long_latency) {
            InstSeqNum newLoadHead = iewStage->ldstQueue.getLoadHeadSeqNum(tid);
            if (newLoadHead == lastLoadHeadSeqNum[tid]) {
                if (!threadFetchBlocked[tid] &&
                    longLatencyStallCycles[tid] >= longLatencyThreshold) {
                    threadFetchBlocked[tid] = true;
                    fetchStats.fetchBlockHoldCycle[0].sample(blockStateHoldCycles[tid]);
                    blockStateHoldCycles[tid] = 0;
                    DPRINTF(Fetch, "[tid:%i] Long-latency load detected: "
                        "LQ head stalled for %llu cycles (reason=%d)\n",
                        tid, longLatencyStallCycles[tid], (int)lqReason);
                }
                longLatencyStallCycles[tid]++;
            } else {
                if (threadFetchBlocked[tid]) {
                    threadFetchBlocked[tid] = false;
                    flushFromInitiated[tid] = false;
                    fetchStats.fetchBlockHoldCycle[1].sample(blockStateHoldCycles[tid]);
                    blockStateHoldCycles[tid] = 0;
                    DPRINTF(Fetch, "[tid:%i] Long-latency load Done\n", tid);
                }
                longLatencyStallCycles[tid] = 0;
                lastLoadHeadSeqNum[tid] = newLoadHead;
            }
        } else {
            if (threadFetchBlocked[tid]) {
                threadFetchBlocked[tid] = false;
                flushFromInitiated[tid] = false;
                fetchStats.fetchBlockHoldCycle[1].sample(blockStateHoldCycles[tid]);
                blockStateHoldCycles[tid] = 0;
                DPRINTF(Fetch, "[tid:%i] Long-latency load Done\n", tid);
            }
            longLatencyStallCycles[tid] = 0;
            lastLoadHeadSeqNum[tid] = UINT64_MAX;
        }
    }
    int blockState = 0;
    for (ThreadID tid = numThreads - 1; tid >= 0; tid--) {
        blockState = (blockState << 1) + (int)(threadFetchBlocked[tid]);
    }
    fetchStats.fetchBlockState[blockState]++;
}

void
Fetch::handleIEWSignals()
{
    // Both consumers use the IEW notification, but own independent state.
    if (!isBTBPred()) {
        return;
    }

    updateEarlyRedirectHints();
    processResolveUpdates();
}

void
Fetch::updateEarlyRedirectHints()
{
    if (numThreads <= 1) {
        return;
    }

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (fromIEW->iewInfo[tid].redirectPending) {
            redirectPending[tid] = true;
            redirectPendingCycles[tid] = redirectPendingHoldCycles;
            dbpbtb->setRedirectPending(tid, true);
        }
    }
}

void
Fetch::processResolveUpdates()
{
    const bool had_pending_resolve = !resolveQueue.empty();
    uint8_t enqueueCount = 0;
    uint8_t enqueueSize = 0;

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        const auto &iewInfo = fromIEW->iewInfo[tid];
        if (iewInfo.redirectPending) {
            // The early redirect reaches Fetch before the formal Commit
            // squash. Apply its cutoff independently of the SMT scheduling
            // hint, including for a single thread.
            squashResolveQueue(tid, iewInfo.redirectLastValidSeqNum);
        }
        for (const auto &resolved : iewInfo.resolvedCFIs) {
            if (!iewInfo.redirectPending ||
                resolved.seqNum <= iewInfo.redirectLastValidSeqNum) {
                enqueueSize++;
            } else {
                fetchStats.resolveSquashedEvents++;
            }
        }
    }

    if (resolveQueueSize && resolveQueue.size() > resolveQueueSize - 4) {
        fetchStats.resolveQueueFullEvents++;
        fetchStats.resolveEnqueueFailEvent += enqueueSize;
    } else {
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            const auto &iewInfo = fromIEW->iewInfo[tid];
            auto &incoming = iewInfo.resolvedCFIs;
            for (const auto &resolved : incoming) {
                panic_if(resolved.tid != tid,
                         "Resolve event arrived on the wrong thread wire");
                if (iewInfo.redirectPending &&
                    resolved.seqNum > iewInfo.redirectLastValidSeqNum) {
                    continue;
                }
                bool merged = false;
                for (auto &queued : resolveQueue) {
                    if (queued.tid == tid &&
                        queued.ftqId == resolved.ftqId) {
                        queued.events.push_back(resolved);
                        merged = true;
                        break;
                    }
                }

                if (merged) {
                    continue;
                }

                ResolveQueueEntry new_entry;
                new_entry.tid = tid;
                new_entry.ftqId = resolved.ftqId;
                new_entry.events.push_back(resolved);
                resolveQueue.push_back(std::move(new_entry));
                enqueueCount++;
            }
        }
        fetchStats.resolveEnqueueCount.sample(enqueueCount);
    }

    fetchStats.resolveQueueOccupancy.sample(resolveQueue.size());

    // Process only entries that were already queued before this cycle.
    if (had_pending_resolve && !resolveQueue.empty()) {
        auto &entry = resolveQueue.front();
        ThreadID tid = entry.tid;
        bool success = dbpbtb->resolveUpdate(entry.events);
        if (success) {
            dbpbtb->notifyResolveSuccess(tid);
            fetchStats.resolveDequeueEventCount += entry.events.size();
            resolveQueue.pop_front();
            fetchStats.resolveDequeueCount++;
        } else {
            dbpbtb->notifyResolveFailure(tid);
        }
    }
}

void
Fetch::squashResolveQueue(ThreadID tid, InstSeqNum squashSeqNum)
{
    for (auto entry = resolveQueue.begin(); entry != resolveQueue.end();) {
        if (entry->tid != tid) {
            ++entry;
            continue;
        }

        const auto oldSize = entry->events.size();
        entry->events.erase(
            std::remove_if(
                entry->events.begin(), entry->events.end(),
                [squashSeqNum](const auto &event) {
                    return event.seqNum > squashSeqNum;
                }),
            entry->events.end());
        fetchStats.resolveSquashedEvents += oldSize - entry->events.size();

        if (entry->events.empty()) {
            entry = resolveQueue.erase(entry);
        } else {
            ++entry;
        }
    }
}

void
Fetch::clearResolveQueue(ThreadID tid)
{
    for (auto entry = resolveQueue.begin(); entry != resolveQueue.end();) {
        if (entry->tid != tid) {
            ++entry;
            continue;
        }

        entry = resolveQueue.erase(entry);
    }
}

bool
Fetch::handleCommitSignals(ThreadID tid)
{
    const auto &commit_info = fromCommit->commitInfo[tid];
    // A commit-time MDP violation can squash after older blocks retire in
    // this cycle. Consume those outcomes before recovering the younger FTQ.
    if (commit_info.doneFtqId) {
        DPRINTF(DecoupleBP, "Commit stream Id: %lu\n",
                commit_info.doneFtqId);
        assert(dbpbtb);
        dbpbtb->commit(
            commit_info.doneFtqId, tid,
            commit_info.committedFetchBlocks);
    }
    if (!commit_info.squash) {
        return false;
    }

    // Check squash signals from commit.
    DPRINTF(Fetch,
            "[tid:%i] Squashing instructions due to squash "
            "from commit.\n",
            tid);

        InstSeqNum squash_seq = fromCommit->commitInfo[tid].doneSeqNum;
        DynInstPtr squash_inst = fromCommit->commitInfo[tid].squashInst;
        if (fromCommit->commitInfo[tid].isTrapSquash &&
            fromCommit->commitInfo[tid].traceTrapSkipInst) {
            squash_seq = fromCommit->commitInfo[tid].traceTrapSeqNum;
            squash_inst = nullptr;
            DPRINTF(Fetch,
                    "[tid:%i] Trap squash with trace ctrl-flow fault: rollback seq=%llu (skip head)\n",
                    tid, static_cast<unsigned long long>(squash_seq));
        }

    // In any case, squash.
    // D4-A R4: the frontend recovery window opens in this fetch tick —
    // the cycle fetch consumes the commit squash signal (commitToFetch-
    // Delay = 2 cycles after commit initiated it), aligned with "the
    // frontend starts recovering".  The overlap rule closes any open
    // recovery window by its elapsed cycles first.  Statistics-only
    // bookkeeping.
    fetchStats.openRecoveryWindow(tid);
    // D4-A R3: a deeper (commit-side) squash invalidates the path an open
    // BTB-miss redirect window was waiting for — truncate it by its
    // elapsed cycles (the "interrupted by a subsequent squash" rule; the
    // squash and robSquashing flags co-arrive on this commitInfo, and a
    // window opened DURING an ongoing robSquashing period is a legitimate
    // correct-path redirect that must survive — hence the event-driven
    // truncation here rather than a per-cycle robSquashing check).
    fetchStats.closeBtbMissWindow(tid);

    squash(*fromCommit->commitInfo[tid].pc, squash_seq,
           squash_inst, tid);

    localSquashVer[tid].update(
        fromCommit->commitInfo[tid].squashVersion.getVersion());
    DPRINTF(Fetch, "Updating squash version to %u\n",
            localSquashVer[tid].getVersion());

    auto mispred_inst = fromCommit->commitInfo[tid].mispredictInst;
    clearRedirectPending(tid);

    if (mispred_inst) {
        DPRINTF(Fetch, "Use mispred inst to redirect, treating as control squash\n");
        const auto corr_pc = fromCommit->commitInfo[tid].pc->as<RiscvISA::PCState>();
        assert(dbpbtb);
        dbpbtb->controlSquash(mispred_inst->getFtqId(), mispred_inst->pcState(),
                              corr_pc, mispred_inst, mispred_inst->staticInst,
                              mispred_inst->getInstBytes(), fromCommit->commitInfo[tid].branchTaken,
                              mispred_inst->seqNum, tid, mispred_inst->getLoopIteration(), true);
    } else if (fromCommit->commitInfo[tid].isTrapSquash) {
        DPRINTF(Fetch, "Treating as trap squash\n", tid);
        const auto trap_pc = fromCommit->commitInfo[tid].pc->as<RiscvISA::PCState>();
        assert(dbpbtb);
        dbpbtb->trapSquash(fromCommit->commitInfo[tid].squashedTargetId, fromCommit->commitInfo[tid].committedPC,
                           trap_pc, tid, fromCommit->commitInfo[tid].squashedLoopIter);
    } else {
        if (fromCommit->commitInfo[tid].pc && fromCommit->commitInfo[tid].squashedTargetId != 0) {
            DPRINTF(Fetch, "Squash with stream id and target id from IEW\n");
            const auto nc_pc = fromCommit->commitInfo[tid].pc->as<RiscvISA::PCState>();
            assert(dbpbtb);
            dbpbtb->nonControlSquash(fromCommit->commitInfo[tid].squashedTargetId, nc_pc,
                                     0, tid, fromCommit->commitInfo[tid].squashedLoopIter);
        } else {
            DPRINTF(Fetch, "Dont squash dbq because no meaningful stream\n");
        }
    }

    return true;
}

bool
Fetch::handleDecodeSquash(ThreadID tid)
{
    // Check squash signals from decode.
    if (fromDecode->decodeInfo[tid].squash) {
        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from decode.\n",tid);

        // This must not depend on fetchStatus: an overlapping older squash
        // can already have placed Fetch in Squashing while Decode supplies a
        // tighter wrong-path cutoff.
        squashResolveQueue(tid, fromDecode->decodeInfo[tid].doneSeqNum);
        auto mispred_inst = fromDecode->decodeInfo[tid].mispredictInst;
        clearRedirectPending(tid);
        if (fromDecode->decodeInfo[tid].branchMispredict) {
            assert(dbpbtb);
            const auto next_pc =
                fromDecode->decodeInfo[tid].nextPC->as<RiscvISA::PCState>();
            dbpbtb->controlSquash(
                mispred_inst->getFtqId(),
                mispred_inst->pcState(),
                next_pc,
                mispred_inst,
                mispred_inst->staticInst, mispred_inst->getInstBytes(),
                fromDecode->decodeInfo[tid].branchTaken,
                mispred_inst->seqNum, tid, mispred_inst->getLoopIteration(),
                false, false,
                isTraceMode() && mispred_inst->hasTraceBranchInfo());
        } else {
            warn("Unexpected non-control squash from decode.\n");
        }

        if (fetchStatus[tid] != Squashing) {

            DPRINTF(Fetch, "Squashing from decode with PC = %s\n",
                *fromDecode->decodeInfo[tid].nextPC);
            // D4-A R3: fetch consumes a decode selfSquash redirect here
            // (decodeToFetchDelay = 1 cycle after decode emitted it).  A
            // BTB-miss redirect (the redirecting instruction's fetch-time
            // snapshot predBtbHit false) (re)opens the per-tid redirect
            // window — the overlap rule closes any open window by its
            // elapsed cycles first.  A false-hit / wrong-target redirect
            // (predBtbHit true) only truncates any open window (its
            // awaited path is invalidated by this redirect) without
            // opening a new one.  Statistics-only bookkeeping.
            if (mispred_inst && !mispred_inst->readPredBtbHit()) {
                fetchStats.openBtbMissWindow(tid);
            } else {
                fetchStats.closeBtbMissWindow(tid);
            }
            // Squash unless we're already squashing
            squashFromDecode(*fromDecode->decodeInfo[tid].nextPC,
                             fromDecode->decodeInfo[tid].squashInst,
                             fromDecode->decodeInfo[tid].doneSeqNum,
                             tid);

            return true;
        }
    }

    return false;
}

DynInstPtr
Fetch::buildInst(ThreadID tid, StaticInstPtr staticInst,
        StaticInstPtr curMacroop, const PCStateBase &this_pc,
        const PCStateBase &next_pc, bool trace)
{
    // Get a sequence number.
    InstSeqNum seq = cpu->getAndIncrementInstSeq();

    DynInst::Arrays arrays;
    arrays.numSrcs = staticInst->numSrcRegs();
    arrays.numDests = staticInst->numDestRegs();

    // Create a new DynInst from the instruction fetched.
    DynInstPtr instruction = new (arrays) DynInst(
            arrays, staticInst, curMacroop, this_pc, next_pc, seq, cpu);

    instruction->setTid(tid);

    cpu->perfCCT->createMeta(instruction);
    cpu->perfCCT->updateInstPos(instruction->seqNum, PerfRecord::AtFetch);

    instruction->setThreadState(cpu->thread[tid]);

    DPRINTF(Fetch, "[tid:%i] Instruction PC %s created [sn:%lli].\n",
            tid, this_pc, seq);

    DPRINTF(Fetch, "[tid:%i] Instruction is: %s\n", tid,
            instruction->staticInst->disassemble(this_pc.instAddr()));

    DPRINTF(Fetch, "Is nop: %i, is move: %i\n", instruction->isNop(),
            instruction->isMov());
    assert(dbpbtb);
    const auto prediction = dbpbtb->ftqFetchBlock(tid);
    DPRINTF(DecoupleBP, "Set instruction %lu with fetch id %lu\n",
            instruction->seqNum, prediction.ftqId);
    instruction->setFtqId(prediction.ftqId);

#if TRACING_ON
    if (trace) {
        instruction->traceData =
            cpu->getTracer()->getInstRecord(curTick(), cpu->tcBase(tid),
                    instruction->staticInst, this_pc, curMacroop);
    }
#else
    instruction->traceData = NULL;
#endif

    // Add instruction to the CPU's list of instructions.
    instruction->setInstListIt(cpu->addInst(instruction));

    // Write the instruction to the first slot in the queue
    // that heads to decode.
    assert(numInst < fetchWidth);
    fetchQueue[tid].push_back(instruction);
    assert(fetchQueue[tid].size() <= fetchQueueSize);
    DPRINTF(Fetch, "[tid:%i] Fetch queue entry created (%i/%i).\n",
            tid, fetchQueue[tid].size(), fetchQueueSize);
    //toDecode->insts[toDecode->size++] = instruction;

    // Keep track of if we can take an interrupt at this boundary
    delayedCommit[tid] = instruction->isDelayedCommit();

    instruction->fallThruPC = this_pc.getFallThruPC();

    return instruction;
}

bool
Fetch::predecodePipelineEnabled(ThreadID tid) const
{
    return enablePredecode && numThreads == 1 && tid < numThreads &&
        !isTraceMode();
}

bool
Fetch::predecodeEnabled(ThreadID tid, const StaticInstPtr &staticInst,
                        const StaticInstPtr &curMacroop) const
{
    return predecodePipelineEnabled(tid) &&
        !curMacroop && staticInst && !staticInst->isMacroop() &&
        !staticInst->isVectorConfig();
}

void
Fetch::clearPredecodePipeline(ThreadID tid)
{
    predecodeStage0[tid].clear();
    predecodeStage1[tid].clear();
}

bool
Fetch::processPredecodeStage(ThreadID tid)
{
    auto &stage = predecodeStage1[tid];
    for (unsigned i = 0; i < stage.size; ++i) {
        const DynInstPtr instruction = stage.entries[i].instruction;
        if (!instruction || instruction->isSquashed())
            continue;

        const auto fault = classifyPredecodeFault(
            instruction, instruction->staticInst);
        instruction->setPredecodeChecked();
        DPRINTF(Fetch,
                "[tid:%i] Predecode checker at PC %#lx, seq=%llu, fault=%u\n",
                tid, instruction->pcState().instAddr(), instruction->seqNum,
                static_cast<unsigned>(fault));
        if (fault != PredecodeFault::None) {
            handlePredecodeFault(tid, instruction, fault);
            return true;
        }
    }
    stage.clear();
    return false;
}

void
Fetch::advancePredecodePipeline()
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (!predecodePipelineEnabled(tid))
            continue;

        if (processPredecodeStage(tid))
            continue;

        predecodeStage1[tid] = std::move(predecodeStage0[tid]);
        predecodeStage0[tid].clear();
    }
}

Fetch::PredecodeFault
Fetch::classifyPredecodeFault(const DynInstPtr &instruction,
                              const StaticInstPtr &staticInst) const
{
    if (instruction->readPredTaken() && !staticInst->isControl())
        return PredecodeFault::NonCfiTaken;
    if (staticInst->isReturn() && !staticInst->isNonSpeculative() &&
        !instruction->readPredTaken())
        return PredecodeFault::ReturnNotTaken;
    if (staticInst->isDirectCtrl()) {
        if (!instruction->readPredTaken() && staticInst->isUncondCtrl())
            return PredecodeFault::DirectNotTaken;
        if (instruction->readPredTaken()) {
            const auto target = instruction->branchTarget();
            if (target->instAddr() !=
                instruction->readPredTarg().instAddr())
                return PredecodeFault::DirectTargetMismatch;
        }
    }
    return PredecodeFault::None;
}

void
Fetch::handlePredecodeFault(ThreadID tid, const DynInstPtr &instruction,
                            PredecodeFault fault)
{
    RiscvISA::PCState target = instruction->pcState().as<RiscvISA::PCState>();
    bool actuallyTaken = true;
    const char *faultName = "unknown";
    switch (fault) {
      case PredecodeFault::DirectNotTaken:
        faultName = "direct-not-taken";
        target = instruction->branchTarget()->as<RiscvISA::PCState>();
        break;
      case PredecodeFault::DirectTargetMismatch:
        faultName = "direct-target-mismatch";
        target = instruction->branchTarget()->as<RiscvISA::PCState>();
        break;
      case PredecodeFault::ReturnNotTaken:
        faultName = "return-not-taken";
        target.set(getPreservedReturnAddr(instruction));
        break;
      case PredecodeFault::NonCfiTaken:
        faultName = "taken-non-cfi";
        target.set(instruction->pcState().getFallThruPC());
        actuallyTaken = false;
        break;
      case PredecodeFault::None:
        return;
    }

    DPRINTF(Fetch,
            "[tid:%i] Predecode redirect (%s) at PC %#lx to %#lx, seq=%llu\n",
            tid, faultName, instruction->pcState().instAddr(),
            target.instAddr(), instruction->seqNum);

    instruction->setPredTaken(actuallyTaken);
    instruction->setPredTarg(target);
    assert(dbpbtb);
    if (fault == PredecodeFault::NonCfiTaken) {
        dbpbtb->nonControlSquash(
            instruction->getFtqId(), instruction->pcState(),
            instruction->seqNum, tid, instruction->getLoopIteration());
    } else {
        dbpbtb->controlSquash(
            instruction->getFtqId(), instruction->pcState(), target,
            instruction,
            instruction->staticInst, instruction->getInstBytes(),
            actuallyTaken, instruction->seqNum, tid,
            instruction->getLoopIteration(), false, true);
    }

    // The faulting instruction is already registered. Preserve older entries
    // that may still be waiting for Decode; squash only removes younger work.
    std::deque<DynInstPtr> preservedFetchEntries;
    for (const auto &queued : fetchQueue[tid]) {
        if (queued->seqNum <= instruction->seqNum)
            preservedFetchEntries.push_back(queued);
    }

    // Redirect Fetch and mark younger in-flight front-end instructions as
    // squashed. At this point they cannot have reached the ROB yet.
    doSquash(target, instruction, instruction->seqNum, tid);
    cpu->removeInstsUntil(instruction->seqNum, tid);
    fetchQueue[tid] = std::move(preservedFetchEntries);
    delayedCommit[tid] = !fetchQueue[tid].empty() &&
        fetchQueue[tid].back()->isDelayedCommit();
    ++fetchStats.predecodeRedirects;
}

bool
Fetch::checkDecoupledFrontend(ThreadID tid)
{
    assert(dbpbtb);
    if (!isTraceMode() && !dbpbtb->ftqHasFetching(tid)) {
        dbpbtb->addFtqNotValid();
        DPRINTF(Fetch, "Skip fetch when FSQ head is not available\n");
        setAllFetchStalls(StallReason::FTQBubble);
        return false;
    }
    return true;
}

ThreadID
Fetch::getEligibleFetchTargetTid(
    const std::array<bool, MaxThreads> &excluded,
    bool record_redirect_skips)
{
    std::array<bool, MaxThreads> eligible;
    eligible.fill(true);
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        eligible[tid] = !redirectPending[tid] && !excluded[tid];
        if (fetchStatus[tid] == Idle || fetchStatus[tid] == Blocked ||
            fetchStatus[tid] == TrapPending ||
            fetchStatus[tid] == WaitingCache) {
            eligible[tid] = false;
        }
    }

    unsigned skipped = 0;
    // Use fetch-queue-aware scheduling: prioritize threads with fewer queue
    // entries.
    std::array<unsigned, MaxThreads> fetchQueueSizes;
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        fetchQueueSizes[tid] = fetchQueue[tid].size();
    }
    ThreadID tid = dbpbtb->getTargetTidByFetchQueueSize(
        eligible, record_redirect_skips ? &skipped : nullptr,
        fetchQueueSizes);
    if (record_redirect_skips && skipped) {
        fetchStats.redirectPendingFetchSkips += skipped;
        DPRINTF(Fetch, "Skipped %u FTQ heads while backend redirect is pending\n",
                skipped);
    }
    if (record_redirect_skips && tid == InvalidThreadID && skipped) {
        fetchStats.redirectPendingOnlyFetchCycles++;
    }
    return tid;
}

bool
Fetch::prepareFetchAddress(ThreadID tid, bool &status_change)
{
    DPRINTF(Fetch, "Attempting to fetch from [tid:%i]\n", tid);

    // The current PC - directly use the actual instruction address
    PCStateBase &this_pc = *threads[tid].fetchpc;

    // Handle status transitions and cache access
    if (threads[tid].cacheReq.getOverallStatus() == AccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Icache miss is complete.\n", tid);
        setThreadStatus(tid, Running);
        setAllFetchStalls(StallReason::NoStall);
        status_change = true;
        return true;
    } else if (canFetchInstructions(tid)) {
        // If the decoder needs bytes, keep this thread eligible for the
        // FTQ-to-I-cache request phase that follows instruction decoding.
        if (!macroop[tid] && !threads[tid].valid) {
            return true;
        } else if (checkInterrupt(this_pc.instAddr()) && !delayedCommit[tid]) {
            // Stall CPU if an interrupt is posted
            ++fetchStats.miscStallCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled!\n", tid);
            return false;
        }
        return true;
    } else {
        if (fetchStatus[tid] == Idle) {
            ++fetchStats.idleCycles;
            ++fetchStats.smtidleCycles[tid];
            DPRINTF(Fetch, "[tid:%i] Fetch is idle!\n", tid);
        }
        // Status is Idle, so fetch should do nothing.
        return false;
    }
}

void
Fetch::fetch(bool &status_change)
{
    //////////////////////////////////////////
    // Start actual fetch
    //////////////////////////////////////////
    std::list<ThreadID>::iterator threadit = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    while (threadit != end) {
        ThreadID tid = *threadit++;
        performInstructionFetch(tid);
    }

    std::array<bool, MaxThreads> attempted{};
    unsigned fetch_targets_started = 0;
    bool fetch_attempted = false;

    // This loop widens only FTQ-to-I-cache target selection. Instruction
    // decoding above still visits all active threads once, independent of
    // smtNumFetchTargetThreads. Charge each selected thread against the
    // target-width budget even if it cannot start a request, so width one
    // retains the baseline single-selection scheduling behavior.
    for (unsigned attempt = 0;
         attempt < numFetchTargetThreads &&
         fetch_targets_started < numFetchTargetThreads;
         ++attempt) {
        const ThreadID tid = getEligibleFetchTargetTid(
            attempted, attempt == 0);
        if (tid == InvalidThreadID) {
            break;
        }
        attempted[tid] = true;

        if (!checkDecoupledFrontend(tid)) {
            continue;
        }
        if (!prepareFetchAddress(tid, status_change)) {
            fetchStats.fetchTargetThreadNotReady++;
            continue;
        }

        fetch_attempted = true;
        if (!sendNextCacheRequest(tid, *threads[tid].fetchpc)) {
            fetchStats.fetchTargetRequestBlocked++;
            continue;
        }

        fetch_targets_started++;
        fetchStats.fetchTargetsStartedByThread[tid]++;
    }

    if (fetch_attempted) {
        ++fetchStats.cycles;
    }
    fetchStats.fetchTargetsStartedPerCycle.sample(fetch_targets_started);
    fetchStats.fetchLineRequestsCreatedPerCycle.sample(
        2 * fetch_targets_started);
}

StallReason
Fetch::checkMemoryNeeds(ThreadID tid, PCStateBase &this_pc,
                        const StaticInstPtr &curMacroop)
{
    // If we are in the middle of a macro-op, the decoder does not need
    // more memory bytes. It will continue processing the existing instruction.
    if (curMacroop) {
        return StallReason::NoStall;
    }

    // Trace 按需消费：在 decode 前逐条供给解码器。
    if (isTraceMode()) {
        assert(traceFetch);
        return traceFetch->checkMemoryNeeds(tid, this_pc);
    }

    Addr fetch_pc = this_pc.instAddr();

    // Check if fetch buffer is valid and contains this PC
    if (!threads[tid].valid) {
        DPRINTF(Fetch, "[tid:%i] Fetch buffer invalid, stalling on ICache\n", tid);
        return StallReason::IcacheStall;
    }

    // Check if the fetch buffer contains enough bytes for this instruction
    // We need at least 4 bytes to decode any RISC-V instruction (including compressed)
    if (fetch_pc < threads[tid].startPC ||
        fetch_pc + 4 > threads[tid].startPC + fetchBufferSize) {
        DPRINTF(Fetch, "[tid:%i] PC %#x outside fetch buffer range [%#x, %#x), stalling on ICache\n",
                tid, fetch_pc, threads[tid].startPC, threads[tid].startPC + fetchBufferSize);
        return StallReason::IcacheStall;
    }

    // Supply bytes to decoder - always provide 4 bytes for RISC-V
    auto *dec_ptr = decoder[tid];
    Addr offset_in_buffer = fetch_pc - threads[tid].startPC;
    memcpy(dec_ptr->moreBytesPtr(), threads[tid].data + offset_in_buffer, 4);

    DPRINTF(Fetch, "[tid:%i] Supplying 4 bytes from fetchBuffer at PC %#x (offset %d)\n",
            tid, fetch_pc, offset_in_buffer);

    // Call decoder with the actual instruction PC
    decoder[tid]->moreBytes(this_pc, fetch_pc);

    return StallReason::NoStall;
}

bool
Fetch::processSingleInstruction(ThreadID tid, PCStateBase &pc,
                                StaticInstPtr &curMacroop,
                                bool allow_two_fetch,
                                bool &continued_to_next_target)
{
    auto *dec_ptr = decoder[tid];
    bool predictedBranch = false;
    bool newMacroop = false;

    // Create a copy of the current PC state to calculate the next PC.
    std::unique_ptr<PCStateBase> next_pc(pc.clone());

    // Decode the instruction, handling macro-op transitions.
    StaticInstPtr staticInst = nullptr;
    if (!curMacroop) {
        // Decode a new instruction if not currently in a macro-op.
        staticInst = dec_ptr->decode(pc);
        ++fetchStats.insts;

        if (staticInst->isMacroop()) {
            curMacroop = staticInst;
            DPRINTF(Fetch, "[tid:%i] Macroop instruction decoded\n", tid);
        }
    }
    if (curMacroop) {
        // Fetch the next micro-op from the current macro-op.
        staticInst = curMacroop->fetchMicroop(pc.microPC());
        DPRINTF(Fetch, "[tid:%i] Fetched macroop microop\n", tid);
        // Check if this is the last micro-op.
        newMacroop = staticInst->isLastMicroop();
    }

    // Build the dynamic instruction and add it to the fetch queue
    DynInstPtr instruction =
        buildInst(tid, staticInst, curMacroop, pc, *next_pc, true);

    if (eipEnabled()) {
        instruction->xsMeta->eipDemandId =
            demandIdForPC(tid, pc.instAddr());
    }

    o3::TraceInstruction traceForThisInst;
    if (isTraceMode()) {
        assert(traceFetch);
        traceFetch->bindPendingTraceMetadata(tid, instruction, pc, traceForThisInst);
    }

    // Special handling for RISC-V vector configuration instructions.
    if (staticInst->isVectorConfig()) {
        waitForVsetvl[tid] = dec_ptr->stall();
        DPRINTF(Fetch, "[tid:%i] Vector config instruction, waitForVsetvl[tid]=%d\n",
                tid, waitForVsetvl[tid]);
    }

    instruction->setVersion(localSquashVer[tid]);
    ppFetch->notify(instruction);
    numInst++;

#if TRACING_ON
    if (debug::O3PipeView) {
        instruction->fetchTick = curTick();
    }
#endif

    // Save current PC to next_pc first
    set(next_pc, pc);

    // Handle branch prediction and update next_pc for both modes
    bool false_hit = false;
    predictedBranch = lookupAndUpdateNextPC(
        instruction, *next_pc, allow_two_fetch, continued_to_next_target,
        false_hit);

    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Branch detected with PC = %s, target = %s\n",
                instruction->threadNumber, pc, *next_pc);
    }

    // Value prediction must run before a predecode recovery can return early.
    if (valuePred && instruction->canLVP()) {
        valuepred::VPPredictRequest predictRequest;
        predictRequest.pc = instruction->getPC();
        predictRequest.seqNo = instruction->seqNum;
        predictRequest.tid = tid;
        // ExampleValuePredictor shows how a predictor can extend the public
        // request with extra fetch-time inputs without changing core fields.
        predictRequest.emplaceExt<valuepred::ExamplePredictRequestExt>(
                curTick(), instruction->opClass());
        instruction->vpResult =
            valuePred->valuePredict(predictRequest, instruction->vpRecord);
    }

    if (predecodeEnabled(tid, staticInst, curMacroop) &&
        !false_hit) {
        auto &stage = predecodeStage0[tid];
        assert(stage.size < MaxWidth);
        stage.entries[stage.size++].instruction = instruction;
        DPRINTF(Fetch,
                "[tid:%i] Predecode stage0 at PC %#lx, seq=%llu\n",
                tid, instruction->pcState().instAddr(), instruction->seqNum);
    }

    if (isTraceMode()) {
        assert(traceFetch);
        traceFetch->postBranchPredict(tid, instruction, traceForThisInst, pc, *next_pc, predictedBranch);
    }

    // A new macro-op also begins if the PC changes discontinuously.
    newMacroop |= pc.instAddr() != next_pc->instAddr();
    if (newMacroop) {
        curMacroop = NULL;
        DPRINTF(Fetch, "[tid:%i] New macroop transition, PC=%s\n",
                tid, pc);
    }

    // Update the main PC state for the next instruction.
    set(pc, *next_pc);

    return predictedBranch;
}

void
Fetch::performInstructionFetch(ThreadID tid)
{
    if (isTraceMode()) {
        assert(traceFetch);
        if (traceFetch->maybeStallFetch(tid)) {
            return;
        }
    }

    // Initialize local variables
    PCStateBase &pc_state = *threads[tid].fetchpc;
    StaticInstPtr &curMacroop = macroop[tid];

    // Control flags for main fetch loop
    bool stopFetchThisCycle = false;
    DPRINTF(Fetch, "[tid:%i] Adding instructions to queue to decode.\n", tid);

    // Main instruction fetch loop - process until fetch width or other limits
    // For decoupled frontend (including trace mode), check FTQ availability
    StallReason stall = StallReason::NoStall;
    while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize &&
           !stopFetchThisCycle && !ftqEmpty(tid) && !waitForVsetvl[tid]) {

        // Check memory needs and supply bytes to decoder if required
        stall = checkMemoryNeeds(tid, pc_state, curMacroop);
        if (stall != StallReason::NoStall) {
            break;
        }

        // Inner loop: extract as many instructions as possible from buffered
        // memory. This is primarily for macro-op instructions, which decode
        // into multiple micro-ops.
        do {
            // Process a single instruction, from decoding to PC update.
            bool continued_to_next_target = false;
            const bool predicted_taken = processSingleInstruction(
                tid, pc_state, curMacroop,
                !threads[tid].usedForTwoFetch,
                continued_to_next_target);
            stopFetchThisCycle =
                predicted_taken && !continued_to_next_target;

        } while (curMacroop &&
                 numInst < fetchWidth &&
                 fetchQueue[tid].size() < fetchQueueSize);
    }

    // Debug output for fetch queue contents
    DPRINTF(FetchVerbose, "FetchQue start dumping\n");
    for (auto it : fetchQueue[tid]) {
        DPRINTF(FetchVerbose, "inst: %s\n", it->staticInst->disassemble(it->pcState().instAddr()));
    }

    // Handle stall conditions and update statistics
    if (stall != StallReason::NoStall) {
        setAllFetchStalls(stall);
    }

    // Log why fetch stopped
    if (stopFetchThisCycle) {
        DPRINTF(Fetch,
                "[tid:%i] Done fetching, predicted branch instruction "
                "encountered.\n", tid);
    } else if (numInst >= fetchWidth) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch bandwidth for this cycle.\n", tid);
    } else if (stall != StallReason::NoStall) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, stalled due to %s.\n", tid,
                stall == StallReason::IcacheStall ? "ICache" : "other reasons");
    } else {
        DPRINTF(Fetch, "[tid:%i] Done fetching, no more instructions to fetch.\n", tid);
    }

    // Update persistent state
    macroop[tid] = curMacroop;

    if (deferVsetvlDecode[tid] && postSquashFetchBatchSize[tid] == 0 &&
        !fetchQueue[tid].empty()) {
        postSquashFetchBatchSize[tid] = fetchQueue[tid].size();
    }

    if (numInst > 0) {
        wroteToTimeBuffer = true;
    }

    // assert(fetchStatus[tid] == Running && "Fetch should be running");
}

bool
Fetch::sendNextCacheRequest(ThreadID tid, const PCStateBase &pc_state) {
    if (threads[tid].valid) {
        return false;
    }

    if (isTraceMode() && !pendingTraceSupplyValid[tid]) {
        return false;
    }

    if (ftqEmpty(tid)) {
        ++fetchStats.smtftqempty[tid];
        DPRINTF(Fetch, "[tid:%i] No FSQ entry available for next fetch\n", tid);
        return false;
    }

    assert(dbpbtb);
    const auto prediction = dbpbtb->ftqFetchBlock(tid);
    const Addr start_pc = prediction.startPC;
    const Addr current_pc = pc_state.instAddr();
    if (isTraceMode()) {
        panic_if(pendingTraceInstructionPc[tid] < prediction.startPC ||
                     pendingTraceInstructionPc[tid] >= prediction.endPC,
                 "Trace supply PC %#llx is outside FTQ range [%#llx, %#llx)",
                 static_cast<unsigned long long>(pendingTraceInstructionPc[tid]),
                 static_cast<unsigned long long>(prediction.startPC),
                 static_cast<unsigned long long>(prediction.endPC));
    }
    threads[tid].startPC = start_pc;

    if (current_pc < prediction.startPC ||
        current_pc >= prediction.endPC) {
        auto &reset_pc = threads[tid].fetchpc->as<RiscvISA::PCState>();
        reset_pc.pc(prediction.startPC);
        reset_pc.npc(prediction.startPC + 4);
        reset_pc.uReset();
        DPRINTF(Fetch,
                "[tid:%i] Resetting fetch PC to new FTQ stream start %s "
                "(previous PC %#lx outside [%#lx, %#lx))\n",
                tid, *threads[tid].fetchpc, current_pc,
                prediction.startPC, prediction.endPC);
    }

    DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access for new FSQ entry, "
                  "starting at PC %#x (endPC %#x; original PC %s)\n",
            tid, start_pc, prediction.endPC, pc_state);
    return fetchCacheLine(start_pc, tid, pc_state.instAddr());
}

void
Fetch::recvReqRetry()
{
    if (retryPkt.empty()) {
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
        cacheBlocked = false;
        return;
    }
    assert(cacheBlocked);
    retryPendingIcacheRequests();
}

void
Fetch::retryPendingIcacheRequests()
{
    while (!retryPkt.empty()) {
        PacketPtr pkt = retryPkt.front();
        const ThreadID tid = cpu->contextToThread(pkt->req->contextId());
        beginTraceDemandAttempt(tid, pkt->req);
        if (!icachePort.sendTimingReq(pkt)) {
            return;
        }

        updateCacheRequestStatusByRequest(tid, pkt->req, CacheWaitResponse);
        ppFetchRequestSent->notify(pkt->req);
        retryPkt.erase(retryPkt.begin());
    }

    cacheBlocked = false;
}

void
Fetch::profileStall(ThreadID tid)
{
    DPRINTF(Fetch,"There are no more threads available to fetch from.\n");

    // @todo Per-thread stats

    if (activeThreads->empty()) {
        ++fetchStats.noActiveThreadStallCycles;
        DPRINTF(Fetch, "Fetch has no active thread!\n");
    } else if (fetchStatus[tid] == Blocked) {
        ++fetchStats.blockedCycles;
        ++fetchStats.smtblockedCycles[tid];
        DPRINTF(Fetch, "[tid:%i] Fetch is blocked!\n", tid);
    } else if (fetchStatus[tid] == Squashing) {
        ++fetchStats.squashCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is squashing!\n", tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == CacheWaitResponse) {
        ++fetchStats.icacheStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting cache response!\n",
                tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == TlbWait) {
        ++fetchStats.tlbCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting ITLB walk to "
                "finish!\n", tid);
    } else if (fetchStatus[tid] == TrapPending) {
        ++fetchStats.pendingTrapStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending trap!\n",
                tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == CacheWaitRetry) {
        ++fetchStats.icacheWaitRetryStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for an I-cache retry!\n",
                tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == AccessFailed) {
            DPRINTF(Fetch, "[tid:%i] Fetch predicted non-executable address\n",
                    tid);
    } else {
        DPRINTF(Fetch, "[tid:%i] Unexpected fetch stall reason "
            "(Status: %i)\n",
            tid, fetchStatus[tid]);
    }
}

void
Fetch::setAllFetchStalls(StallReason stall)
{
    for (int i = 0; i < stallReason.size(); i++) {
        stallReason[i] = stall;
    }
}


bool
Fetch::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(O3CPU, "Fetch unit received timing\n");
    // We shouldn't ever get a cacheable block in Modified state
    assert(pkt->req->isUncacheable() ||
           !(pkt->cacheResponding() && !pkt->hasSharers()));

    DPRINTF(Fetch, "received pkt addr=%#lx, req addr=%#lx\n", pkt->getAddr(),
            pkt->req->getVaddr());

    fetch->processCacheCompletion(pkt);

    return true;
}

void
Fetch::IcachePort::recvReqRetry()
{
    fetch->recvReqRetry();
}

bool
Fetch::canFetchInstructions(ThreadID tid) const
{
    // Thread must be in Running state
    if (fetchStatus[tid] != Running) {
        return false;  // Covers Idle, Squashing, Blocked, TrapPending
    }

    // Cache must be ready for new requests or have completed data
    CacheRequestStatus cacheStatus = threads[tid].cacheReq.getOverallStatus();
    return (cacheStatus == CacheIdle || cacheStatus == AccessComplete);
}

bool
Fetch::hasPendingCacheRequests(ThreadID tid) const
{
    // Check for any active cache operations (excluding terminal states)
    CacheRequestStatus overallStatus = threads[tid].cacheReq.getOverallStatus();
    return (overallStatus == TlbWait ||
            overallStatus == CacheWaitResponse ||
            overallStatus == CacheWaitRetry);
}

void
Fetch::setThreadStatus(ThreadID tid, ThreadStatus status)
{
    assert(tid < MaxThreads);

    ThreadStatus oldStatus = fetchStatus[tid];
    fetchStatus[tid] = status;
    DPRINTF(Fetch, "[tid:%d] setThreadStatus: %s -> %s\n", tid, fetchStatusStr[oldStatus], fetchStatusStr[status]);
}

void
Fetch::updateCacheRequestStatus(ThreadID tid, size_t reqIndex,
                               CacheRequestStatus status)
{
    assert(tid < MaxThreads);
    assert(reqIndex < threads[tid].cacheReq.requestStatus.size());

    DPRINTF(Fetch, "[tid:%d] updateCacheRequestStatus[%d]: %d -> %d\n",
            tid, reqIndex, threads[tid].cacheReq.requestStatus[reqIndex], status);

    threads[tid].cacheReq.requestStatus[reqIndex] = status;
}

void
Fetch::updateCacheRequestStatusByRequest(ThreadID tid, const RequestPtr& req,
                                        CacheRequestStatus status)
{
    assert(tid < MaxThreads);

    size_t reqIndex = threads[tid].cacheReq.findRequestIndex(req);
    if (reqIndex != SIZE_MAX) {
        updateCacheRequestStatus(tid, reqIndex, status);
    } else {
        warn("Cannot find req %#x for status update to %d\n", req->getVaddr(), status);
    }
}

void
Fetch::cancelAllCacheRequests(ThreadID tid)
{
    assert(tid < MaxThreads);

    DPRINTF(Fetch, "[tid:%d] cancelAllCacheRequests: status before cancel: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

    // Cancel all cache requests
    threads[tid].cacheReq.cancelAllRequests();

    DPRINTF(Fetch, "[tid:%d] cancelAllCacheRequests: status after cancel: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

}


const o3::TraceInstruction*
Fetch::getTraceInstMetadata(InstSeqNum seqNum) const
{
    return traceFetch ? traceFetch->getTraceInstMetadata(seqNum) : nullptr;
}

bool
Fetch::isTraceInstruction(InstSeqNum seqNum) const
{
    return traceFetch ? traceFetch->isTraceInstruction(seqNum) : false;
}

void
Fetch::cleanupTraceMetadataOnCommit(InstSeqNum seqNum)
{
    if (traceFetch) {
        traceFetch->cleanupTraceMetadataOnCommit(seqNum);
    }
}

uint64_t
Fetch::findTraceIndexForSeqNum(InstSeqNum seqNum) const
{
    return traceFetch ? traceFetch->findTraceIndexForSeqNum(seqNum) : 0;
}

bool
Fetch::lookupTraceIndexForSeqNum(InstSeqNum seqNum, uint64_t &index) const
{
    if (!traceFetch) {
        index = 0;
        return false;
    }
    return traceFetch->lookupTraceIndexForSeqNum(seqNum, index);
}

Addr
Fetch::getTracePCByIndex(uint64_t index)
{
    return traceFetch ? traceFetch->getTracePCByIndex(index) : 0;
}

} // namespace o3
} // namespace gem5
