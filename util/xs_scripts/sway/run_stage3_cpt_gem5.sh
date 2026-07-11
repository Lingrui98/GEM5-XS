#!/usr/bin/env bash

# Stage 3 checkpoint adapter for distributed_trace_scheduler.py.
#
# The shared checkpoint runner predates XS_WARMUP_INSTS_NO_SWITCH. Inject the
# scheduler's warmup boundary into both the CPU and SWAY phase-stream metadata
# through SimObject params, while leaving the shared runner otherwise intact.

set -euo pipefail

warmup=${XS_WARMUP_INSTS_NO_SWITCH:?XS_WARMUP_INSTS_NO_SWITCH is required}
maxinsts=${XS_MAX_INSTS:?XS_MAX_INSTS is required}

if [[ ! "${warmup}" =~ ^[0-9]+$ || ! "${maxinsts}" =~ ^[0-9]+$ ]]; then
    echo "Stage 3 requires integer warmup/maxinsts values" >&2
    exit 2
fi
if (( maxinsts <= warmup )); then
    echo "Stage 3 requires XS_MAX_INSTS > XS_WARMUP_INSTS_NO_SWITCH" >&2
    exit 2
fi

stage3_params="system.cpu[0].warmupInstCount=${warmup}"
stage3_params+=";system.cpu[0].branchPred.swayWarmupInsts=${warmup}"
stage3_params+=";system.cpu[0].branchPred.enableSwayRealloc=False"
if [[ -n "${XS_EXTRA_PARAMS:-}" ]]; then
    export XS_EXTRA_PARAMS="${XS_EXTRA_PARAMS};${stage3_params}"
else
    export XS_EXTRA_PARAMS="${stage3_params}"
fi

shared_runner=${SWAY_SHARED_CPT_RUNNER:-/nfs/home/goulingrui/project/mytools/tools/distributed-trace-scheduler/scripts/run_cpt_gem5.sh}
exec "${shared_runner}" "$@"
