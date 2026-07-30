/*
 * Copyright (c) 2024 The Chinese University of Hong Kong
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_PRED_BTB_PROBE_BTBP_TRACE_ENTRY_BUILDER_HH__
#define __CPU_PRED_BTB_PROBE_BTBP_TRACE_ENTRY_BUILDER_HH__

#include "cpu/pred/btb/probe/btbp_trace_event.hh"
#include "proto/btbp_trace.pb.h"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

/**
 * Build the protobuf `BtbpTraceEntry` for a trace event. This is the
 * serialization body extracted from `BtbpTracer::trace` so the v2.3
 * emitter-to-protobuf field contract (contract stage-a v2.3 §5.3/§5.6) is
 * unit-testable without a SimObject tracer. Production `BtbpTracer::trace`
 * calls this and writes the returned entry. Field semantics and the schema-v6
 * field numbers are unchanged.
 */
inline ProtoMessage::BtbpTraceEntry
buildBtbpTraceEntry(const BtbpTraceEvent &event, uint64_t eventSeq)
{
    ProtoMessage::BtbpTraceEntry entry;
    entry.set_tick(event.tick);
    entry.set_event_type(event.eventType);
    entry.set_schema_version(BtbpTraceEvent::SchemaVersion);
    entry.set_event_seq(eventSeq);
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
    if (event.secureValid) {
        entry.set_secure(event.secure);
    }
    if (event.prefetchAttemptIdValid) {
        entry.set_prefetch_attempt_id(event.prefetchAttemptId);
    }
    if (event.gateOutcomeValid) {
        entry.set_gate_outcome(event.gateOutcome);
    }
    if (event.incomingRequestKindValid) {
        entry.set_incoming_request_kind(event.incomingRequestKind);
    }
    if (event.incomingPrefetchDecisionIdValid) {
        entry.set_incoming_prefetch_decision_id(
            event.incomingPrefetchDecisionId);
    }
    if (event.incomingPrefetchSourceValid) {
        entry.set_incoming_prefetch_source(event.incomingPrefetchSource);
    }
    if (event.prefetchOwnedMshrValid) {
        entry.set_prefetch_owned_mshr(event.prefetchOwnedMshr);
    }
    if (event.replacementValid) {
        entry.set_replacement(event.replacement);
    }
    if (event.requestUidValid) {
        entry.set_request_uid(event.requestUid);
    }
    if (event.lookupUidValid) {
        entry.set_lookup_uid(event.lookupUid);
    }
    if (event.fetchEpochValid) {
        entry.set_fetch_epoch(event.fetchEpoch);
    }
    if (event.demandAttemptOrdinalValid) {
        entry.set_demand_attempt_ordinal(event.demandAttemptOrdinal);
    }
    if (event.traceInstructionOrdinalValid) {
        entry.set_trace_instruction_ordinal(event.traceInstructionOrdinal);
    }
    if (event.visibilityTickValid) {
        entry.set_visibility_tick(event.visibilityTick);
    }
    if (event.supplySourceValid) {
        entry.set_supply_source(event.supplySource);
    }
    if (event.pathStateValid) {
        entry.set_path_state(event.pathState);
    }
    for (const uint64_t owner : event.ownerPrefetchDecisionIds) {
        entry.add_owner_prefetch_decision_ids(owner);
    }
    return entry;
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_PROBE_BTBP_TRACE_ENTRY_BUILDER_HH__
