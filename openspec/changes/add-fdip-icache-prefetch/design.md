# Design: FDIP (FTQ-directed ICache prefetch) — RTL-aligned

## Dependency update (2026-03-25)

本设计的 baseline 已从 `update-decoupled-btb-control-pc-views` 切换到
`update-bpu-control-pc-tail-halfword`：

- `startPC` 仍决定 fetch coverage；
- `controlPC` 决定 split 4B control 属于哪个 FTQ/predict block。

## Goals
- 在 decoupled frontend 下，基于 **FTQ** 提前预取 ICache cacheline，降低 ICache miss stall。
- 提供最小侵入的 **FTQ 只读 peek** 能力，供 FDIP / BTB-entry prefetch 复用。
- 让 FDIP 的核心语义更贴近 XiangShan RTL：`prefetchPtr/fetchPtr`、单周期 1 条预取仲裁、
  ITLB/PMP/mmio gate、redirect/`fence.i` 下的冲刷/丢弃行为（以可实现的 gem5 映射为准）。

## Non-goals (KISS/YAGNI)
- 不把 FDIP 变成新的取指主通路：FDIP 只做 best-effort 预取，绝不阻塞 demand fetch。
- 不在本变更中实现 XiangShan ICache 的完整流水（WayLookup/MetaArray/DataArray 的功耗建模等）。
  我们只对齐 **外部可观测** 的行为（请求来源/分类、指针推进、过滤/冲刷语义、资源上限、统计口径）。
- 不强制引入跨子系统的全局唯一 request-id；优先用 `(tid, epoch, ftqId/startPC)` 对齐生命周期。

## Implementation roadmap (phased)
本变更建议按 **Phase 0 -> Phase 1 -> Phase 1.5 -> Decision Gate -> Phase 2/3** 推进，
而不是一开始就把 gem5 ICache 改成完整 XiangShan RTL 形态。

### Phase 0: Baseline lock (must-have)
目标：先锁定 post-`tail-halfword control-pc-view` 的 demand-fetch 语义，避免把旧“固定 66B request span”噪声混入 FDIP 评估。

范围：
- 以 `update-bpu-control-pc-tail-halfword` 语义为唯一 baseline：
  - fetch request span 由实际 coverage window 决定
  - cross-boundary 4B control 的 trailing halfword 由 `controlPC` 所属 entry 的完整 coverage 自然决定
- 补齐 directed helper / helper-level witness：
  - coverage helper
  - cross-boundary 4B control-tail
  - redirect 后 partial state 清理
- 跑最小 trace / FS baseline 回归，记录稳定 witness

退出条件：
- demand fetch 的 request range 与 `startPC -> predEndPC` 语义一致
- 不再依赖旧式“固定 66B request span”作为研究前提

### Phase 1: First-order FDIP approximation (recommended first implementation)
目标：先回答“FTQ-directed ICache prefetch 是否能明显降低 I$ miss stall”。

范围：
- predictor-side `ftqPeek()` + `prefetchPtr`
- Fetch-local FDIP scheduler
- 按 **actual fetch coverage** 生成 cacheline prefetch 集合
- 请求类型采用 `Request::INST_FETCH | Request::PREFETCH`
- 加入 epoch / redirect / outstanding / issue-bandwidth 控制

显式不做：
- 不拆 tag/data 访问路径
- 不复用 tag/way lookup 结果给 demand
- 不改 cache 对外接口

这一阶段主要回答：
- lookahead 是否有效
- 收益是否主要来自 miss 提前启动
- 错误路径污染与竞争是否已经大到抵消收益

### Phase 1.5: Contention / pollution approximation
目标：在不重写 ICache 的前提下，先补齐最关键的“RTL 偏差项”。

范围：
- demand > prefetch 的明确仲裁与统计
- old-path prefetch 的 epoch mismatch 统计
- 可选 `fdip_drop_refill_on_epoch_mismatch`
- useful / late / unused / dropped / filtered 等统计补齐

这一阶段的意义：
- 若收益弱，能区分“FDIP 本身无效”还是“竞争/污染吃掉了收益”
- 为是否继续做更深 RTL 对齐提供证据

### Decision Gate
完成 Phase 1 / 1.5 后，必须先做 go/no-go 决策，再决定是否进入更重的 RTL 对齐阶段。

建议 gate：
- 若高 I$ 压力 workloads 上 `fetch.icacheStallCycles` 改善弱且 timeliness stats 不支持继续，
  则停止，不进入 Phase 2/3。
- 若 miss 路径收益明确，但 hit-path 仍明显落后 RTL 预期，再进入 Phase 2。

### Phase 2: Half-RTL alignment
目标：在不完全重写 cache 的前提下，开始显式建模 `TLB -> tag/way -> data` 的先后关系。

范围：
- 为 Fetch/FDIP 引入 tag-probe result / selected-way 的短生命周期状态
- demand 命中时尝试复用最近的 tag/way 结果
- 继续保持 cache 外部 API 尽量稳定，不立刻引入完整公开的 tag-only/data-only 两段接口

主要代价：
- squash/redirect 下需要维护更多生命周期状态
- partial line / second-line pending / redirect recovery 的状态复杂度明显上升

### Phase 3: Full RTL-aligned FDIP / ICache front-end interface
目标：真正对齐 XiangShan RTL 的前端 ICache micro-architecture，而不只是行为近似。

范围：
- ICache 对 fetch/FDIP 暴露两段式接口：
  - tag/way lookup
  - data read by selected way
- 建模 fetchMSHR / prefetchMSHR、tag/data 端口仲裁、old-path refill drop
- 让 FDIP 的主要收益来源从“提前启动 miss”扩展到“提前完成 TLB/tag/way 准备”

主要代价：
- 需要跨 `fetch` / predictor / `src/mem/cache/*` 做较大范围改造
- 状态机、验证、研究口径都会显著复杂化

结论：
- 默认研发路线应以 **Phase 1 / 1.5** 为主线；
- **Phase 2 / 3** 只有在研究目标明确要求 hit-path 周期级对齐时才值得推进。

## Key code facts (grounded in current xs-dev code)
- decoupled frontend 下 Fetch 以 FTQ head 的 `startPC` 作为取指起点发起 ICache access：
  - `src/cpu/o3/fetch.cc`: `sendNextCacheRequest()` 使用 `dbpbtb->ftqHead().startPC`
- Demand inst-fetch request 使用 `Request::INST_FETCH` 标记，并以 `MemCmd::ReadReq` 发往 ICache：
  - `src/cpu/o3/fetch.cc`: `handleMultiCacheLineFetch()` / `handleSuccessfulTranslation()`
- 旧版 `xs-dev` 曾隐式依赖 `fetchBufferSize=66` 的 overfetch；当前 post-`control-pc-view` 基线中，
  demand fetch 的 **request span** 已由 `currentFetchRequestSpan()` / `fetchCoverageSpan()` 按
  `startPC -> predEndPC` 的实际 coverage 计算，不再语义上固定为 66B：
  - `src/cpu/o3/fetch.cc`: `currentFetchRequestSpan()`
  - `src/cpu/pred/btb/common.hh`: `coverageEndPC()`
- `DecoupledBPUWithBTB` 的 FTQ 存储是 `std::deque<FetchTarget>`，并显式维护 head 指针：
  - `src/cpu/pred/btb/decoupled_bpred.hh`: `fetchTargetQueue`, `fetchHeadFtqId`
  - `src/cpu/pred/btb/decoupled_bpred.cc`: `consumeFetchTarget()` 推进 `fetchHeadFtqId`
- gem5 的 request typing 规则：`Request::PREFETCH` 会让 `Packet::makeReadCmd()` 选择 `MemCmd::SoftPFReq`；
  `HardPFReq` 是 cache 内建 prefetcher 框架使用的命令（不适合 CPU 直接发 I$ 预取）：
  - `src/mem/packet.hh`, `src/mem/cache/base.cc`
- `SoftPFReq` 在 cache miss 时会立即回一个 dummy resp 给上游，但 miss/fill 会继续异步推进：
  - `src/mem/cache/cache.cc`: “Software prefetch handling”

## RTL alignment anchors (XiangShan reference)
为了让 OpenSpec 的行为约束可落地且“更像 RTL”，本设计对齐以下 XiangShan ICache/FTQ 语义（来自 Design-Doc/RTL）：
- FTQ 同时维护 `fetchPtr` 与 `prefetchPtr`：复位/redirect 时二者对齐；每次“成功发送”取指/预取请求时各自 `++`。
- PrefetchPipe 每周期最多处理/向下游发射 1 条预取（双行时需仲裁分两拍）。
- 预取先过 ITLB，再做 PMP/PMA/mmio(pbmt) gate；异常/mmio 时不向 MissUnit 发预取。
- MissUnit 有独立的 `fetchMSHR/prefetchMSHR`，整体优先级：fetch > prefetch；prefetchMSHR 采用 FIFO 维护请求顺序。
- redirect/`fence.i` 冲刷下：in-flight grant 返回时可丢弃 refill（不写 SRAM、不产生污染）。

## Proposed architecture (gem5 mapping)

### 1) FTQ interface & pointers (predictor-side)
在 `DecoupledBPUWithBTB` 内新增一个 **独立于 fetchHead 的 prefetch head 指针**，用于 FDIP：
- `FetchTargetId prefetchHeadFtqId`：初始化与 `fetchHeadFtqId` 相同

同时保留/新增两类最小接口：

1) **只读 peek（给 FDIP/BTB-entry prefetch 的“看未来”能力）**
- `bool ftqPeek(int offset, const FetchTarget *&out) const;`
  - `offset=0` 读 `fetchHeadFtqId`（不改变 supply/advance 状态）
  - `offset>0` 读未来 entry（越界返回 false）

2) **prefetch head（贴近 RTL 的 `prefetchPtr` 语义）**
- `bool ftqHasPrefetchHead() const;`
- `FetchTargetId ftqPrefetchHeadId() const;`
- `const FetchTarget &ftqPrefetchHead() const;`
- `void consumePrefetchTarget();`（仅推进 `prefetchHeadFtqId`，不影响 `fetchHeadFtqId`）

Redirect/squash 行为（对齐 RTL）：
- 每当 `fetchHeadFtqId` 因 squash/redirect 被重置/跳转时，`prefetchHeadFtqId` 同步对齐到新的 `fetchHeadFtqId`
  （或对齐到 redirect target 的下一 entry，按现有 `consumeFetchTarget` 语义落地）。

Prefetch distance 上限（对齐 WayLookup depth/backpressure 概念）：
- 将 `fdip_lookahead_entries` 定义为 **`prefetchHeadFtqId - fetchHeadFtqId` 的最大允许距离**（默认 0/关闭）。

### 2) FDIP engine (Fetch-local, drives predictor prefetch head)
FDIP 位于 `o3::Fetch`，并通过上述 “prefetch head API” 驱动 `prefetchHeadFtqId` 推进。

关键约束（与 `update-decoupled-btb-control-pc-views` 对齐）：
- FDIP 的 per-entry coverage 必须与 demand fetch 使用同一语义：
  - 取指窗口是该 FTQ entry 的 **实际 request coverage**
  - 不是“固定 66B overfetch window”
- 对跨边界 4B RVI control，若当前 FTQ entry 的 `predEndPC` 已因 control-tail coverage 延长，
  FDIP 也必须把这部分尾部 line 视为当前 entry 的覆盖范围；反之，不得靠旧 overfetch 假设偷偷带回。

行为约束（贴近 RTL、但保持 gem5 可实现）：
- **带宽模型（对齐 PrefetchPipe arbiter）**：默认每周期最多发出 1 条 **cacheline 粒度** 的 prefetch请求。
  双行覆盖时分两拍发射（第二条线可在下一拍发出）。
- **不阻塞 demand**：FDIP 只能用“剩余的端口/周期”做 best-effort；demand 取指在同周期有工作时优先。
- **过滤逻辑**：prefetch 必须先完成 ITLB 翻译；若翻译 fault 或最终请求为 uncacheable/mmio（由 Request flags 表示），
  则该 cacheline 预取被过滤（不向 ICache 发请求），并计数。
- **指针推进**：仅当某个 FTQ entry 所需的 cacheline（按覆盖规则）都“已发出”或“已被过滤”时，才 `consumePrefetchTarget()`。

请求构造（demand-distinguishable 且 inst-fetch 语义一致）：
- 使用 `Request::INST_FETCH | Request::PREFETCH` 标记，使其在 mem/cache 层被识别为：
  - instruction fetch 类型（inst）
  - prefetch 类型（SoftPFReq）

资源上限（对齐 prefetchMSHR 容量的抽象）：
- `fdip_max_outstanding` 以 **cacheline** 为单位限制 FDIP 在途的预取请求数（best-effort；满则停发/丢弃）。

### 3) Squash/redirect & refill pollution policy
Epoch tagging（生命周期）：
- FDIP 为每条发出的 prefetch line 记录 `(tid, epoch, ftqId/startPC)`，用于统计与 byte buffer（若启用）。

Redirect/squash 下的两种模式（用于对齐 RTL vs gem5 现实差异）：
- **Base（默认）**：不改变 cache 行为，允许 wrong-path prefetch 继续完成并可能污染 ICache；
  FDIP 只在自己的 bookkeeping 中将其归类为 old-path（epoch mismatch）并忽略其 bytes（若有）。
- **RTL-aligned（可选）**：提供一个 `fdip_drop_refill_on_epoch_mismatch`（或等价）开关，使得 old-path
  的 FDIP prefetch refill 不安装到 ICache（以更贴近 XiangShan “flush 丢弃 refill” 语义）。
  该模式需要 cache-side 最小支持（见 Tasks），默认关闭以保持最小侵入。

另提供一个轻量开关控制 “未发出/半完成”的清理：
- `fdip_flush_partial_on_epoch_change`：redirect 时清除 FDIP 当前 FTQ entry 的“第二条线待发”等 partial state。

### 4) Optional byte buffer (for BTB-entry prefetch reuse)
当 `fdip_enable_byte_buffer` 开启时：
- 缓存键建议：`(tid, epoch, ftqId)` 或 `(tid, epoch, startPC_aligned)`
- 查询只返回“完整窗口”或“miss”（complete-window-or-miss），避免部分 bytes 导致错误 predecode

注意：byte buffer 不是 XiangShan FDIP 的必需组件；在本项目中它主要服务于后续 BTB-entry prefetch 的研究接口复用。

## Stats (minimal)
- `fdipIssuedLines`: 发出的 prefetch 请求数（按 cacheline 粒度）
- `fdipDropped`: 因带宽/端口/queue/outstanding 限制而未能发出的次数（可选 reason-code）
- `fdipFilteredFault`: 因翻译 fault 被过滤的次数
- `fdipFilteredUncacheable`: 因 uncacheable/mmio 被过滤的次数
- `fdipOutstandingMax`: outstanding 峰值（cacheline 粒度）
