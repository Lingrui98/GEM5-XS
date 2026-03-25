# Design: decoupled BPU tail-halfword control-PC view

## Context

当前代码中，控制流 PC 语义混在同一条数据路径里：

- `src/cpu/pred/btb/stream_struct.hh` 的 `BranchInfo::pc` 是单字段，既被当成指令起始地址，也被当成 BPU 内部 branch identity。
- `src/cpu/pred/btb/btb_tage.cc` 的 position/tag 计算直接使用 `entry.pc`。
- `src/cpu/pred/btb/decoupled_bpred.cc` / `src/cpu/o3/fetch.cc` 通过 FTQ `takenPC` / `endPC` 决定 taken redirect。
- `src/arch/riscv/decoder.cc` 当前 `moreBytes()` 一次接收数据后就把指令视为 ready，不支持 2B + 2B 的增量拼装。

这套抽象在 split 4B control case 下会产生两个问题：

1. predictor 想把 4B RVI control 归到“后半条所在块”，但 fetch/decode 又必须从 `startPC` 开始拼装完整指令；
2. 若继续只保留单个 `pc`，实现者只能把边界特判塞进 fetch，导致 control-pc-view 语义越来越难维护。

## Goals / Non-Goals

- Goals:
  - 让 BPU 内部统一以 `controlPC` 识别控制流指令
  - 保持 fetch/decoder 仍围绕 `startPC` 做字节覆盖与指令拼装
  - 让跨块 4B control 的 taken 时机自然落在后半条所在预测块
  - 降低 fetch 对 split control 的硬编码特判
- Non-Goals:
  - 不在本 proposal 中改写所有前端结构的最终代码
  - 不把非控制流 4B RVI 指令也强制映射成新的 BPU 身份语义
  - 不改变旧 validation 基础设施的总体组织方式

## Decisions

### 1. Dual-view contract: `startPC` for bytes, `controlPC` for prediction

- `startPC` 定义为架构意义上的指令起始地址。
- `controlPC` 定义为最后 2B 的起始地址：`startPC + size - 2`。
- 在 BPU 可见的数据结构中：
  - `pc` SHALL 表示 predictor-visible `controlPC`
  - `startPC` SHALL 显式保留，不能继续依赖“从 `pc` 反推”
- `endPCExclusive` 继续由 `startPC + size` 推导，用于字节覆盖与完整指令判断。

选择这个方案的原因是最小化 predictor 侧 churn：多数已有表项/lookup/update 路径都依赖 `pc`，
因此把 `pc` 重新定义为 `controlPC`，再额外补 `startPC`，比保留“`pc=startPC` 再额外传播 `controlPC`”
更接近新需求，也更不容易遗漏内部调用点。

### 2. Predictor ownership follows `controlPC`

- BTB/TAGE 相关的 index、tag、position、slot ownership、lookup/update/match 统一使用 `controlPC`。
- split 4B control instruction 属于 `controlPC` 所在预测块，而不是 `startPC` 所在块。
- 这意味着：
  - 前一个块最多只携带 leading 2B，不得把该指令视为 taken slot
  - 后一个块才拥有该 control entry，对应的 taken redirect 也只在这里发生

对于 half-align / 32B 边界 case，`controlPC` 归属直接决定“哪一个预测块看见这条控制流”；
不再要求 fetch 通过提前 redirect 或补字节来模拟这种归属。

### 3. Fetch coverage remains `startPC`-based

- Fetch/FTQ entry 的 coverage 仍然围绕 `startPC` 与完整字节可见性组织。
- 对 split 4B control：
  - 第一个块拿到前 2B
  - 若没有 taken redirect，fetch 将自然继续请求下一个块
  - 第二个块带回后 2B 后，decoder 才能完成这条从 `startPC` 开始的 4B 指令
- 因此 fetch 侧不应再维持“每次 decode 前必须已有连续 4B”这一假设。

这条决策的核心目的是把复杂度放在“控制流归属”而不是“fetch 特判”上：predictor 决定是否 redirect，
fetch 只负责持续把可见字节送进 decoder。

### 4. Decoder upgrades to halfword-granular fill

- RISC-V decoder SHALL 支持 2B 单次填充。
- 2B 指令在第一次 halfword 填充后即可 ready。
- 4B 指令在第一次 halfword 填充后必须保持 not-ready，并显式请求更多字节；
  第二次 halfword 到达后才完成 decode。
- fetch SHALL 依据 decoder 的 `instReady()/needMoreBytes()` 契约反复喂入 halfword，
  而不是默认 `memcpy(..., 4)` 并一次性完成当前指令。

这样处理后，跨块 4B control 与跨块 4B non-control 的字节拼装路径可以统一，
不需要额外给“控制流跨块”单独开一条 fetch 旁路。

### 5. Pending-change alignment is explicit

- `add-btb-entry-prefetch` 必须改成：
  - 完整字节窗口/拼装仍用 `startPC`
  - BTB prefill key / index / tag / position 改用 `controlPC`
- `add-fdip-icache-prefetch` 必须改成：
  - baseline 引用新的 `update-bpu-control-pc-tail-halfword`
  - FDIP 仍按实际 fetch coverage 工作，但跨边界 4B control 的覆盖归因来自 `controlPC` 所在 entry

## Risks / Trade-offs

- 将 `BranchInfo::pc` 重新定义为 `controlPC` 会影响大量既有 debug / tests；
  缓解方式是同步保留显式 `startPC`，并在 range-sensitive logs 中同时打印两者。
- decoder 改成 halfword-granular fill 后，fetch 的局部状态机会变复杂一些；
  但这类复杂度是通用字节拼装复杂度，不再是“仅 control-pc-view 才有”的特判。
- 两个 pending changes 的依赖语义会变化；因此必须在 proposal 阶段就显式修文档，避免后续实现者继续沿旧假设推进。

## Migration Plan

1. 先合入本 proposal 与相关依赖文档修正，锁定语义。
2. 后续实现时先重构 predictor-visible PC 视图，再改 fetch/decoder 半字节填充。
3. 最后复用旧 change 的 validation matrix 回归，并增补新的 cross-boundary directed witnesses。
