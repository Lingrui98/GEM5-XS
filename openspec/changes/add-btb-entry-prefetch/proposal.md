# Change: Add proactive BTB-entry prefetch (decode-ahead) for decoupled BTB

## Dependency update (2026-03-25)

本 change 现在默认建立在 `update-bpu-control-pc-tail-halfword` 的语义上：

- `startPC` 用于字节窗口与完整指令覆盖；
- `controlPC`（尾半字节起始地址）用于 BTB 的 key/index/tag/position。

## Why
我们希望研究/实现一种“主动 BTB 表项预取”机制：沿着预测路径提前拿到未来 fetch block 中的
控制流指令（CFI）信息，并在真正 demand 访问 BTB 之前，把对应 BTB 表项预先填充/预热，从而：
- 降低 BTB 冷启动 miss（coverage）；
- 隐藏 BTB 访问/更新延迟（特别是主 BTB 层次）；
- 评估 wrong-path 污染对 BTB 的影响与收益。

该类 BTB 预取通常需要对未来 fetch block 的指令字节做预译码（至少识别 direct CFI 并解析 direct target），
因此需要依赖 FDIP 提供 FTQ lookahead 与（可选）指令字节获取能力。

## What Changes
本变更引入一个 **默认关闭** 的 BTB-entry prefetch 框架（prefill / warmup）：

1. **BTB-entry prefetch 引擎（基于 FTQ lookahead）**
   - 沿预测路径 lookahead 若干个 FTQ entry（深度可配），对每个 entry 的 **实际 fetch coverage window**
     做预处理，而不是假设一个固定 66B 的预译码窗口。

2. **预译码（predecode）与 direct target 计算**
   - 使用 FDIP 的 prefetched-byte query（若可用）对实际 fetch coverage window 做轻量预译码：
     - 识别 direct control-flow（例如 RISC-V branch/jal 及其压缩形式）；
     - 计算 direct target；
     - 构造 BTBEntry（pc/type/target/...）。
   - 预译码得到的 BTB 条目必须显式区分 `startPC` 与 `controlPC`：
     `startPC` 负责 bytes coverage / 完整性判断，`controlPC` 才是 BTB 存储/索引 key；
     不得再把 split 4B control 退化回 “只存 startPC” 的旧语义。
   - 对跨边界 4B direct CFI，bytes provider 必须遵循 **complete window or miss** 语义：
     只有当当前 FTQ entry 的完整控制指令字节都在窗口里可见时，才允许生成可写入的 BTBEntry。
   - 对无法确定 target 的类别（如 indirect）默认跳过或按策略处理（KISS：先跳过）。

3. **BTB 表项预填充/预热策略**
   - 将预译码得到的 BTBEntry 以受限带宽/容量写入指定 BTB 结构（优先主 BTB，如 MBTB）：
     - 每周期最大插入条数；
     - 可选：仅对某些分支类型启用（uncond/call/return/cond）。
   - 污染控制：提供开关决定是否允许 wrong-path prefill 污染（默认允许以贴近真实硬件；但可统计与可控）。

4. **参数、统计与文档**
   - 新增 enable/limit/类型过滤参数（默认关闭）。
   - 新增统计：预译码块数、发现 CFI 数、插入 BTBEntry 数、后续命中率/覆盖率等。
   - 文档说明：与 FDIP 的依赖关系、降级行为（无 bytes 时不工作或只做 tag-only warmup）。

## Impact
- `src/cpu/pred/btb/*`：增加 BTB-entry prefetch 引擎与 prefill 写入路径（优先 MBTB）。
- `src/cpu/o3/fetch.*` / FDIP：通过稳定接口获取 prefetched bytes（依赖 change `add-fdip-icache-prefetch`）。
- ISA 相关：增加 RISC-V 轻量预译码辅助（仅用于识别 direct CFI 与计算 direct target）。

## Acceptance Criteria
1. 默认关闭时行为不变。
2. 开启后能在不影响功能正确性的前提下，向 BTB 结构预填充 direct CFI 的表项（受限带宽/容量）。
3. squash/redirect 不会导致跨生命周期误用（通过 epoch 等机制保证）。
4. 提供足够统计评估收益与污染（correct/wrong-path 维度）。
5. 对 split 4B direct CFI：
   - 仅在完整控制指令字节可见时才允许生成 BTBEntry；
   - BTB 存储/索引 identity 使用 `controlPC`，同时保留 `startPC` 以支撑 bytes window 与调试归因。

## Research questions & falsifiable hypotheses

### Research questions
1. 在 decoupled BTB 前端中，BTB-entry prefill/warmup 是否能提升 BTB coverage（降低冷启动 miss），并带来可观测的前端收益（IPC 或控制相关 stall/squash 降低）？
2. 预译码 direct-only 的设计上限是什么（indirect 占比高时是否几乎无效）？
3. wrong-path prefill 污染的净效应如何：它是“成本”（错误条目/替换/干扰）还是在某些场景可复用为“收益”？是否存在可调拐点？

### Hypotheses (falsifiable)
- **H1 (coverage/warmup)**：在 bytes 可用且 direct-only 的边界内，开启 prefill 后 MBTB/整体 BTB 的 miss-rate 显著下降，且 IPC/前端控制相关 stall 有净改善；若无改善则否证。
- **H2 (first-touch causality)**：收益主要来自“首次遇到某些静态 direct CFI 时” demand lookup 前已存在 entry（warmup 速度提升），而不是长期稳态偶然波动；若 first-touch 命中率不变则否证。
- **H3 (wrong-path tradeoff)**：随着 lookahead 深度/插入带宽增大，wrong-path 插入比例上升，净收益出现拐点；若无拐点则需证明 wrong-path 成本确实低（例如 harmfulWrongpathUse 接近 0）。

## Mechanism / causal chain (what must be observed)
为避免“只看 IPC 难以归因”，本研究要求同时观测中间变量链路：
- FTQ lookahead 可用性 -> bytes window 命中率（bytes provider 命中） -> predecode 发现 direct CFI 数与正确率 -> bounded prefill 写入量（按结构） ->
  demand lookup hit-stage 分布变化（按 UBTB/ABTB/MBTB） -> miss/override/squash 的变化 -> IPC / frontend stall。

## Evaluation plan (minimal matrix)

### Baselines
- **B0**：BTB-entry prefetch=off（主对照），其它条件相同（包括 bytes provider/FDIP 是否开启）。

### Ablations / sweeps (KISS 版)
- bytes provider：`bytes_available ∈ {0, 1}`（用于验证“bytes 不可用 -> 安全降级且不产生虚假统计”）
- `btb_prefetch_lookahead_entries ∈ {1, 2, 4, 8}`
- `btb_prefetch_insert_per_cycle ∈ {1, 2, 4}`
- 分支类型过滤：`types_mask ∈ {uncond_only, uncond+call+ret, all_direct_including_cond}`
- 目标结构：`target_struct ∈ {MBTB, (optional) ABTB/UBTB}`（先 MBTB，证明有效后再扩展）
- wrong-path 策略：`btb_prefetch_allow_wrongpath_pollution ∈ {True, False}`（需要在设计里落地可实现语义）

### Stop criteria
- 若在 direct-heavy 子集上 coverage 改善不显著、或 IPC 无提升/退化，同时 bytes-hit 率较高，则认为机制本身不足（否证 H1）。
- 若收益仅出现在极短冷启动窗口，需追加“周期性 flush/phase 切换”的定向实验以证明 H2（否则不进入更复杂实现）。
- 若 harmfulWrongpathUse 随 lookahead/bw 上升而显著增长，则优先研究 no-pollution（overlay/rollback）对照与限流策略，而不是扩大预取范围。

## Enable/Disable
新增一组 BTB-entry prefetch 参数（默认 False/0），例如：
- `enable_btb_entry_prefetch`
- `btb_prefetch_lookahead_entries`
- `btb_prefetch_insert_per_cycle`
- `btb_prefetch_types_mask`（或等价过滤参数）
- `btb_prefetch_allow_wrongpath_pollution`（默认 True）

## Dependencies
该变更依赖 `add-fdip-icache-prefetch` 提供的：
- FTQ peek API（沿预测路径 lookahead）
- （可选）prefetched-byte query API（用于预译码）

并默认建立在 post-`update-bpu-control-pc-tail-halfword` 的前端语义之上：
- fetch coverage window 以实际 `predEndPC` / 完整 control 指令覆盖为准
- BTB key/index/tag/position 使用 `controlPC`
- `startPC` 仅作为 predecode bytes window 与日志归因的显式字段保留

## Trace note: instruction bytes availability (strict)
主评测路径为 trace 且可提供 instruction bytes。当前已存在“trace 指令定义 -> RISC-V 指令编码”的实现：
`src/cpu/o3/trace/TraceFetch.cc` 中 `TraceFetch::createMachInstFromTrace()` 会基于 `TraceInstruction`
生成 2B/4B 的 `MachInst`，可作为 predecode 的 bytes 来源。

注意：在 `traceTrainBranches=false` 时，branch 指令会被退化为 NOP（用于避免训练/副作用），此时 direct-CFI
预译码将无法工作；评测配置需显式打开分支指令生成/训练选项，并在文档中记录该前提。

严格说明（避免误解）：
- `createMachInstFromTrace()` 是 per-instruction 编码能力，并不自动提供“fetch window 连续 bytes”。
- BTB-entry predecode 需要的是 **按 FTQ entry 对齐的 fetch-window bytes**。在 trace 主路径下，这需要一个
  “window assembler”把 trace 指令序列的 2B/4B `MachInst` 按确定性规则拼接为固定大小窗口，并遵循
  **complete window or miss** 语义。
- 对于有损/退化编码（例如 RV64 下 compressed call 退化为 `c.nop`、PC-relative imm out-of-range 的 clamp/fallback），
  window assembler 应视为 bytes-miss（或至少显式计数并在研究中作为威胁有效性报告），避免把“编码工件”误归因于微结构效果。

因此，本 proposal 将 bytes 获取抽象为“只读 bytes provider”（complete window or miss），其可由：
- FDIP 的 byte buffer 提供（prefetch/FS 场景），或
- trace-bytes provider 提供（trace 场景）。

若 bytes provider 不可用，本变更按 spec 安全降级（不进行 predecode/prefill），并要求统计明确反映该状态。
