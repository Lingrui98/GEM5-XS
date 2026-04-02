#!/usr/bin/env bash

set -euo pipefail

export GEM5_WT=/nfs/home/goulingrui/project/GEM5/.worktrees/impl-control-pc-tail-halfword
export GEM5_HOME=$GEM5_WT
export GEM5_BIN=$GEM5_WT/build/RISCV/gem5.opt
export GEM5_FS_CFG=$GEM5_WT/configs/example/kmhv3.py

export MYTOOLS_HOME=/nfs/home/goulingrui/project/mytools
export SERVERS=/nfs/home/goulingrui/servers.txt
export TRACE_ARCH_SCRIPT=$MYTOOLS_HOME/tools/distributed-trace-scheduler/scripts/run_trace_gem5.sh

export VALIDATION_DIR=$GEM5_WT/openspec/changes/update-bpu-control-pc-owner-migration/validation

export FS_CHECKPOINT_ROOT=/nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc
export FS_WORKLOAD_LIST=$VALIDATION_DIR/fs_spec_target12.lst
export FS_MAXINSTS=2000
export FS_JOBS=12
export FS_TAG=fs_spec_target12

export TRACE_ROOT=/nfs/home/share/glr/champsim_traces
export TRACE_TOP66_LST=$MYTOOLS_HOME/configs/champsim_traces_btb_or_icache_mpki_top50_union.kmhv3_base.lst
export TRACE_MAX_GLOBAL=64
export TRACE_THREADS_PER_HOST=2
export TRACE_POLL_SECONDS=30
export TRACE_TAG=trace_top66

export GCBV_REF_SO=${GCBV_REF_SO:-/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so}
export GCB_RESTORER=${GCB_RESTORER:-/nfs/home/share/gem5_ci/tools/normal-gcb-restorer.bin}
export GCBV_RESTORER=${GCBV_RESTORER:-/nfs/home/share/gem5_ci/tools/gcbv-restorer.bin}

export RUN_STAMP=${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}
export RUN_ROOT=${RUN_ROOT:-$GEM5_WT/.regression_runs/control_pc_tail_halfword_$RUN_STAMP}
