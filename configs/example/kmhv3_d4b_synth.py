# D4-B synthetic coverage fixture config (kmhv3-d4b-synth).
#
# Independent TEST-ONLY configuration for the D4-B production coverage
# evidence (btb_prefetch_paper .local spike, production/coverage/):
# it reproduces the full kmhv3.py system build and then SHRINKS the
# core structures (ROB / IQs / LSQ / squash width) so that the rare
# admission/signature patterns the design contract section 1.5e needs
# (S19/S20/S20b/S21 exact pipeline signatures) become reachable through
# the REAL CPU scheduling path (ROB/IQ/backpressure/squash) on short
# deterministic traces.
#
# This is an approved synthetic acceptance fixture, NOT a modification of
# the formal experiment configuration: configs/example/kmhv3.py is
# untouched, and every run under this config is labeled by its own
# outdir/tag.  No frozen scientific definition changes here -- the
# classification policy, W_C4, and the supported-domain guards are
# exactly the production ones (decodeWidth == renameWidth == 8 is kept,
# so ledgerState stays 0/valid).
#
# Usage (same trace-mode CLI as kmhv3.py, plus optional knobs):
#   gem5.opt [--outdir=...] configs/example/kmhv3_d4b_synth.py \
#       --enable-trace-mode --trace-format=champsim \
#       --trace-enable-decoupled-bp --trace-file=TRACE.gz \
#       --maxinsts=200000 --warmup-insts-no-switch=20000 \
#       --roi-insts=100000 \
#       [--enable-d4b-topdown-ledger --enable-d4b-diag \
#        --d4b-topdown-audit-export=PATH] \
#       [--synth-rob=16 --synth-iq=4 --synth-lq=8 --synth-sq=8 \
#        --synth-squash-width=2 --l1i_size=4kB --l1d_size=4kB]
#
# Knob constraints enforced below:
#   - numROBEntries stays >= renameWidth (a narrower ROB deadlocks the
#     commit-side fixedbuffer insert gate: canAllocate(used, arrivals)
#     can never succeed for a full-width arrival burst);
#   - IQ size stays >= 1 (the per-tick dispatch-bandwidth limit that
#     produces the bwFull break is the production inports mechanism,
#     not a fixture invention; kmhv3's per-IQ inports value is kept).

import os
import sys

import m5
from m5.objects import *
from m5.util import addToPath, fatal

addToPath('../')

from common.FSConfig import *
from common.SysPaths import *
from common import Simulation
from common.xiangshan import *
from kmhv3 import setKmhV3Params


def extract_btbp_trace_out(argv):
    trace_out = None
    filtered = [argv[0]]
    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg == "--btbp-trace-out":
            if i + 1 >= len(argv):
                fatal("--btbp-trace-out requires a path")
            trace_out = argv[i + 1]
            i += 2
            continue
        if arg.startswith("--btbp-trace-out="):
            trace_out = arg.split("=", 1)[1]
            i += 1
            continue
        filtered.append(arg)
        i += 1
    return trace_out, filtered


def setSynthOverrides(args, system, rob, iq, lq, sq, squash_width):
    for cpu in system.cpu:
        if rob < 8:
            fatal("synth fixture requires --synth-rob >= renameWidth (8)")
        cpu.numROBEntries = rob
        cpu.LQEntries = lq
        cpu.SQEntries = sq
        cpu.RARQEntries = lq
        cpu.RAWQEntries = sq
        if squash_width >= 1:
            cpu.squashWidth = squash_width
        for que in cpu.scheduler.IQs:
            if iq >= 1:
                que.size = iq


if __name__ == '__m5_main__':
    FutureClass = None

    def _take(prefix, default, cast):
        for i, a in enumerate(sys.argv):
            if a.startswith(prefix + '='):
                v = a.split('=', 1)[1]
                del sys.argv[i]
                return cast(v)
        return default

    synth_rob = _take('--synth-rob', 16, int)
    synth_iq = _take('--synth-iq', 4, int)
    synth_lq = _take('--synth-lq', 8, int)
    synth_sq = _take('--synth-sq', 8, int)
    synth_sqw = _take('--synth-squash-width', 2, int)

    btbp_trace_out, sys.argv = extract_btbp_trace_out(sys.argv)
    args = xiangshan_system_init()
    args.btbp_trace_out = btbp_trace_out

    assert not args.external_memory_system

    args.bp_type = 'DecoupledBPUWithBTB'
    args.l2_size = '1MB'
    args.kmh_align = True

    TestMemClass = Simulation.setMemClass(args)

    test_sys = build_xiangshan_system(args)
    if args.raw_cpt and args.generic_rv_cpt and os.path.basename(args.generic_rv_cpt) == "linux.bin":
        configure_xiangshan_linux_workload(test_sys, args)
    setKmhV3Params(args, test_sys)
    setSynthOverrides(args, test_sys, synth_rob, synth_iq, synth_lq,
                      synth_sq, synth_sqw)
    if args.btbp_trace_out:
        if len(test_sys.cpu) != 1:
            fatal("--btbp-trace-out currently supports one CPU per trace file")
        test_sys.cpu[0].btbpTracer = BtbpTracer(
            manager=test_sys.cpu[0],
            output_file=args.btbp_trace_out)

    root = Root(full_system=True, system=test_sys)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
