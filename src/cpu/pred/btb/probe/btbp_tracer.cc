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

    listeners.push_back(new Listener(this, "BtbpTraceBtbLookup",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceBtbFill",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceIPrefetchIssue",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceIPrefetchFill",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceIcacheDemandFill",
                                     &BtbpTracer::trace));
    listeners.push_back(new Listener(this, "BtbpTraceDecodeBranch",
                                     &BtbpTracer::trace));
}

void
BtbpTracer::trace(const BtbpTraceEvent &event)
{
    ProtoMessage::BtbpTraceEntry entry;
    entry.set_tick(event.tick);
    entry.set_event_type(event.eventType);
    entry.set_thread_id(event.threadId);
    entry.set_branch_pc(event.branchPc);
    entry.set_btb_level(event.btbLevel);
    entry.set_hit(event.hit);
    entry.set_target(event.target);
    entry.set_fill_source(event.fillSource);
    entry.set_line_addr(event.lineAddr);
    entry.set_trigger_pc(event.triggerPc);
    entry.set_taken_hint(event.takenHint);
    entry.set_was_in_btb_at_lookup(event.wasInBtbAtLookup);

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
