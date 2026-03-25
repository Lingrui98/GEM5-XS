# Change: Switch decoupled BPU control-PC view to tail-halfword identity

## Why

当前 pending change `update-decoupled-btb-control-pc-views` 采用的是“`startPC` 作为 canonical branch identity，
`triggerPC` 只是派生 taken-timing 视图”的方案。这已经无法满足新的 `impl-control-pc-view` 需求：

- BPU 内部凡是把“分支 PC”当成 key/slot/position/index/tag 的地方，都必须永远看到**最后两 byte 的起始地址**；
- 对跨对齐边界的 4B RVI 控制流指令，必须只在**后半条所在预测块**产生 taken 预测，而不是在前半条所在块提前 redirect；
- fetch 侧应尽量少背负“split control instruction”的特判复杂度，真正的跨块拼装由 fetch coverage + decoder 增量喂码自然完成。

当前代码中，`BranchInfo` 的单个 `pc` 同时承担了“指令起始地址”“BPU 控制流身份”“日志显示地址”三种职责。
这种混用在 split 4B control case 下会把复杂度推给 Fetch。新的 proposal 要显式拆分：

- `startPC`：指令起始地址，负责字节覆盖与解码归属；
- `controlPC`：最后 2B 的起始地址，负责 BPU 内部控制流身份。

## What Changes

- 新建一个后续 OpenSpec change：`update-bpu-control-pc-tail-halfword`
- 明确声明它**替代** `update-decoupled-btb-control-pc-views` 中“BPU canonical identity = startPC”的目标语义
- 定义 `controlPC = startPC + size - 2`
  - 2B RVC control：`controlPC == startPC`
  - 4B RVI control：`controlPC == startPC + 2`
- 规定 BPU 内部所有以“branch pc”驱动的逻辑都统一使用 `controlPC`：
  - BTB/UBTB/MBTB/ABTB/TAGE/MGSC 的 index、tag、position、lookup、update、match
  - predictor-side branch slot 归属
  - predictor-side taken redirect eligibility
- 规定 fetch coverage 继续围绕 `startPC` 与完整指令字节展开，而不是把 fetch 的字节语义也改成 `controlPC`
- 规定 split 4B control instruction 只属于 `controlPC` 所在预测块；前一个块可以提供前 2B，但不得据此 taken redirect
- 规定 RISC-V decoder/fetch 合同升级为**2B 粒度的单次指令码填充**：
  - 2B 指令一次 halfword 喂码即可 ready
  - 4B 指令允许先吃前 2B，再等待后 2B 后完成 decode
- 联动修正依赖旧语义的 pending changes：
  - `add-btb-entry-prefetch`
  - `add-fdip-icache-prefetch`

## Impact

- Affected specs: `decoupled-btb-control-pc-semantics`
- Affected code:
  - `src/cpu/pred/btb/stream_struct.hh`
  - `src/cpu/pred/btb/btb_tage.cc`
  - `src/cpu/pred/btb/decoupled_bpred.cc`
  - `src/cpu/o3/fetch.cc`
  - `src/arch/riscv/decoder.cc`
- Affected pending changes:
  - `update-decoupled-btb-control-pc-views` (semantic baseline superseded)
  - `add-btb-entry-prefetch` (BTB key semantics updated to `controlPC`)
  - `add-fdip-icache-prefetch` (baseline wording updated to the new split `startPC`/`controlPC` contract)

## Validation Plan

本 proposal 的验证方案**沿用**旧 change `update-decoupled-btb-control-pc-views` 的整体框架，不重新发明验证矩阵。
后续实现时应直接复用其验证组织方式，包括：

- `openspec validate ... --strict`
- 旧 change 的 CSV manifest / env template 契约
- helper/build smoke
- SPEC checkpoint FS 主验证
- trace smoke / short regression / top set 回归
- runtime witness 日志闭环

新增的 directed validation 只补语义差异，不换验证平台：

1. `Rvc2B_ControlPCView`
   - 验证 2B control 保持 `startPC == controlPC`
2. `Rvi4B_ControlPCView`
   - 验证 4B control 使用 `controlPC = startPC + 2`
3. `Rvi4B_ControlPC_InSingleBlock`
   - 验证单块内 4B control 仍可正常 predict / redirect
4. `Rvi4B_ControlPC_CrossBoundaryPredictInNextBlock`
   - 验证跨块 4B control 不在前半条所在块 taken，而只在后半条所在块 taken
5. `Decoder_Incremental2BFill_Rvi4B`
   - 验证 decoder 支持先接收前 2B、后接收后 2B，再完成同一条 4B 指令 decode

建议后续实现沿用旧 change 的 `validation/tasks.csv` / `validation/env.example.sh` 结构，
并在新 change 中保持相同的任务/产物合同，而不是设计另一套 orchestration 格式。
