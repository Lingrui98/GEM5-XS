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
    // Upstream probe.hh now owns listeners as ProbeListenerPtr
    // (unique_ptr with ProbeListenerCleanup); attach through
    // ProbeManager::connect, which registers the listener with the
    // manager supplied by Parent.any (the CPU's probe manager in the
    // kmhv3 wiring).
    using Listener = ProbeListenerArg<BtbpTracer, BtbpTraceEvent>;
    auto attach = [this](const char *name) {
        listeners.push_back(
            manager->connect<Listener>(this, name, &BtbpTracer::trace));
    };

    attach("BtbpTraceMbtbLookup");
    attach("BtbpTraceMbtbFill");
    attach("BtbpTraceIPrefetchIssue");
    attach("BtbpTraceLineLifecycle");
    attach("BtbpTraceL1IDemandAccess");
    attach("BtbpTraceDecodeBranch");
    attach("BtbpTraceBranchDemand");
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
