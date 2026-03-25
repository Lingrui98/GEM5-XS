# Design: Proactive BTB-entry prefetch (decode-ahead)

## Dependency update (2026-03-25)

本设计已从旧的 `update-decoupled-btb-control-pc-views` 语义切换到
`update-bpu-control-pc-tail-halfword`：

- `startPC` 负责 fetch-window bytes 覆盖；
- `controlPC` 负责 BTB key/index/tag/position。

## Goals
- 基于预测路径（FTQ lookahead）提前构建/预热 BTB 表项，提升 BTB coverage、降低冷启动 miss。
- 通过轻量预译码支持 direct CFI（RISC-V branch/jal 等）的 target 解析，避免依赖执行。
- 对 wrong-path 污染提供可观测/可控的研究接口（参数 + 统计）。

## Non-goals (KISS/YAGNI)
- 不在初版支持 indirect target 预测的预填充（无法从静态字节确定 target，先跳过）。
- 不把 BTB-entry prefetch 变成训练/更新主通路：prefill 是 best-effort，受限带宽，不影响功能正确性。
- 不强制要求 FDIP byte buffer 一定开启：若 bytes 不可用则按策略降级（默认直接不工作）。

## Key code facts (grounded in current code)
- decoupled BTB predictor 维护 FTQ/FSQ，并向 Fetch 供给 `FtqEntry`：
  - 设计文档：`src/cpu/pred/btb/docs/decoupled_bpred.md`
  - FTQ：`src/cpu/pred/btb/fetch_target_queue.hh`
- BTB 层次结构与预测/更新路径已存在（UBTB/ABTB/MBTB 等）：
  - 文档：`src/cpu/pred/btb/docs/btb.md`
  - S3 预测回填 UBTB/ABTB：`src/cpu/pred/btb/decoupled_bpred.cc:267`
- post-`tail-halfword control-pc-view` 基线已明确 control 指令的两类视图：
  - byte/decode identity = `startPC`
  - predictor identity = `controlPC`
  该语义要求任何 BTB prefill 都以 `controlPC` 为 key/index/tag/position，
  同时保留 `startPC` 处理完整指令 bytes coverage。

## Proposed architecture

### 1) Dataflow overview
1. 通过 FTQ peek API 获取未来若干个 fetch blocks（`btb_prefetch_lookahead_entries`）。
2. 对每个 block：
   - 从 FDIP byte buffer 查询该 FTQ entry 的 **实际 fetch coverage window**（若 miss 则跳过或等待）。
   - 轻量预译码识别 direct CFI，计算 direct target。
   - 构造 `BTBEntry` 列表（按 predictor-visible `controlPC` 顺序）。
3. 以受限带宽将 `BTBEntry` 写入目标 BTB 结构（优先 MBTB）作为“预填充”。

### 2) Predecode strategy (RISC-V)
初版仅支持能静态解析 target 的 direct CFI：
- RVI: `JAL`, `JALR`（JALR target 依赖寄存器，默认跳过）, `BRANCH`（BEQ/BNE/...）
- RVC: 压缩跳转/分支（若实现成本可控）

输出信息：
- `pc`（predictor-visible `controlPC`）
- `startPC`（显式保留的指令起始地址）
- `isCond/isCall/isReturn/isIndirect`（按可判定信息填充）
- `target`（仅 direct 可确定时填充）

额外约束：
- 对跨边界 4B control，只有当当前 FTQ entry 的 window 已覆盖完整指令字节时，才允许生成 `BTBEntry`；
- `startPC` 决定 bytes window 是否完整，`controlPC` 决定 BTBEntry 的 canonical key。

### 3) Prefill policy (BTB mutation)
写入策略遵循 KISS：
- 每周期最多插入 `btb_prefetch_insert_per_cycle` 条表项；
- 只对 direct CFI 预填充；
- 目标结构优先主 BTB（例如 MBTB），避免对小容量 L0 结构造成过度污染；
- 提供 `btb_prefetch_allow_wrongpath_pollution` 开关：
  - True：允许 wrong-path prefill 持久化（贴近真实硬件）
  - False：提供“no-pollution 对照”，保证 wrong-path 产生的 prefill 不影响后续正确路径预测（研究用；需定义可实现语义，见下）

### 4) Squash/epoch semantics
- predecode 与 prefill 都以 `(tid, epoch)` 绑定生命周期。
- wrong-path prefill 的处理受 `allow_wrongpath_pollution` 控制：
  - 允许污染：prefill 写入不回滚，但统计需能区分来源 epoch；
  - 不允许污染：需要提供一个不会破坏主 BTB 状态、但能实现“no-pollution”对照的机制（推荐 epoch overlay；见下）。

## Wrong-path policy: recommended implementation semantics
为了让 `btb_prefetch_allow_wrongpath_pollution=false` 成为一个**可验证**的对照（而非“丢弃未来工作”这种弱语义），
推荐采用一个 KISS 的实现语义：

### Epoch overlay (recommended)
- 增加一个小容量的“prefill overlay”（可以是 per-thread、per-epoch 的旁路结构）：
  - prefill 写入 overlay（而非直接写入 MBTB）
  - 预测时 overlay 优先于 MBTB 查询
  - 当对应 epoch 被 squash/redirect 时，直接丢弃 overlay（实现 no-pollution）
  - 当 epoch 被确认（例如进入 correct-path 稳态/满足条件）时，可选择性将 overlay 合并写入 MBTB（可选）

该方案的研究价值：
- `allow=true`：prefill 直接写 MBTB（真实污染）
- `allow=false`：prefill 写 overlay，squash 时丢弃（no-pollution 对照）
二者差异清晰且易于观测/统计。

## Metrics definitions (to make results explainable)
为避免 “inserted 很多但不知道是否有用”，建议把统计口径锁死为可计算定义：

### Useful-hit (minimum)
当某条 BTBEntry 满足以下条件时记为 useful：
1. 最近一次写入来源为 prefill（可用 per-entry last-writer 标记实现：`origin=prefill`）
2. 在被 demand update 覆盖或被替换前，至少一次被 demand lookup 命中

对应统计建议拆分为：
- `btbPrefetchInserted`（已存在）
- `btbPrefetchUsefulHit`（新增：useful-hit）
- `btbPrefetchUnusedEvicted`（新增：prefill 写入但未被 demand 使用即被替换/覆盖）

### Wrong-path impact (minimum)
仅统计 “wrongpathInserted” 不足以支持权衡结论，建议新增两个最小归因统计：
- `btbPrefetchWrongpathHitOnCorrect`：wrong-origin entry 在后续 correct-path demand lookup 被命中次数（可能是收益）
- `btbPrefetchHarmfulWrongpathUse`：wrong-origin 命中后导致 overrideTargetMismatch/control squash 等明显负效应的计数（成本）

## Trace note: predecode correctness
trace 主路径下 bytes 可能来自“trace 指令定义 -> RISC-V 编码”。严格说明：
- `TraceFetch::createMachInstFromTrace()` 提供的是 per-instruction 的 2B/4B `MachInst` 编码能力；
  BTB-entry predecode 需要的是按 FTQ entry 对齐的 **fetch-window bytes**，因此需要一个确定性的 window assembler
  将连续 trace 指令的 `MachInst` 按固定规则拼接成窗口，并遵循 **complete window or miss** 语义。
- 对于有损/退化编码（例如 RV64 下 compressed call 退化为 `c.nop`、out-of-range immediate 的 clamp/fallback），
  window assembler 应返回 miss（或至少显式计数并作为威胁有效性讨论），避免把“编码工件”误归因于微结构效果。

为避免把编码/预译码错误当作 wrong-path 污染：
- predecode 单元测试必须覆盖：RVI direct CFI（branch/jal）与（若启用）RVC direct CFI
- 建议增加一个 `btbPrefetchPredecodeMismatch` 统计（预译码结果与 commit/trace ground-truth 不一致的计数），用于界定“实现错误”与“机制权衡”的边界

## Stats (minimal)
- `btbPrefetchBlocks`: 预译码的 fetch blocks 数
- `btbPrefetchDecodeMiss`: 因 bytes 不可用导致跳过的块数
- `btbPrefetchDiscoveredCFI`: 发现的 direct CFI 数
- `btbPrefetchInserted`: 实际插入 BTBEntry 数
- `btbPrefetchUseful`: 后续 demand 命中（或 BTB miss 降低）的计数（定义需实现中明确）
- `btbPrefetchWrongpathInserted`: wrong-path 插入计数（用于污染评估）

## Integration note (overlap with FDIP)
本变更尽量只依赖 FDIP 的稳定接口：
- FTQ peek：获取 lookahead blocks
- byte query：获取完整指令字节窗口（complete-window-or-miss，且窗口语义与当前 FTQ entry 的实际 fetch coverage 对齐）

为减少两个 worktree 的冲突，建议由 FDIP 变更落地接口定义（header/ABI），BTB-entry prefetch 在其上实现。
