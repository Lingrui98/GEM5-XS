#include "cpu/pred/btb/probe/btbp_tracer.hh"

#include "base/logging.hh"
#include "base/output.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

BtbpTracer::BtbpTracer(const BtbpTracerParams &params)
    : ProbeListenerObject(params)
{
    fatal_if(params.output_file.empty(),
             "%s requires a non-empty output_file\n", name());

    const std::string filename = simout.resolve(params.output_file);
    stream = std::make_unique<ProtoOutputStream>(filename);

    registerExitCallback([this]() { close(); });
}

BtbpTracer::~BtbpTracer()
{
    close();
}

void
BtbpTracer::regProbeListeners()
{
    using Listener = ProbeListenerArg<BtbpTracer, BtbpTraceEvent>;

    listeners.push_back(new Listener(this, "BtbpTraceMbtbLookup",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceMbtbFill",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceIPrefetchIssue",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceLineLifecycle",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceL1IDemandAccess",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceDecodeBranch",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceBranchDemand",
                                     &BtbpTracer::trace));
}

void
BtbpTracer::trace(const BtbpTraceEvent &event)
{
    ProtoMessage::BtbpTraceEntry entry;
    entry.set_tick(event.tick);
    entry.set_event_type(event.eventType);
    entry.set_schema_version(BtbpTraceEvent::SchemaVersion);
    entry.set_event_seq(++eventSeq);
    entry.set_thread_id(event.threadId);
    if (event.branchPcValid) {
        entry.set_branch_pc(event.branchPc);
    }
    if (event.hitValid) {
        entry.set_hit(event.hit);
    }
    if (event.targetValid) {
        entry.set_target(event.target);
    }
    if (event.fillSourceValid) {
        entry.set_fill_source(event.fillSource);
    }
    if (event.lineAddrValid) {
        entry.set_line_addr(event.lineAddr);
    }
    if (event.virtualLineAddrValid) {
        entry.set_virtual_line_addr(event.virtualLineAddr);
    }
    if (event.triggerPcValid) {
        entry.set_trigger_pc(event.triggerPc);
    }
    if (event.takenHintValid) {
        entry.set_taken_hint(event.takenHint);
    }
    if (event.lineSizeValid) {
        entry.set_line_size(event.lineSize);
    }
    if (event.addressSpaceIdValid) {
        entry.set_address_space_id(event.addressSpaceId);
    }
    if (event.asidHashValid) {
        entry.set_asid_hash(event.asidHash);
    }
    if (event.ftqIdValid) {
        entry.set_ftq_id(event.ftqId);
    }
    if (event.lookupTickValid) {
        entry.set_lookup_tick(event.lookupTick);
    }
    if (event.instBytesValid) {
        entry.set_inst_bytes(event.instBytes);
    }
    if (event.streamStartPcValid) {
        entry.set_stream_start_pc(event.streamStartPc);
    }
    if (event.structuralMissValid) {
        entry.set_structural_miss(event.structuralMiss);
    }
    if (event.fdipEpochValid) {
        entry.set_fdip_epoch(event.fdipEpoch);
    }
    if (event.fillBytesTickValid) {
        entry.set_fill_bytes_tick(event.fillBytesTick);
    }
    if (event.l1iReadyTickValid) {
        entry.set_l1i_ready_tick(event.l1iReadyTick);
    }
    if (event.scanCompleteTickValid) {
        entry.set_scan_complete_tick(event.scanCompleteTick);
    }
    if (event.requestKindValid) {
        entry.set_request_kind(event.requestKind);
    }
    if (event.branchKindValid) {
        entry.set_branch_kind(event.branchKind);
    }
    if (event.demandUidValid) {
        entry.set_demand_uid(event.demandUid);
    }
    if (event.prefetchDecisionIdValid) {
        entry.set_prefetch_decision_id(event.prefetchDecisionId);
    }
    if (event.prefetchSourceValid) {
        entry.set_prefetch_source(event.prefetchSource);
    }
    if (event.terminalReasonValid) {
        entry.set_terminal_reason(event.terminalReason);
    }
    if (event.mshrDemandOwnedValid) {
        entry.set_mshr_demand_owned(event.mshrDemandOwned);
    }
    if (event.mshrInstPrefetchOwnedValid) {
        entry.set_mshr_inst_prefetch_owned(event.mshrInstPrefetchOwned);
    }
    if (event.mshrTotalValid) {
        entry.set_mshr_total(event.mshrTotal);
    }
    if (event.lifecycleKindValid) {
        entry.set_lifecycle_kind(event.lifecycleKind);
    }
    if (event.residencyIdValid) {
        entry.set_residency_id(event.residencyId);
    }
    if (event.coreCycleValid) {
        entry.set_core_cycle(event.coreCycle);
    }
    if (event.committedInstsValid) {
        entry.set_committed_insts(event.committedInsts);
    }
    if (event.roiInstsValid) {
        entry.set_roi_insts(event.roiInsts);
    }

    stream->write(entry);
}

void
BtbpTracer::close()
{
    stream.reset();
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
