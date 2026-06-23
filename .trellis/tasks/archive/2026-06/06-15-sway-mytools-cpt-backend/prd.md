# mytools: cpt-mode gem5 backend for SWAY motivation runs

## TL;DR — what the next session needs to do

写一个新的 `run_cpt_gem5.sh` 后端脚本 + 一个新的
`backend.gem5_cpt.sample.json`，让 `distributed-trace-scheduler` 能批跑
**spec17 GCB-V checkpoint** 模式的 gem5（而不是它现在只支持的 trace
模式）。这是 SWAY paper Figure 1 motivation 数据（PENDING-9）批跑的
唯一前置任务，做完直接进入 `06-15-sway-batch-pending9-figure1`。

## Why now（背景：SWAY paper 进度与瓶颈）

- SWAY paper（`~/project/papers/sway-paper/PLAN.md`）目标 CAL 短文 + HPCA
  扩展，论点是 "BTB/ITTAGE/TAGE conditional metadata 在不同 workload 间
  utility 差异显著，可以做 way 粒度跨组件 reallocation"。
- 整篇论文最致命的实验是 **PENDING-9 Figure 1: per-component stranded
  ratio 跨 workload 差异**。如果跨 workload 没有显著差异（< 3×），thesis
  立刻 collapse，要 pivot 成纯 characterization paper（PLAN.md §183）。
- 当前状态：
  - 已完成单 workload smoke（`runs/smoke_blender_26411_v2/`），证明探针
    工作正常，blender 上 tage 高编号 table + microtage 大面积闲置。
  - 已完成 W=10K sweep smoke（`runs/smoke_W10K/`），证明 `-P
    system.cpu[0].branchPred.phaseSizeByInst=N` 能改 phase 长度。
  - **下一步要 13 workloads × 3 W 值 = 39 跑，单机循环太慢**，必须用
    `mytools/distributed-trace-scheduler` 多机分布式调度。

## Why distributed-trace-scheduler 不能直接复用

`tools/distributed-trace-scheduler/scripts/run_trace_gem5.sh` 仅支持
trace-mode（`--enable-trace-mode --trace-file=<champsim/cbp>`），不支持
cpt-mode（`--generic-rv-cpt=<gcb_v_zstd>` + GCB_RESTORER）。SWAY 的
SPEC17 / DaCapo workloads 都是 cpt 格式，必须新写一个并列后端脚本。

## What I already know（前一会话固化结论）

### Worktree / branch

- 在 `~/project/GEM5-sway/` worktree，branch `sway/phase-stranded-probe`，
  base `trace-new`。
- HEAD commit `4222d246ea` 已包含 MBTB + BTBTAGE per-way visit counters
  + DecoupledBPUWithBTB 的 `phaseSizeByInst` / `subPhaseRatio` Param 化。
- **Worktree 里 `build/RISCV/gem5.opt` 还没编**——开干前先 `cd
  ~/project/GEM5-sway && scons build/RISCV/gem5.opt -j50`（hostname
  node001 是 384 核胖节点；耗时约 10-15 min）。原 trace-new 的
  `build/RISCV` 已被清除，避免误用旧 binary。

### 跑通的命令模板（单机 baseline）

```bash
export GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so
export GCB_RESTORER=/nfs/home/share/gem5_ci/tools/normal-gcb-restorer.bin   # 关键！别用 ~/project/NEMU 那个

~/project/GEM5-sway/build/RISCV/gem5.opt \
  --outdir=<OUTDIR> --stats-file=<OUTDIR>/stats.txt \
  ~/project/GEM5-sway/configs/example/kmhv3.py \
  --generic-rv-cpt=<cpt_zstd_path> \
  --bp-type=DecoupledBPUWithBTB \
  --ideal-kmhv3 \
  --mem-size=128GB \
  --maxinsts=${XS_MAX_INSTS} \
  --param system.enable_mem_dedup=True \
  --param 'system.cpu[0].enable_mem_dedup=True' \
  --param 'system.cpu[0].enable_difftest=False' \
  --param "system.cpu[0].branchPred.phaseSizeByInst=${XS_PHASE_SIZE}"
```

### 踩过的坑

1. **不能用 `--maxtime`** 设运行时长。`trace-new` commit `e7a2c5a829
   "sim: When simulate XS system dont treate a0 as exit delay"` 改了
   `m5exit` 语义——XS system 下任何 m5_exit 立即退出，而 GCPT restore
   前几条就会撞到。**只能用 `--maxinsts=N`**。
2. **GCB_RESTORER 必须是
   `/nfs/home/share/gem5_ci/tools/normal-gcb-restorer.bin`（4.2 KB）**，
   不是 `~/project/NEMU/resource/gcpt_restore/build/gcpt.bin`（1.0 MB
   那个会在 boot 头几条触发 m5_exit）。
3. **Difftest 必须关**：`--param 'system.cpu[0].enable_difftest=False'`，
   否则会因 SWAY 探针之外的不相关原因 panic（unified-frontend 同 cpt
   formal 跑就因此挂）。
4. trace-new src 之前需要 Python 3.12 兼容（已在 trace-new HEAD 里），
   build artifact cache 不能跨分支复用。**clean build 必走**。

### 输出 CSV

成功跑完后 `<OUTDIR>/sway_stranded_by_phase.csv` 形如：

```csv
phaseID,scope,total_ways,valid_ways,active_ways
1,mbtb_sram0,4096,28,28
1,mbtb_sram1,4096,28,27
1,tage_t0,4096,34,32
...
1,microtage_t0,1024,0,0
```

每个 phase 11 行（mbtb_sram0/1 + tage_t0..t7 + microtage_t0），phaseID
从 1 开始，**phase 0 不 dump**（这是 `processPhase` 的语义）。

## Open questions（请下一会话决定，不要替用户拍板）

1. **W sweep 是否走 backend env 注入还是 per-task scheduler 参数？**
   - 倾向：env 注入（`XS_PHASE_SIZE_BY_INST`），三档 W 跑三遍调度，
     每遍换一个 OUTDIR root（如 `runs/W10K/`、`runs/W50K/`、
     `runs/W200K/`）。与现有 `XS_MAX_INSTS` 模式一致。
   - 也可考虑在 workload list 里加扩展列 `phase_size`，但 README 说
     "明确需求为无扩展字段"，先不动。
2. **workload list 文件用哪个？** PLAN §V 列了 SPEC int frontend-bound
   子集（gcc / perlbench / xalancbmk / omnetpp）+ DaCapo javac +
   championship traces (IPC-1 / CVP-1 / CBP-2025 indirect subset)。
   - cpt 路径前缀已知：`/nfs/home/share/zyy/spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/checkpoint-0-0-0/<workload>/<id>/_<id>_<weight>_.zstd`
   - DaCapo / championship 路径需要确认存在性，user 可能要现场给。
3. **MAX_INSTS 选多大？**
   - smoke 用了 2M，CSV 拿到 20 个 phase。
   - Figure 1 motivation 要 phase 数足够（>= 50）做箱线/CDF；建议
     **5M instr per workload**（W=100K 出 50 phase；W=10K 出 500 phase；
     W=200K 出 25 phase）。可由 user 在第一次跑前调。

## Concrete implementation plan（建议下一会话照做）

### Step 1 — 加 `scripts/run_cpt_gem5.sh`

形态参考 `scripts/run_trace_gem5.sh`，唯一区别是 cmdline 拼装：

- 消费的环境变量：
  - `GEM5_HOME`（必须）/ `GEM5_BUILD_TYPE`（默认 opt）
  - `GCBV_REF_SO`（默认 `/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so`）
  - `GCB_RESTORER`（默认 `/nfs/home/share/gem5_ci/tools/normal-gcb-restorer.bin`）
  - `XS_MAX_INSTS`（默认 1000000）
  - `XS_PHASE_SIZE_BY_INST`（默认 100000，注入到 `-P
    system.cpu[0].branchPred.phaseSizeByInst=N`）
  - `XS_MEM_SIZE`（默认 128GB）
  - `OUTDIR`（scheduler 注入）
- 位置参数：`<cpt_path>`（GCB-V `.zstd`/`.gz`）
- 不要 `--enable-trace-mode`、`--trace-file`，**改用 `--generic-rv-cpt`**
- 不要 `--trace-enable-decoupled-bp`，cpt 模式默认 BPU 就启用
- 别忘三个 `--param ...enable_mem_dedup`、`enable_difftest=False` 的硬性需求

不要新加 SimObject Param；W 只走 `-P` 注入。

### Step 2 — 加 `configs/backend.gem5_cpt.sample.json`

形态参考 `configs/backend.gem5.sample.json`：

```json
{
  "env": {
    "GEM5_HOME": "/nfs/home/goulingrui/project/GEM5-sway",
    "GCBV_REF_SO": "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so",
    "GCB_RESTORER": "/nfs/home/share/gem5_ci/tools/normal-gcb-restorer.bin",
    "XS_MAX_INSTS": "5000000",
    "XS_PHASE_SIZE_BY_INST": "100000"
  },
  "args": []
}
```

Sample 文件用默认 W=100K；W sweep 走外层覆盖 `--backend-config` 的方式
传第二/三份配置（或者直接 export 同名 env 覆盖 sample，看
distributed_trace_scheduler.py 里 backend env 与 process env 的合并优先级
——下一会话先确认）。

### Step 3 — 写 workload list

放在 `~/project/papers/sway-paper/runs/configs/`（不污染 mytools 库）。
格式按 mytools README：`name path skip fw dw sample`，其中：
- `name`：`<workload>_<checkpoint_id>`（如 `blender_26411`）
- `path`：cpt 相对 trace_root 的路径，scheduler 自动加 `.zstd` 后缀
- `skip fw dw sample`：cpt 模式不需要这几个字段，用占位 `0 0 0 0`
  （需要确认 scheduler 不会校验这几个字段非零；如果会，看 `--workload-list`
  的字段解析逻辑能否容忍）

### Step 4 — 自检（不必跑大批）

```bash
# 单机本地 dummy 测调度器能拉起 run_cpt_gem5.sh
ssh localhost true || exit 1
python ~/project/mytools/tools/distributed-trace-scheduler/distributed_trace_scheduler.py \
  --backend shell \
  --arch-script ~/project/mytools/tools/distributed-trace-scheduler/scripts/run_cpt_gem5.sh \
  --backend-config ~/project/mytools/tools/distributed-trace-scheduler/configs/backend.gem5_cpt.sample.json \
  --work-root ~/project/papers/sway-paper/runs/cpt_smoke_dts \
  --workload-list <workload_list_with_just_blender_26411> \
  --trace-root /nfs/home/share/zyy/spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/checkpoint-0-0-0 \
  --server-list <(echo localhost) \
  --tag smoke_dts \
  --max-global 1 \
  --max-threads-per-host 1 \
  --poll-seconds 5
```

检查 `runs/cpt_smoke_dts/smoke_dts/blender_26411/m5out/sway_stranded_by_phase.csv`
是否生成且形态对，确认调度链路通。

### Step 5 — commit + 关闭本任务

mytools 是独立 git 仓（不是 GEM5-sway 仓），新文件 commit 到
`~/project/mytools` 的当前分支。Trellis task 标 completed，
unblock 下一个 `06-15-sway-batch-pending9-figure1`。

## Done definition

- [ ] `~/project/mytools/tools/distributed-trace-scheduler/scripts/run_cpt_gem5.sh` 存在且
      `./run_cpt_gem5.sh --help` 自带帮助
- [ ] `~/project/mytools/tools/distributed-trace-scheduler/configs/backend.gem5_cpt.sample.json` 存在
- [ ] 单 workload (blender_26411) 通过 distributed-trace-scheduler 跑完，
      `sway_stranded_by_phase.csv` 形态正确
- [ ] mytools 仓 commit（含 scripts/ + configs/ + README 一段简述）
- [ ] Trellis task.json `status: completed`，写入 `commit` 字段
