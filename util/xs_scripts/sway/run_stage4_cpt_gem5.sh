#!/bin/env bash

# Stage 4 checkpoint adapter for distributed_trace_scheduler.py.
#
# Extends run_stage3_cpt_gem5.sh: in addition to the warmup/maxinsts wiring,
# it can enable the live SWAY beta controller and forward the self-normalized
# theta pair. Set XS_SWAY_ENABLE=1 to turn the controller on; otherwise it
# behaves like the Stage 3 baseline adapter (enableSwayRealloc=False, bit-exact
# baseline, raw CSV still populated).
#
# Env:
#   XS_WARMUP_INSTS_NO_SWITCH  required, warmup boundary (instructions)
#   XS_MAX_INSTS               required, total instruction count (> warmup)
#   XS_SWAY_ENABLE             optional, "1" enables enableSwayRealloc=True
#   XS_SWAY_THETA_DONOR        optional, default 1.5
#   XS_SWAY_THETA_DONEE        optional, default 1.5
#   XS_EXTRA_PARAMS            optional, forwarded through unchanged

set -euo pipefail

warmup=${XS_WARMUP_INSTS_NO_SWITCH:?XS_WARMUP_INSTS_NO_SWITCH is required}
maxinsts=${XS_MAX_INSTS:?XS_MAX_INSTS is required}

if [[ ! "${warmup}" =~ ^[0-9]+$ || ! "${maxinsts}" =~ ^[0-9]+$ ]]; then
    echo "Stage 4 requires integer warmup/maxinsts values" >&2
    exit 2
fi
if (( maxinsts <= warmup )); then
    echo "Stage 4 requires XS_MAX_INSTS > XS_WARMUP_INSTS_NO_SWITCH" >&2
    exit 2
fi

theta_donor=${XS_SWAY_THETA_DONOR:-1.5}
theta_donee=${XS_SWAY_THETA_DONEE:-1.5}
if [[ ! "${theta_donor}" =~ ^[0-9]+([.][0-9]+)?$ || \
      ! "${theta_donee}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "Stage 4 theta values must be non-negative numbers" >&2
    exit 2
fi

sway_enable=0
if [[ "${XS_SWAY_ENABLE:-0}" == "1" ]]; then
    sway_enable=1
fi

stage4_params="system.cpu[0].warmupInstCount=${warmup}"
stage4_params+=";system.cpu[0].branchPred.swayWarmupInsts=${warmup}"
stage4_params+=";system.cpu[0].branchPred.enableSwayRealloc=${sway_enable}"
stage4_params+=";system.cpu[0].branchPred.swayReallocThetaDonor=${theta_donor}"
stage4_params+=";system.cpu[0].branchPred.swayReallocThetaDonee=${theta_donee}"
if [[ -n "${XS_EXTRA_PARAMS:-}" ]]; then
    export XS_EXTRA_PARAMS="${XS_EXTRA_PARAMS};${stage4_params}"
else
    export XS_EXTRA_PARAMS="${stage4_params}"
fi

shared_runner=${SWAY_SHARED_CPT_RUNNER:-/nfs/home/goulingrui/project/mytools/tools/distributed-trace-scheduler/scripts/run_cpt_gem5.sh}
exec "${shared_runner}" "$@"
