#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <checkpoint>" >&2
    exit 2
fi

if [[ -z "${GEM5_HOME:-}" ]]; then
    echo "GEM5_HOME is required" >&2
    exit 2
fi

checkpoint=$1
outdir=${OUTDIR:-$(pwd)}
build_type=${GEM5_BUILD_TYPE:-opt}
gem5_bin=${GEM5_BIN:-$GEM5_HOME/build/RISCV/gem5.$build_type}
config_script=${GEM5_FS_CFG:-$GEM5_HOME/configs/example/kmhv3.py}
maxinsts=${FS_MAXINSTS:-2000}

if [[ ! -x "$gem5_bin" ]]; then
    echo "gem5 binary not found: $gem5_bin" >&2
    exit 1
fi

if [[ ! -f "$config_script" ]]; then
    echo "config script not found: $config_script" >&2
    exit 1
fi

mkdir -p "$outdir"

cmd=(
    "$gem5_bin"
    "--outdir=$outdir"
    "--stats-file=stats.txt"
    "$config_script"
    "--maxinsts=$maxinsts"
    "--generic-rv-cpt=$checkpoint"
)

printf 'Running:'
printf ' %q' "${cmd[@]}"
printf '\n'
"${cmd[@]}"
