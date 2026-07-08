#ifndef __CPU_PRED_BTB_PROBE_BTBP_TRACER_HH__
#define __CPU_PRED_BTB_PROBE_BTBP_TRACER_HH__

#include <memory>

#include "cpu/pred/btb/probe/btbp_trace_event.hh"
#include "params/BtbpTracer.hh"
#include "proto/btbp_trace.pb.h"
#include "proto/protoio.hh"
#include "sim/probe/probe.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

class BtbpTracer : public ProbeListenerObject
{
  public:
    BtbpTracer(const BtbpTracerParams &params);
    ~BtbpTracer() override;

    void regProbeListeners() override;

  private:
    void trace(const BtbpTraceEvent &event);
    void close();

    std::unique_ptr<ProtoOutputStream> stream;
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_PROBE_BTBP_TRACER_HH__
