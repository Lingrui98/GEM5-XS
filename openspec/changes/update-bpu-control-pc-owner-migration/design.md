# Design: fetch ownership migration for tail-halfword control-PC semantics

## Context

在 `update-bpu-control-pc-tail-halfword` 基线上：

- predictor-visible `pc` 已经是 `controlPC`
- 跨边界 4B control 只会在 trailing-halfword 所在预测块里产生 taken 预测
- 但 fetch 仍默认“当前 FTQ target 拥有当前 decode 的指令”，并靠
  `trigger_covered` 防止前一个 target 提前 redirect

这样做能工作，但 ownership 仍不统一：

- `BranchInfo`/BTB/TAGE 看见的是 following target 的 branch
- `buildInst()` 生成的 DynInst 和 `ftqId` 却还是前一个 target 的
- `lookupAndUpdateNextPC()` 只能通过 coverage gate 来补这个语义裂缝

## Goals / Non-Goals

- Goals:
  - 让 split taken control 的 DynInst ownership 与 predictor ownership 一致
  - 删除 fetch taken redirect 对 `trigger_covered` 的依赖
  - 保持 stats / trace 继续使用架构 `startPC`
  - 尽量不扩大 fetch/decoder 状态机的复杂度
- Non-Goals:
  - 不重新设计 decoder 增量喂码接口
  - 不改变 predictor 内部 `controlPC` key 语义
  - 不改变非跨界 control instruction 的 FTQ ownership

## Decisions

### 1. `FetchTarget` 显式暴露 `decodeStartPC`

每个 `FetchTarget` 新增一个 `decodeStartPC`，表示“从哪条架构指令开始，这个
target 才真正拥有 decode/buildInst 的归属”。

- 默认情况下：`decodeStartPC == startPC`
- 仅当 following target 携带一个跨边界且 predicted-taken 的 split control 时：
  - `decodeStartPC = predBranchInfo.startPC()`

这使得“trailing halfword 属于谁”和“这条 DynInst 属于谁”统一到同一个显式字段，
而不是散落在 fetch coverage 推理里。

### 2. fetch 在 `buildInst()` 前迁移 owner target

`Fetch::processSingleInstruction()` 在为当前 `pc.instAddr()` 构造 DynInst 之前：

1. 读取当前 `ftqFetchingTarget()`
2. 如果 `curr_pc < target.decodeStartPC`
3. 说明当前 target 不拥有这条指令
4. 先 `consumeFetchTarget()` 切到 following target
5. 再继续 `buildInst()`

这样 `buildInst()` 绑定的 `ftqId`、后续 commit/squash 使用的 fetch target，
都会自然落到 following target。

### 3. taken matching 改成 owner-target `startPC` 匹配

owner migration 生效后，`lookupAndUpdateNextPC()` 不再需要
`trigger_covered` 这个 coverage gate。

新的 taken 规则是：

- 当前 target `predTaken == true`
- 当前 DynInst 的 `startPC == target.predBranchInfo.startPC()`

满足时直接 taken redirect。

对于跨边界 4B control：

- 前一个 target 因为不再拥有这条指令，不会构造该 DynInst，也就不会 taken
- following target 构造这条指令时，`startPC` 恰好与 `predBranchInfo.startPC()`
  相等，因此只在这里 taken

### 4. stats/debug 继续使用 `startPC`

owner migration 只改变微架构所有权，不改变用户可读统计的 branch key。

- stats branch key 继续是架构 `startPC`
- `controlPC` 只用于 predictor 内部索引、tag、position、匹配

这样既满足新语义，也保留原有调试和性能归因的可读性。

## Risks / Trade-offs

- fetch target 的消费时机提前到 `buildInst()` 之前，必须确保普通 target
  不会被误消费；缓解方式是只在 `curr_pc < decodeStartPC` 时迁移。
- owner migration 改变了 DynInst 绑定的 `ftqId`，因此必须补 directed test
  覆盖 split control case。
- `trigger_covered` 删除后，taken 判断完全依赖 owner target 和 `startPC`
  匹配；这要求 BPU 构造 target 时正确设置 `decodeStartPC`。

## Migration Plan

1. 给 `FetchTarget` 增加 `decodeStartPC`，并在 BPU 生成 following target 时填充。
2. 在 fetch `processSingleInstruction()` 前半段加入 owner migration。
3. 删除 `lookupAndUpdateNextPC()` 的 `trigger_covered` 逻辑，改为 owner-target
   `startPC` 匹配。
4. 跑 unit test / build smoke，确认 stats/debug 仍使用 `startPC`。
