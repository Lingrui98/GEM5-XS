# PC 数据流说明

## 背景
- 现仓库只面向 RISC-V，但仍沿用 gem5 通用 `PCStateBase` 抽象，导致 fetch、decoder、分支预测器、commit 等阶段都要面对多态 `PCState`，既重复更新 `_pc/_npc`，也让压缩指令（16bit）逻辑分散。
- 多数“PC 前进”操作仍默认 4B（见 `GenericISA::UPCState<4>::advance()`），而 RISC-V 的真实步长由 decoder 根据 `compressed()` 判断并写入 `PCState::npc()`，容易被后续 `advancePC()` 覆盖。
- 该文档帮助快速定位 PC 的生命周期与关键写入点，便于后续重构或调试。

## 数据流全景
1. **取指前检查**（`src/cpu/o3/fetch.cc:1889` `checkMemoryNeeds`）
   - 从 `fetchBuffer` 拷出 4B 到 decoder 的 `machInst` 缓冲，`PCStateBase` 仅参与传递当前指令地址。
2. **解码阶段**（`src/cpu/o3/fetch.cc:1975` -> `src/arch/riscv/decoder.cc:56`）
   - `Decoder::decode(pc)` 在确定指令长度后直接写 `pc.as<PCState>().npc(...)` 并设置 `compressed` 标志，这是唯一“正确”计算 `_npc` 的地方。
3. **分支预测**（`src/cpu/o3/fetch.cc:723`）
   - `lookupAndUpdateNextPC()` 在“非分支或预测不跳”时调用 `inst->staticInst->advancePC(next_pc)`，而 `RiscvStaticInst::advancePC()` 又调用 `PCState::advance()`（间接 +4）。若 decoder 刚写了 `npc = pc + 2`，此处会被覆盖。
4. **DynInst 建立 / ROB 推进**（`src/cpu/o3/fetch.cc:1998`、`src/cpu/o3/commit.cc:1373`）
   - `buildInst()` 把 `pc`/`next_pc` 封装进动态指令；commit 阶段再一次 `advancePC(*pc[tid])`，默认 4B。
5. **异常/故障路径**（例如 `src/sim/faults.cc:76`, `src/arch/riscv/faults.cc:255`）
   - Fault 处理同样使用 `advancePC()`，若没有在 fault 触发前 decode 完整 inst size，也会 fallback 到 +4。

下图（文字描述）可参考：
```
Fetch buffer bytes ──► Decoder::decode() ──► next_pc.npc = pc + {2|4}
           │                               │
           └─► DynInst.buildInst() ◄───────┘
                                      │
                       lookupAndUpdateNextPC() → advancePC()(+4)
                                      │
                               Commit / Fault
```

## PCState 语义模型（RISC-V）

### 字段含义（RISC-V PCState = Generic UPCState<4> + compressed/rv32）
- `pc`：当前（宏）指令地址；`instAddr()` 返回它（`src/arch/generic/pcstate.hh:106`）。
- `npc`：当前（宏）指令“解析/执行后”的下一条（宏）指令地址。它在解码阶段通常先被写成 fall-through，在控制流指令执行时可能被覆盖成跳转目标。
- `upc/nupc`：macroop 被拆成 microop 时，用于标识“当前在第几条 microop / 下一条 microop”（`src/arch/generic/pcstate.hh:430`）。

RISC-V 侧的 `PCState` 额外携带：
- `compressed`：用于计算 fall-through（2B/4B）与 `branching()`（`src/arch/riscv/pcstate.hh:85`）。
- 注意：PCState 的 `equals()` 比较不包含 `compressed/rv32`（因为未 override equals），只比较 `pc/upc/npc/nupc`。

### 两种形态：pre-advance vs post-advance（理解 pc/npc“到处变”的关键）
PCState 在流水线中经常以两种“表示形态”出现：

1. pre-advance（“描述当前指令”）
pc=当前指令地址，npc=该指令计算出来的 next（先是顺序 fall-through，执行时可能改成目标）

这一阶段 npc 的来源有两类：
* 解码阶段先把 fall-through 写进去（RVC 用 2B、非 RVC 用 4B）：decoder.cc (line 92)
* 控制流指令执行语义在条件满足时覆盖 npc 为目标，例如 beq：decoder.isa (line 1983)

2. post-advance(“下一条要取的指令表示”）：把 A 通过 advancePC() 推进成“下一条的位置”

这一步由 StaticInst::advancePC() 统一做：
* 普通（非 microop）指令：RiscvStaticInst::advancePC 直接调用 PCState::advance()（static_inst.hh (line 62)），而 advance() 的核心是 pc = npc; npc += 4（pcstate.hh (line 380)）。
* microop：用 uAdvance/uEnd 在 microop 内推进或结束 macroop（static_inst.cc (line 42)）。

关键点：真正决定“下一条从哪取”的是 pc = npc 这一下；后面那个 npc += 4 在你现在这套前端里更多是“占位/惯例”，下一次 decode 会重写 npc（例如你们 fetch 里顺序路径明确写了 placeholder：fetch.cc (line 815)）。

### 为什么你看到 NPC 在执行前后变化（以 beq/bne 为例）
* 执行前：decoder 先把顺序 fall-through 写到 npc（RVC/非 RVC 分别是 pc+2/pc+4），见 decoder.cc (line 92)
* 执行时：分支语义判断 taken 就覆盖 NPC = PC + imm，not-taken 就保持原值（你看到的 NPC = NPC 本质是 no-op），见 decoder.isa (line 1983)
* 执行后：此时 inst->pcState().npc() 就是“该指令的真实 next PC”（taken=target，not-taken=fall-through）
这其实是 gem5 ISA 语义里常见的写法：先给 NPC 一个默认 fall-through，然后控制流指令在需要时改写 NPC。

### 你觉得“mispredicted() 很恶心”的原因：它在比较两种“表示”
mispredicted() 不是直接拿 inst->pcState().npc() 和预测比，而是：

* clone 当前指令的 PCState（A：pre-advance）
* 调 advancePC() 把它推进成（B：post-advance）
* 用这个 B 去和 predPC 比（dyn_inst.hh (line 701)）

而 predPC 是 fetch 阶段保存的“预测后的下一条要取的位置”（post-advance 形态），在你们代码里由 Fetch::lookupAndUpdateNextPC() 写入（fetch.cc (line 783)，inst->setPredTarg(next_pc) 在 fetch.cc (line 838) / fetch.cc (line 857)）。

所以这里“看起来绕”，但它本质上是在把“实际 next（藏在 npc 里）”先做一次 advance，转换成和 predPC 同一种表示再比较。


## 模块与接口
| 模块 | 核心函数 | 触碰 PC 的原因 | 备注 |
| --- | --- | --- | --- |
| Fetch | `processSingleInstruction()` | clone 当前 `pc`、交给 decoder 填写 `next_pc` | `std::unique_ptr<PCStateBase>` 使复制成本高 |
| Decoder | `Decoder::decode(PCStateBase &)` | 根据指令宽度写 `npc`、更新压缩标志 | 唯一 knows inst size 的模块 |
| 分支预测 | `lookupAndUpdateNextPC()` | 预测 taken 时写目标；not-taken 时 `advancePC()` | Decoupled frontend 模式还会 invalidate fetch buffer |
| StaticInst | `RiscvStaticInst::advancePC()` | 调用 `PCState::advance()`（固定 +4） | 微指令还会更新 micro PC |
| Commit | `Commit::commitHead()` 等 | 退休时 `advancePC()` 以推进 architectural PC | 假定 decode 已设置 `npc` |

## 常见陷阱
- **多处写 NPC**：decoder 与 branch predictor 都写 `next_pc`，很难看出最终谁生效。调试时建议对 `PCState::advance()`/`Decoder::decode()` 加 `DPRINTF` 或 `panic_on_overwrite`。
- **宏/微指令混用**：`curMacroop` 情况下不会重进 decoder，micro-op 的 `advancePC()` 只更新 `microPC`，实际 `npc` 仍保留上次宏指令的值，需要确认 `_compressed` 是否保持正确。
- **Fault/断点路径缺少 inst size**：异常触发时可能尚未 decode 完整指令，`advancePC()` 就会默认 +4，导致恢复后 PC 偏移。需要在 fault 前确保 `pc.compressed()` 已设定，或者在 fault handler 中读取 `StaticInst::instSize()`。

## 建议的梳理步骤
1. **自动清单**：运行 `rg "PCState" -n src`、`rg "advancePC" -n src`，并将结果分类成“读取/写入 PC”两份列表，附加用途说明。
2. **标注关键信息**：在 `src/arch/riscv/pcstate.hh` 顶部新增注释，明示“只有 decoder 负责写 npc，其他模块请使用 instSize 信息”，避免误改。
3. **封装 helper**：先实现一个 `inline void advanceByInstSize(PCState &, unsigned size)`，fetch/branch predictor/commit 全部通过它更新 `npc`，为后续完全去除 `PCStateBase` 打基础。
4. **调试辅助**：短期内可在 `PCState::advance()` 中 `panic_if(!compressedKnown)` 或输出 WARNING，提醒调用者不要直接依赖默认 +4。
5. **文档更新**：当梳理出更具体的模块交互后，把这份文件按章节补充实例（例如具体 DPRINTF 输出、常见 bug 案例），形成团队内部共识。

## 风险
- 梳理过程中若贸然修改 `advancePC()` 行为，会影响 commit、fault、checker 等多个子系统，必须在每次改动后运行至少 `scons build/RISCV/gem5.opt` + 快速仿真回归。
- 文档与代码偏离：如果文档更新不及时，可能让团队依赖过时信息。建议在提交中把相关 PR 编号/日期写入本文件顶部。

## 改进建议
1. 每当新增/修改触碰 PC 的代码路径时，要求在 MR 描述中引用本文件并说明是否影响数据流。
2. 定期（例如每个迭代）由维护者运行脚本重新生成“PC 操作列表”，对比变化，确保无人引入新的 `_npc += 4`。
3. 长期目标：在 RISC-V-only 分支中，将 `PCStateBase` 的引用逐步替换为 `RiscvISA::PCState &`，并让 `PCState::advance()` 依据 `_compressed` 调整步长，从接口层面杜绝 +4 误用。

## 补充：decoupled BTB 中的 startPC / controlPC / ownerStartPC

上面的内容主要讨论 `PCState` 的推进语义；而在 decoupled BTB 路径里，还存在另一组同样容易混淆的“PC 身份”。

在近期 control-PC / owner-migration 改动之后，至少要区分三类地址：

1. `startPC`
   - 指令的 architectural 起始地址。
   - 主要用于 fetch byte coverage、decoder 组装、RAS/uRAS 回退地址，以及很多统计/debug 场景。
2. `controlPC`
   - predictor-visible 的控制流身份。
   - 对跨 block 的 32-bit control instruction，会切换到 tail-halfword 视角。
3. `ownerStartPC`
   - `FetchTarget` 实际拥有的最早 inst-start PC。
   - 对普通 target，`ownerStartPC == startPC`；对 split-control handoff，`ownerStartPC < startPC`。

这三者的关系可以概括成一句话：

- `startPC` 决定“指令从哪里开始”
- `controlPC` 决定“predictor 认为是哪一个控制流身份”
- `ownerStartPC` 决定“当前 fetch target 对哪些 inst-start PC 负责”

### 为什么 controlPC 要切到 tail-halfword

对一条跨 block 的 32-bit 控制流指令而言：

- 从 decoder/fetch buffer 的角度，它仍然从 `startPC` 开始组装
- 但从 predictor key 的角度，真正决定控制流命中的位置可能已经落到后一个 halfword

因此当前模型把 `controlPC` 作为 predictor-visible identity，并在 split 4B control instruction 上采用 tail-halfword 视角。这样做以后，BTB/TAGE/ITTAGE/uBTB 等子预测器可以统一围绕 controlPC 建 key，而不再在每个子模块里重复塞“如果跨界就挪半个指令”的特判。

### owner migration 解决的是什么问题

controlPC 切换以后，会出现这样一种情况：

- 当前 fetch buffer 里确实还拿着这条 split control instruction 的前半段字节
- 但 predictor 视角下，这条控制流已经属于 following target

如果 fetch 还把这条指令当作“当前 target 自己拥有的指令”，就会出现：

- buildInst 发生在错误的 target 上
- taken matching 仍在旧 target 上判断
- 后续 redirect / update 元信息与 predictor key 脱节

因此引入了 owner migration：在真正 buildInst 之前，如果 following target 声明“当前这条 inst-start PC 归我”，fetch 会先把 owner handoff 到 following target，再在新的 owner 上执行 taken matching。

### 当前 fetch 侧如何判断 owner handoff

最终简化版已经不再让 fetch 手写协议，而是通过 `FetchTarget` helper 表达 owner 规则：

- `ownsInstPC(inst_pc)`：当前 inst-start PC 是否属于这个 target
- `shouldTakeSplitControlOwnershipFrom(previous, inst_pc)`：当前是否应从前一个 target 迁移 owner
- `isTakenControlAt(inst_pc)`：当前 inst-start PC 是否正好命中这个 target 的 taken control instruction

这几个 helper 的意义是把“owner 语义”从 fetch 状态机里抽出来，让 fetch 只消费一个已经定义好的数据契约。

## fetch handoff 与 trace / FS 边界

owner migration 是 decoupled BTB fetch 路径的一部分，但并不是所有运行模式都需要模拟这套协议。

### 正常 FS / decoupled fetch

在正常 FS / decoupled BTB 路径中：

- fetch 要逐条消费当前 target 的 inst-start PC
- redirect / taken match 还要和 predictor-visible controlPC 保持一致

因此 owner migration 必须发生，否则 split-control 情况下：

- 当前 PC 可能落在“following target 才拥有的 inst-start range”里
- 但 fetch 仍在旧 target 上继续推进

这会直接导致 taken 匹配点、redirect 归属和 update 元信息错位。

### trace mode

trace mode 的目标是稳定消费 trace，而不是完整重放 FTQ handoff 协议。因此当前实现里：

- fetch-time owner migration 在 trace mode 下直接跳过
- 如果 trace 当前 PC 不落在 owner range 内，就回到顺序推进，而不是强行执行 FTQ owner handoff

这条边界的本质是：

- FS / 正常 decoupled fetch 需要保留真实 owner 语义
- trace mode 则优先保证 trace 消费和回滚路径简单、稳定

### startPC consumers 仍然必须保持 architectural 语义

即使 predictor key 已经切换到 controlPC，下面这些路径仍然必须继续使用 `startPC`：

- RAS/uRAS 的 call fall-through 推导
- trace wrong-path NOP sizing
- fetch coverage / decoder 仍然按字节跨度工作的逻辑

换句话说，controlPC 的引入不是“所有地方都改成 controlPC”，而是把不同模块真正关心的 PC 身份拆清楚。

## 最终简化后的读图方法

如果你在调试 fetch/BPU 的 PC 相关 bug，可以按下面顺序看：

1. 先看 `startPC`
   - 指令字节是不是从正确的位置开始拼出来的
2. 再看 `controlPC`
   - predictor 命中的控制流身份是不是你期望的那条
3. 最后看 `ownerStartPC`
   - 当前 `FetchTarget` 是否真的拥有这条 inst-start PC

当三者不一致时，通常意味着问题落在下面三类之一：

1. control-PC 视角切换错了
2. owner handoff 没有及时发生
3. 某个 start-PC consumer 被误改成了 controlPC 路径

> 本文件适合作为日常排查 PC 相关 bug 的入口，可继续扩展“案例分析”章节（例如具体 trace 片段），帮助新同学快速定位问题。若你发现新的数据流或工具，也请补充到此处。
