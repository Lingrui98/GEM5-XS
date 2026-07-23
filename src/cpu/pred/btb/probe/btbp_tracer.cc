#include "cpu/pred/btb/probe/btbp_tracer.hh"

#include "base/logging.hh"
#include "base/output.hh"
#include "cpu/pred/btb/probe/btbp_trace_entry_builder.hh"

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
    stream->write(buildBtbpTraceEntry(event, ++eventSeq));
}

void
BtbpTracer::close()
{
    stream.reset();
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
