# Change: Migrate split control ownership to the following fetch target

## Why

`update-bpu-control-pc-tail-halfword` 已经把 predictor-visible branch identity
切到 `controlPC`，因此跨边界 4B RVI control 现在只会在“后半条所在预测块”生成 taken
预测。但当前 fetch 仍沿用“DynInst 归属当前 FTQ target，再用 `trigger_covered`
阻止过早 redirect”的做法，这会留下两个问题：

- BPU 认为 branch 属于 following target，而 fetch/buildInst 仍把同一条指令挂到前一个 target；
- `lookupAndUpdateNextPC()` 还需要依赖 `trigger_covered` 这种 coverage gate，fetch 语义没有真正简化。

新的 change 需要把这层语义补齐：既然 predictor 已经把 split taken control 归到
following target，fetch 也必须在构造 DynInst 前迁移 ownership，使 redirect 仅由
“当前 owner target 是否拥有这条指令”决定，而不是由额外 coverage patch 决定。

## What Changes

- 为 `FetchTarget` 增加显式的 `decodeStartPC`：
  - 普通 target：`decodeStartPC == startPC`
  - 对于跨边界且 predicted-taken 的 carried split control target：
    `decodeStartPC == predBranchInfo.startPC()`
- 要求 fetch 在 `buildInst()` 之前检查 owner target：
  - 如果 following target 的 `decodeStartPC` 已经小于等于当前指令 `startPC`
  - 则先消费当前 target，并切换到 following target，再继续构造该条 DynInst
- 删除 fetch taken matching 中对 `trigger_covered` 的依赖：
  - taken redirect 只由 owner target 的 `predTaken + predBranchInfo.startPC()`
    匹配决定
- 明确保留现有的语义分工：
  - predictor 内部 key/index/tag/position 继续使用 `controlPC`
  - stats / trace / 调试归因继续使用架构 `startPC`

## Impact

- Affected specs: `decoupled-btb-control-pc-semantics`
- Affected code:
  - `src/cpu/pred/btb/common.hh`
  - `src/cpu/pred/btb/decoupled_bpred.hh`
  - `src/cpu/pred/btb/decoupled_bpred.cc`
  - `src/cpu/o3/fetch.cc`
  - `src/cpu/pred/btb/test/btb.test.cc`
- Dependency:
  - builds on `update-bpu-control-pc-tail-halfword`

## Validation Plan

验证策略沿用 `update-bpu-control-pc-tail-halfword` 的方案，只补 owner-migration
相关 witness，不新造另一套矩阵：

- `openspec validate update-bpu-control-pc-owner-migration --strict`
- `scons -j128 build/RISCV/gem5.opt`
- `scons -j128 --unit-test build/RISCV/cpu/pred/btb/test/btb.test.opt build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt`
- `./build/RISCV/cpu/pred/btb/test/btb.test.opt`
- `./build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt`

新增或更新的 directed witness 至少包括：

1. `Rvi4B_ControlPC_CrossBoundaryPredictInNextBlock`
   - 验证跨边界 4B control 的 taken owner 落到 following target
2. `SplitControlOwnershipMigratesBeforeBuildInst`
   - 验证 fetch 在 `buildInst()` 之前切换到拥有该指令的 target
3. `TakenMatchUsesOwnerTargetStartPC`
   - 验证删除 `trigger_covered` 后，taken 匹配仍只在 following target 生效
