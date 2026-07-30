from m5.objects.Probe import *


class BtbpTracer(ProbeListenerObject):
    type = "BtbpTracer"
    cxx_class = "gem5::branch_prediction::btb_pred::BtbpTracer"
    cxx_header = "cpu/pred/btb/probe/btbp_tracer.hh"

    output_file = Param.String("", "BTBP protobuf trace output file")
