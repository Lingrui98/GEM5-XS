# 昆明湖 BPU 模块 gem5 文档


## 文档说明及背景知识

在昆明湖架构的前端中，分支预测单元（BPU）的 gem5 模型最为详尽，最接近 RTL 的实现。本文档旨在帮助读者理解 BPU 的 gem5 模型，允许读者进一步得对其进行修改。

我们建议读者在阅读后续内容前先了解前端 BPU 的架构，关注以下内容即可：

- **Reinman G, Austin T, Calder B. A scalable front-end architecture for fast instruction delivery[J]. ACM SIGARCH Computer Architecture News, 1999, 27(2): 234-245.**
  
    本篇论文介绍了解耦式前端的基本工作原理，与昆明湖架构极为相关，阅读第 1,3 section 来熟悉 FTB，FTQ 这些组成部分的职责和他们的交互方式。

- **xiangshan-design-doc/docs/frontend/BPU/index.md**
  
    昆明湖的前端架构延续了上述论文的思路，但也存在不同之处。阅读此文档，有助于读者构建对昆明湖前端架构的全局了解。我们的后续讨论也将基于昆明湖架构，这要求读者适应它的设计文档的术语，以及这些术语在昆明湖架构下的具体意义（很可能和其他论文存在出入）。

    虽然xiangshan-design-doc为每一个BPU的子模块都提供了详细的介绍，只希望从全局的视角理解BPU的读者只阅读index.md就足够了，读者可以将子模块视为黑箱，知道各子模块的职责和接口即可。




## BPU 源代码的文件结构

熟悉了昆明湖的 BPU 架构后，我们就可以来尝试理解它在 gem5 模拟器里的实现。

首先，让我们看一下 BPU 的文件结构：

```
src/cpu/pred/
├── btb/
│   ├── btb.cc
│   ├── btb.hh
│   ├── btb_ittage.cc
│   ├── btb_ittage.hh
│   ├── btb_sc.cc
│   ├── btb_sc.hh
│   ├── btb_tage.cc
│   ├── btb_tage.hh
│   ├── btb_ubtb.cc
│   ├── btb_ubtb.hh
│   ├── decoupled_bpred.cc
│   ├── decoupled_bpred.hh
│   ├── fetch_target_queue.cc
│   ├── fetch_target_queue.hh
│   ├── folded_hist.cc
│   ├── folded_hist.hh
│   ├── history_manager.hh
│   ├── jump_ahead_predictor.hh
│   ├── loop_buffer.hh
│   ├── loop_predictor.hh
│   ├── ras.cc
│   ├── ras.hh
│   ├── stream_struct.hh
│   ├── test/
│   ├── timed_base_pred.cc
│   ├── timed_base_pred.hh
│   ├── uras.cc
│   └── uras.hh
├── ftb/
├── stream/
├── BranchPredictor.py
├── README.md
├── SConscript
...
```
BPU 的全部源代码都放在 src/cpu/pred/中，读者会发现这个文件夹里还有很多其它文件被省略掉了，（比如 src/cpu/pred/ras.hh），这是因为香山的gem5模型是基于官方gem5开发的。而官方gem5的代码库里本来就提供一些常见微架构的模拟代码（gem5 stdlib？）。我们在香山的 gem5 代码库中，保留了这些文件，但是实际使用的则是以上列举的文件。可以看到真正使用的文件大部分都在 src/cpu/pred/ftb/中。

其中 decoupled_bpred.cc 是 BPU 的顶层文件，它提供接口供其他组件使用（比如前端的顶层文件（fetch.cc？）），同时它也包含了 BPU 的运行逻辑，并在内部调用各个子模块的接口来完成 BPU 的功能。

每一个子模块都在自己的文件中，例如：

1. btb.cc：BTB 的实现
2. btb_tage.cc：TAGE 的实现  
3. ras.cc：RAS 的实现

最后，我们在 stream_struct.hh 中定义了一些公共的结构体，比如 Fetch Block，FullPrediction 等。

## BPU 顶层文件：decoupled_bpred.cc/hh

下面将介绍 BPU 运行过程中较为关键的几个流程，或者涉及 BPU 的流程，希望通过这些流程，读者能更直观的理解 BPU 的接口的意义和用法。

更多关于 BPU 和 Fetch 交互的细节，请参见[BPU 与 Fetch 交互](frontend_guide.md#interaction-with-fetch)。

### Fetch::tick() 访问 FTQ

Fetch::tick() 是整个前端的顶层函数，而其中的 fetch() 函数负责模拟 IFU 的运行，目前我们没有模拟 IFU 的微架构，而是在一个 tick 内，根据 FTQ 项，获取整条 Fetch Block 的指令码，并人工地增加延迟。以下是 Fetch::fetch() 中访问 FTQ 的流程：

- 通过主循环遍历当前 Fetch Block 中的指令。
- 在每条指令的处理过程中，调用 BPU::decoupledpredict() 接口，访问 FTQ，获取 next_pc。
- BPU::decoupledpredict() 可以被视作 FTQ 的一个 access helper function，了解微架构的读者可能会发现模型和 RTL 在这里的差异：前者需要在 fetch 每一条指令时都访问该指令所在的 FTQ 项，而后者则是一次性的收到一个 FTQ 项作为流水线的输入。

下面提供一段 inline 过的伪代码，只对 BPU 感兴趣的读者可以重点关注 BPU 和 FTQ 的接口的调用：
```
// IFU的顶层函数
Fetch::tick()
|  // helper function
|--Fetch::fetch()
|  |  // 主循环，每个迭代对应一条指令的fetch
|  |--while( numInst < fetchWidth && ! predictBranch)
|  |  |
|  |  |  // helper function
|  |  |--predictBranch |= Fetch::lookupAndUpdateNextPC( next_pc)
|  |  |  |  // 调用BPU的接口，获取next_pc和跳转与否
|  |  |  |--BPU::decoupledpredict(&pc)
|  |  |     |  // 调用FTQ的接口，获取当前正在读取的FTQ项（对应RTL的ifuPtr）
|  |  |     |--FTQ::getTarget()
|  |  |     |--set(pc, target)
|  |  |     |--if(run_out_of_this_entry)
|  |  |     |  FTQ::processFetchTargetCompletion()
|  |  |--set(this_pc, *next_pc)
|--BPU::trySupplyFetchWithTarget()
```

>相关内容链接：
> - [BPU 与 fetch 的接口](frontend_guide.md#detailed-explaination-of-interface-with-fetch)


### FTQ 转发 redirect 信号以及对 BPU 子预测器的更新

我们的微结构中，FTQ 会将来自后端或 IFU 的 redirect 信号转发给 BPU，同样的，当 FTQ 收到来自 commit stage 的信号，得知 fetch block 被全部提交后，也会用它存储的 meta 信息更新 BPU 的子预测器。我们的 gem5 模型中也做了等效的处理，只不过我们把这两种逻辑放在 BPU::controlSquash() 和 BPU::update() 中。而它们的调用是发生在 Fetch::checkSignalAndUpdate() 中，下面是伪代码：


```
Fetch::tick()
|
|--Fetch::checkSignalAndUpdate()
   |
   |--if(fromCommit->commitInfo[tid].squash)
   |  BPU::controlSquash()
   |--BPU::update()

```


### 分支预测流水线

BPU::tick() 是 BPU 的顶层函数，它负责模拟分支预测流水线，也就是从 S0 pc 生成三个 stage 的预测结果，再将最准确的结果推到 FTQ 中的过程。BPU 在模型上的原理和微架构类似：

```
[UBTB] ->   [BTB/TAGE/ITTAGE]
   ↓         ↓           ↓
   └──→ [FetchStream Queue] 
              ↓
     [FetchTarget Queue]
              ↓
     [Instruction Cache]
```

每个预测器都会每拍生成预测结果，其中 BTB/uBTB 生成最核心的 BTBEntry, 
然后其他预测器按需填入对应的方向或者别的信息，
共同生成每一级的 FullBTBPrediction(predsOfEachStage), 最后 3 选 1 得到最终的 FullBTBPrediction（finalPred）；

下一拍会根据 finalPred 结果生成一项 FSQEntry 放入 FSQ 中

再下一拍会用 FSQEntry 生成一个 FTQEntry 放入 FTQ 中


这里需要注意模型对微架构的一个简化：

在微架构中，对于某一个 s0 pc，它的 S1 到 S3 的预测结果是在三拍中相继生成的，而我们的模型在获得 s0 pc 后的第一拍就生成了 S1 到 S3 的预测结果。如果我们假设没有 override*发生，
那么这个简化与微架构是等效的。但是细心的读者会发现：在有 override 的情况下，微架构会在产生 override 的那拍重定向 s0 pc，并覆写已经写入的 FTQ 项。也就是说最终的预测结果在发生
override 的那拍才会写入 FTQ。这样一来我们的模型就与微架构不一致了。为了弥补这个差异，我们为有 override 的 s0 pc 手动加入相应的延迟。具体做法是通过变量 numOverrideBubbles。





*：override：一旦高级预测器在后续流水级的预测结果与已有结果不一致，就将会使用高级预测器结果作为新的输出更新后续 FTQ 中存储预测块结果 并重定向 s0 级 PC，清空新结果流水级之前的流水级的错误路径结果。
```
BPU::tick()
|
|--if(!receivedPred && numOverrideBubbles == 0)
|  // 生成最终预测结果，并创建override bubbles
|  BPU::generateFinalPredAndCreateBubbles()
|  |
|  |--finalPred = predsOfEachStage[numStages - 1]
|  |--numOverrideBubbles = firstHitStage
|  |--receivedPred = true
|
|--processEnqueueAndBubbles()
|  |
|  |--tryEnqFetchTarget();
|  |  |
|  |  |--if (!validateFTQEnqueue())
|  |  |  return;
|  |  |--ftq_entry = createFtqEntryFromStream(ftq_enq_state.streamId)
|  |  |--fetchTargetQueue.enqueue(ftq_entry);
|  |
|  |--tryEnqFetchStream();
|  |  |
|  |  |--if (!validateFSQEnqueue())
|  |  |  return;
|  |  |--processNewPrediction(true);
|  |  |  |
|  |  |  |--entry = createFetchStreamEntry();
|  |  |  |--s0PC = finalPred.getTarget(predictWidth);
|  |  |  |--updateHistoryForPrediction(entry);
|  |  |  |--fetchStreamQueue.emplace(fsqId, entry);
|  |  |  |--fsqId++;
|  |  |
|  |  |--receivedPred = false;
|  |  
|  |--if (numOverrideBubbles > 0)
|     numOverrideBubbles--;
|
|--requestNewPrediction()
   |
   |--if (!receivedPred)
      |--for (int i = 0; i < numComponents; i++)
         components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);
```


### 正在修订的内容

###### 模型运行过程中，一个 fetch block 的生命周期

###### 其他模型的技巧：FSQ+FTQ = RTL 的 FTQ

###### 数据结构的关系（BTBEntry vs FullBTBPrediction）

###### 关键函数解释（参见 cpu/pred/README.md）

###### 特殊功能

###### 变量解释

###### 子预测器的抽象


## Decoupled BTB 的 control-PC 语义演进

这一节补充近期 BTB-only 路径中最容易看混的几个语义点。核心结论是：

- predictor-visible 的 branch identity 不再直接使用“指令起始地址”，而是使用 **control PC view**
- 对于跨 block 的 32-bit 控制流指令，这个 control PC view 采用 **tail-halfword** 作为身份
- 但统计、debug、RAS/uRAS 回退地址、fetch byte coverage 仍然需要保留 **architectural startPC**

也就是说，当前模型里至少同时存在三种“和同一条控制流指令有关”的 PC：

1. `startPC`
   - 指令的真实起始地址，也是 architectural 语义最稳定的表示。
   - 用于 byte coverage、fall-through 计算、RAS/uRAS 的 start-PC 相关逻辑，以及很多 debug/统计信息。
2. `controlPC`
   - predictor-visible 的控制流身份。
   - 对普通情况，它和 `startPC` 相同；对 split 32-bit control instruction，它可能落在后一半 halfword 上。
3. `predEndPC`
   - 当前 fetch target 允许 fetch 消费的上界（exclusive）。
   - fetch 是否“跑出这个 target 的覆盖区间”，看的是这个值，而不是单纯看一条 branch 的起始位置。

这套拆分的动机是：让 predictor key 与 RTL 的控制流身份保持一致，同时又不丢失 fetch/decoder/RAS 仍然必须依赖的起始地址语义。

### tail-halfword 视角为什么需要单独引入

对于一条跨 fetch block 边界的 32-bit 控制流指令：

- decoder 和 fetch byte coverage 的视角，天然更关心 `startPC`
- 但 BTB 的 control-flow key 更接近“控制流真正生效的那个 halfword 位置”

如果继续把同一个字段同时拿来做：

- predictor lookup key
- fetch coverage trigger
- 回退/统计的 architectural address

那么代码中会不断出现“到底是 startPC 还是 controlPC”的临时分支，既容易出错，也很难和 RTL 对齐。

因此这一轮修改把这两个语义正式拆开：

- `BranchInfo::startPC()` 继续表示 architectural start
- `BranchInfo::controlPC()` 表示 predictor-visible control identity

fetch 侧随后再通过 coverage helper 判断“当前 PC 是否仍属于这个 target 的覆盖区间”，而不再把“是否落在 controlPC 上”误当成整个 target 的消费边界。

## split-control owner migration 与 fetch handoff

在 control-PC 切换到 tail-halfword 以后，会出现一个新的情况：

- 一条 taken control instruction 的 `controlPC` 已经属于“后一个 fetch target”的视角
- 但它的起始字节仍然可能位于“前一个 fetch target”抓到的 fetch buffer 中

这时如果 fetch 仍然机械地按“当前 FTQ head 拥有当前所有 inst-start PC”的假设运行，就会出现两个问题：

1. buildInst 仍在旧 target 上构造这条 split control instruction
2. taken matching / redirect matching 仍在旧 target 上判断，导致 owner 语义和 predictor key 脱节

因此引入了 **owner migration**：

- 当前正在消费的 target 仍然按顺序提供 fetch bytes
- 但当 following target 明确声明“这条 split control instruction 由我拥有”时，fetch 会在 buildInst 之前把 owner 切到 following target

### 当前 owner 语义如何表达

`FetchTarget` 现在显式区分了“stream 起点”和“owner 起点”：

- `startPC`
  - 这个 fetch target 在 FTQ 中的 nominal 起点
- `ownerStartPC()`
  - 这个 target 实际拥有的最早 inst-start PC
  - 对普通 target，`ownerStartPC() == startPC`
  - 对 split-control handoff，`ownerStartPC() < startPC`

这也是为什么之前的 `decodeStartPC` 被重命名成 `ownerStartPC`：

- 它表达的并不是 decoder 的局部状态
- 它表达的是“这条 fetch target 对哪些 inst-start PC 负责”

### fetch handoff 的触发条件

fetch 侧不再手写一长串 owner-migration 条件，而是通过 `FetchTarget` helper 来统一表达：

- `hasSplitControlOwnership()`
- `ownsInstPC(inst_pc)`
- `shouldTakeSplitControlOwnershipFrom(previous, inst_pc)`
- `isTakenControlAt(inst_pc)`

这样做的好处是：

- owner range 的定义只保留一份
- fetch 与单测不再各自维护一套“手写布尔表达式”
- 以后如果 owner 语义继续演进，只需要改 `FetchTarget` 的契约

从使用层面看，fetch 的判断可以概括成两步：

1. 如果 following target 对当前 `inst_pc` 满足 `shouldTakeSplitControlOwnershipFrom(...)`，先完成 handoff
2. handoff 完成后，再在新的 owner target 上判断：
   - 当前 PC 是否仍在 owner range 内
   - 当前 PC 是否正好命中 taken control instruction

## trace / FS 路径的边界

owner migration 本质上是一个“fetch 消费 FTQ target 时的运行时协议”。这条协议并不是所有前端模式都需要照搬。

### FS / 正常 decoupled fetch

在正常 decoupled BTB fetch 路径中：

- fetch 需要逐条指令地消费当前 target
- 同时还要保持与 predictor-visible controlPC 身份一致

因此 owner migration 必须真实发生，否则 split-control 的 taken matching、redirect 和 update 语义都会漂移。

### trace mode

trace mode 的目标不同：

- 它更像“用 trace 驱动一条顺序消费路径”
- 而不是“完整重放 FTQ owner handoff 协议”

因此当前实现里，fetch-time owner migration 在 trace mode 下会直接跳过；如果 trace 消费时发现当前 PC 不落在 owner range 内，就退回顺序推进，而不是继续尝试 FTQ owner handoff。

这个约束的意义是：

- 避免把正常 FTQ handoff 协议生搬到 trace mode，导致回滚/恢复逻辑更复杂
- 把 trace mode 的行为收敛为“能顺序消费就顺序消费，不能映射 owner 时不强行模拟 handoff”

### 相关回归点

这一轮语义调整之后，至少有三类路径需要一起看：

1. normal FS / decoupled BTB
   - 检查 split-control handoff、taken matching、redirect 是否一致
2. trace mode
   - 检查 owner migration 被跳过后，是否还能顺序推进并正确回滚
3. start-PC consumers
   - 例如 RAS/uRAS、trace wrong-path NOP sizing、coverage helper 等，是否仍然使用 architectural startPC

## 最终简化：为什么要把 owner 规则收回到 FetchTarget

在 owner migration 最初落地时，fetch 里存在较强的“协议泄漏”：

- fetch 自己知道 owner handoff 的所有条件
- 测试代码也手写了一份几乎等价的判定

这样的问题在于：

- reader 很难分辨“这是 fetch 的状态机”还是“这是 FetchTarget 的领域规则”
- 一旦语义有改动，很容易只改到 fetch 没改到测试，或反过来

最终简化版做的事情并不只是改名，而是把契约收回到 `FetchTarget`：

- `decodeStartPC -> ownerStartPC`
- handoff / owner range / taken match 都通过 helper 表达
- `decoupled_bpred`、`fetch`、`btb.test` 共用同一套 owner 语义

因此更准确的理解是：

- `FetchTarget` 描述“这个 target 拥有哪段 inst-start PC，以及哪一个位置是 taken control”
- `fetch` 只负责按这个契约消费 target，而不再自己重写一套协议

这也是后续若继续简化 FTQ / FSQ 边界时，最重要的准备工作之一：先把 owner 规则变成稳定的数据契约，再考虑把 handoff 进一步下沉到 queue/predictor 侧。

