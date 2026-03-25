# Change: Add FDIP (FTQ-directed ICache prefetch) for decoupled frontend

## Dependency update (2026-03-25)

本 change 现在默认建立在 `update-bpu-control-pc-tail-halfword` 的语义上：

- `startPC` 负责 fetch coverage 与指令字节拼装；
- `controlPC` 决定跨块 4B control 属于哪个 FTQ / predict block。

## Why
在当前 decoupled frontend 中，Fetch 以 FTQ 为取指驱动（每个 FTQ entry 对应一次取指窗口），
且在采用 `update-bpu-control-pc-tail-halfword` 语义之后，当前仓库的 demand fetch request span
已按 **实际 coverage window** 计算：仍保留 66B fetch buffer 作为上限/容量，但 request span
不再隐式固定为 66B；对于跨边界 4B RVI control，窗口还可能因 `controlPC` 所属 entry 的完整指令覆盖而自然延长到尾部。
在这一语义下，ICache miss 仍然会直接导致前端停顿。

另外，本仓库的建模目标是尽可能贴近 XiangShan RTL 的前端/ICache 行为。XiangShan 的 FDIP/ICache
在 micro-arch 上具有一些关键语义（例如 `prefetchPtr/fetchPtr`、单周期 1 条预取仲裁、ITLB/PMP/mmio
过滤、redirect/`fence.i` 下丢弃 refill 避免污染）。若 OpenSpec 的 FDIP 方案与这些语义偏离过大，
后续在 `xs-dev` 主线落地会更难对齐与复现研究结论。

## What Changes
本变更引入一个 **默认关闭** 的 FDIP（Fetch Directed Instruction Prefetch）框架：

1. **RTL-aligned 的 ICache 预取引擎（FDIP engine）**
   - 位置：O3 Fetch（decoupled frontend 路径）。
   - 行为：基于 FTQ 的 **`prefetchPtr/fetchPtr` 语义** 驱动预取：
     - 在 predictor/FTQ 内维护独立的 `prefetchPtr`（复位/redirect 对齐 `fetchPtr`）；
     - FDIP 每次只处理 `prefetchPtr` 指向的一个 FTQ entry，并按该 entry 的 **实际 fetch coverage window**
       （而非固定 66B overfetch 假设）向 ICache 发起 cacheline 粒度 inst-fetch 预取；
     - 默认 **每周期最多发出 1 条 cacheline 预取**（双行覆盖时分两拍），以贴近 RTL 的仲裁约束；
     - 仅当该 FTQ entry 所需 cacheline 都“已发出”或“被过滤”（fault/uncacheable/mmio）时，`prefetchPtr++`。
   - 约束：对每周期带宽、最大 outstanding、队列大小做上限控制；在资源不足时丢弃/推迟预取，
     **不得阻塞 demand fetch**（KISS：prefetch 不能影响正确性路径）。
   - squash/redirect：通过 epoch（或等价机制）标识生命周期，squash 后旧 epoch 的 FDIP
     outstanding/bytes 必须被丢弃或忽略，避免跨生命周期误用。
   - 可选（RTL-aligned）：提供一个“redirect/flush 下不安装 old-path refill”模式，用于更贴近
     XiangShan ICache 的冲刷语义；默认关闭以保持最小侵入与向后兼容。

2. **最小化的 FTQ peek API（供 FDIP/后续 BTB-entry prefetch 复用）**
   - 为 decoupled predictor（至少 `DecoupledBPUWithBTB`）提供“按 offset 读取 FTQ entry”
     的只读接口，不改变 FTQ 供给状态（不 pop / 不 advance）。
   - 目的：避免 FDIP 通过侵入式方式访问 predictor 内部队列，保持 SRP/OCP。

3. **参数、统计与文档**
   - 增加 enable/limit 参数（默认全关闭）。
   - 增加关键 stats（issued/dropped/useful/squashed 等）。
   - 文档说明：启用方式与参数语义。

## Impact
- `src/cpu/o3/fetch.*`：新增 FDIP engine 的调度点与 squash 生命周期管理。
- `src/cpu/pred/btb/decoupled_bpred.*`：新增 FTQ peek + `prefetchPtr`（prefetch head）及 redirect 对齐语义。
- `src/mem/*`：必要时为 inst-fetch prefetch 请求增加最小标记/区分（SoftPFReq 等）；可选地增加 “old-path refill drop” 支持。
- 配置脚本与文档：新增参数说明与示例。

## Acceptance Criteria
1. **默认不变**：FDIP 默认关闭时，功能/性能/统计与现状一致。
2. **不影响正确性路径**：FDIP 开启后，在资源不足时可以丢弃/推迟预取，但不得阻塞 demand fetch。
3. **squash 安全**：squash/redirect 后，FDIP 不会把旧路径产生的状态误用于新路径（通过 epoch 等机制保证）。
4. **可观测性**：提供最小但足够的统计，能定位“预取发出/被丢弃/被 squash/命中带来收益”等。
5. **coverage 对齐**：在 post-`update-bpu-control-pc-tail-halfword` 基线上，FDIP 必须以每个 FTQ entry 的实际 fetch request
   coverage（`[startPC, predEndPC)` 及其自然延伸）为准；跨边界 4B control 的 trailing halfword 不能依赖旧式固定 66B overfetch 才被预取。

## Execution orchestration contract

为支持 agent / 子 agent 低干预并行推进，本提案要求验证阶段额外提供一个**机器可读的 CSV manifest**，
作为任务编排真源；自然语言 checklist/tasks 仅作审阅辅助，不作为唯一执行依据。

- 建议路径：
  - `openspec/changes/add-fdip-icache-prefetch/validation/tasks.csv`
  - `openspec/changes/add-fdip-icache-prefetch/validation/env.example.sh`
- 每行代表一个原子任务，至少包含：
  - `task_id`
  - `stage`
  - `deps`
  - `kind`
  - `workdir`
  - `command`
  - `artifacts`
  - `pass_criteria`
  - `status`
  - `notes`

推荐依赖图：
- `setup_env`：固化环境变量、baseline 路径、trace/FS 配置
- `build_unit <- setup_env`
- `trace_smoke_off <- build_unit`
- `trace_smoke_on <- build_unit`
- `fs_smoke_off <- build_unit`
- `fs_smoke_on <- build_unit`
- `trace_hi_ipress_a <- trace_smoke_off;trace_smoke_on`
- `trace_hi_ipress_b <- trace_smoke_off;trace_smoke_on`
- `final_verify <- 所有前述任务进入终态（passed/failed/blocked）`

失败策略要求：
- `build_unit` 失败：立即阻塞所有后续任务
- 任一 smoke 失败：对应研究批次进入 `blocked`，但 `final_verify` 仍需收敛并汇总
- distributed trace 任务允许一次自动重试；重试后仍失败再升级人工介入

产物约定要求：
- 每个任务目录至少包含：
  - `log.txt`
  - `status.json`
  - `summary.txt`
- 顶层汇总至少包含：
  - `final_status.csv`
  - `final_report.md`

## Research questions & falsifiable hypotheses
本 proposal 既是工程骨架，也是研究用基础设施。为了保证结论可证伪/可复现，本变更将显式回答：

### Research questions
1. 在 decoupled frontend（FTQ 驱动）下，FTQ-lookahead 的 ICache prefetch（FDIP）是否能显著降低 ICache miss stall（例如 `fetch.icacheStallCycles`）？
2. FDIP 的代价是什么（额外流量、MSHR/端口竞争、污染、wrong-path 流量），是否会抵消收益或造成回归？
3. 收益来自 coverage 还是 timeliness（prefetch 是否“及时”），其上限主要受 lookahead 可用性还是资源约束影响？

### Hypotheses (falsifiable)
- **H1 (benefit)**：在 ICache-miss-sensitive traces 上，启用 FDIP 后 `fetch.icacheStallCycles`（或其占比）显著下降；若不下降或上升则否证。
- **H2 (timeliness-dominated)**：收益主要来自 timely prefetch（fill 早于首次 demand）；若大多数 prefetch 为 late/unused，则收益不可持续。
- **H3 (contention cost)**：在无 guard 的情况下，FDIP 可能增加 `fetch.icacheWaitRetryStallCycles` 或 L1I/L2 竞争；若开/关 guard 可显著改变收益/代价，则竞争是关键瓶颈。
- **H4 (wrong-path tradeoff)**：lookahead 深度越大 wrong-path 占比越高，存在净收益拐点；若无拐点需用统计证明 wrong-path 成本确实可控。

## Evaluation plan (minimal matrix)
主路径为 trace。为了保证对照公平性：**baseline 与 variant 使用相同 trace、相同 cache 层次、相同 warmup/ROI 口径**；FDIP 仅作为增量开关。

### Baselines
- **B0**：FDIP=off（主对照）。
- **B1（建议）**：一个“非 FTQ 定向”的简单 ICache prefetch 对照（若系统已有则启用现有 ICache prefetcher；否则实现/启用一个最小 next-line distance=1 对照）。

### Ablations / sweeps (KISS 版)
- `fdip_lookahead_entries ∈ {1, 2, 4, 8, 16, 32}`（prefetchPtr 距离上限；对齐 WayLookup depth/backpressure 的抽象）
- `fdip_issue_bandwidth ∈ {1, 2}`
- `fdip_max_outstanding ∈ {2, 8}`
- 实际 fetch coverage 跨线策略（需设计里明确）：
  `prefetch_lines_per_ftq ∈ {start_line_only, cover_actual_fetch_range}`
- redirect 行为：`fdip_flush_partial_on_epoch_change ∈ {0, 1}`（影响“半完成/第二条线待发”的 partial state）
- 污染策略（RTL-aligned 对照）：`fdip_drop_refill_on_epoch_mismatch ∈ {0, 1}`

### Stop criteria
- 若在“高 ICache 压力”子集上 `median(Δ fetch.icacheStallCycles)` 绝对值 <2% 且无可解释的 timeliness/coverage 提升，则认为 FDIP 在当前架构下不值得继续复杂化（否证 H1）。
- 若 FDIP 仅在极少数 traces 获益且伴随明显竞争/污染代价（例如 L1I/L2 流量显著上升、`icacheWaitRetry` 上升），则优先研究 guard/优先级策略，而不是扩大 lookahead。

## Enable/Disable
新增一组 FDIP 参数（默认 False/0），例如：
- `enable_fdip`
- `fdip_lookahead_entries`（prefetchPtr 距离上限）
- `fdip_max_outstanding`
- `fdip_issue_bandwidth`
 - `fdip_flush_partial_on_epoch_change`
 - `fdip_drop_refill_on_epoch_mismatch`（默认 False；RTL-aligned 对照）
