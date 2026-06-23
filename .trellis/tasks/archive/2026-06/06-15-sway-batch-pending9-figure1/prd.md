# SWAY: batch 13 workloads x 3 W -> Figure 1 stranded-ratio data

## TL;DR

把 `06-15-sway-mytools-cpt-backend` 做完后能用的 scheduler 跑一遍 PLAN
里写定的 13 workloads × 3 个 W 值（10K/50K/200K instr），收齐每个跑的
`sway_stranded_by_phase.csv`，做 Figure 1 motivation 数据。结果直接决定
SWAY paper 是按原计划 push（thesis 有效）还是 emergency pivot 到
characterization fallback（thesis collapse）。

**这是 SWAY paper 的生死线。** 在这批数据出来之前，写任何 §III / §IV
prose 都是赌博（`docs/NEXT_ACTIONS.md`）。

## Why now（背景）

- 论文目标 venue: IEEE Computer Architecture Letters (CAL) primary，4
  页短文。论文身体里有 4 个 contribution，C1（per-component utility
  curve characterization）需要这批数据；C2/C3/C4 是 SWAY mechanism +
  evaluation，依赖 C1 motivation 站得住。
- CHECKPOINT 1 截止日：Week 5-6，即 **2026-07-15 至 2026-07-22**。今天
  2026-06-15，还有 4-5 周窗口。
- 决策矩阵（见 PENDING.md PENDING-9 + PLAN.md §183）：
  - 跨 workload stranded ratio 变化 **≥ 3×** → 继续 Phase 1（SWAY
    mechanism implementation）
  - **< 3×** → emergency pivot 到 R3-C3 (a) characterization paper

## Workload set（PLAN.md §V，最终 LOCKED）

13 个 workloads，cpt 路径前缀
`/nfs/home/share/zyy/spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/checkpoint-0-0-0/`：

| Category | Workloads |
|----------|-----------|
| SPEC CPU 2017 frontend-bound | gcc, perlbench, xalancbmk, omnetpp |
| Championship traces | IPC-1 server subset, CVP-1 indirect, CBP-2025 indirect |
| Spotlight | DaCapo javac (γ path) |

具体 checkpoint id 待下一会话拉路径确认。已知 blender_26411 在
`<prefix>/blender/26411/_26411_0.174363_.zstd`，其他 workload 按同套
`/<wname>/<id>/_<id>_<weight>_.zstd` 模式查；如果某 workload 没出现在
share 目录里，需要 user 现场给路径。

**注意**：championship traces 不是 cpt，是 trace 格式（cbp2025 /
champsim）。这两类不能走同一个 backend：
- cpt workloads（SPEC int + DaCapo javac）走新建的 `run_cpt_gem5.sh`
- trace workloads（championship）走已有的 `run_trace_gem5.sh`

或者：第一轮只跑 SPEC int + DaCapo（5 个），先看趋势再决定要不要扩。

## W sweep (PENDING-11)

- 主用 W = **100K instr**（PLAN.md 默认）
- Sensitivity sweep: W ∈ {10K, 50K, 200K}（PENDING-11 解锁条件）
- 通过 `XS_PHASE_SIZE_BY_INST` 环境变量注入到
  `-P system.cpu[0].branchPred.phaseSizeByInst=N`

## MAX_INSTS

建议 5M instr/run，确保：
- W=100K → 50 phase
- W=10K → 500 phase（够做 CDF）
- W=200K → 25 phase（最少也要这么多）

5 个 cpt workloads × 4 W 值 = 20 runs。若 W=50K 不跑（默认值与 100K
差异不大），就是 5 × 3 = 15 runs。

## Pre-requisites（确认 06-15-sway-mytools-cpt-backend 完成）

- [ ] `~/project/GEM5-sway/build/RISCV/gem5.opt` 已 build（含 SWAY 探针 +
      Param 化）
- [ ] `~/project/mytools/.../scripts/run_cpt_gem5.sh` 存在且自检过
- [ ] cpt workload list 文件在
      `~/project/papers/sway-paper/runs/configs/cpt_workloads.lst`
- [ ] server list（可达节点）准备好（参考
      `~/project/mytools/configs/servers.kmhv3_base.frontend_archdb.ok.txt`
      做格式参照）

## Concrete run plan

### Step 1 — W=100K main run（必须）

```bash
mkdir -p ~/project/papers/sway-paper/runs/W100K
XS_PHASE_SIZE_BY_INST=100000 XS_MAX_INSTS=5000000 \
python ~/project/mytools/tools/distributed-trace-scheduler/distributed_trace_scheduler.py \
  --backend shell \
  --arch-script ~/project/mytools/tools/distributed-trace-scheduler/scripts/run_cpt_gem5.sh \
  --backend-config ~/project/mytools/tools/distributed-trace-scheduler/configs/backend.gem5_cpt.sample.json \
  --work-root ~/project/papers/sway-paper/runs/W100K \
  --workload-list ~/project/papers/sway-paper/runs/configs/cpt_workloads.lst \
  --trace-root /nfs/home/share/zyy/spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/checkpoint-0-0-0 \
  --server-list ~/project/papers/sway-paper/runs/configs/servers.lst \
  --tag main_W100K \
  --max-global 32 \
  --max-threads-per-host 8 \
  --poll-seconds 30
```

### Step 2 — W=10K + W=200K sweep（PENDING-11）

同上结构换两个 work-root + tag，分别 export
`XS_PHASE_SIZE_BY_INST=10000` 和 `=200000`。

### Step 3 — 聚合 + Figure 1 plot

每个 run 出一个 `<work_root>/<tag>/<workload>/m5out/sway_stranded_by_phase.csv`。
合并所有：

```python
import pandas as pd, glob, os
rows = []
for d in glob.glob('runs/W100K/main_W100K/*/'):
    wname = os.path.basename(d.rstrip('/'))
    csv = os.path.join(d, 'm5out/sway_stranded_by_phase.csv')
    if not os.path.exists(csv): continue
    df = pd.read_csv(csv)
    df['workload'] = wname
    df['W'] = 100000
    df['stranded_ratio'] = (df['valid_ways'] - df['active_ways']) / df['total_ways']
    rows.append(df)
all_df = pd.concat(rows)
# Figure 1: per-workload bar chart, per-component stacked
```

放在 `~/project/papers/sway-paper/figures/figure1_stranded.py`，生成
`figure1_stranded.pdf`。

### Step 4 — CHECKPOINT 1 决策（任务 #5 的工作）

跨 workload 计算 stranded ratio 的 max/min 比值：

```python
g = all_df.groupby(['workload', 'scope'])['stranded_ratio'].mean().reset_index()
per_workload_total = g.groupby('workload')['stranded_ratio'].mean()
ratio = per_workload_total.max() / per_workload_total.min()
print(f'Cross-workload stranded variation = {ratio:.2f}x')
```

- ratio ≥ 3× → 进入 PLAN.md Phase 1，开 mechanism task。
- ratio < 3× → 立刻通知 user 启动 pivot：
  - 改 PLAN.md §I/§III prose
  - thesis 改成 "we characterize per-workload utility curves and show
    that single-budget partitioning is suboptimal"（不带 mechanism）
  - 重新评估 paper venue（characterization paper 可能更适合
    workshop / 计研发）

## Done definition

- [ ] cpt workload 全跑完（无 abort，每个 m5out 都有 stats.txt +
      sway_stranded_by_phase.csv）
- [ ] W=100K 主跑 + 至少 W=10K 一档 sweep 跑完
- [ ] Figure 1 PDF 生成
- [ ] Cross-workload variation 算出来，写进
      `~/project/papers/sway-paper/PLAN.md` PENDING-9/10 那两行
- [ ] CHECKPOINT 1 决策写进 `docs/NEXT_ACTIONS.md`（push or pivot）
- [ ] Trellis task.json `status: completed`
