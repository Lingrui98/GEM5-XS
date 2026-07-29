# SWAY Stage 4 — 可行性苏格拉底讨论记录（rolling）

> **Date**: 2026-07-26（进行中）
> **Format**: deep-research Socratic 五层对话（Layer 1 问题框定 ✓ / Layer 2 方法论 ✓ / Layer 3 证据设计 — 进行中）
> **Audience**: glr + 后续接手 agent。记录已收敛的 INSIGHT、DA 检查点修订、实验设计骨架。

## 0. 收敛后的研究问题（Layer 1 定稿）

> 在布线可行的受限设计点（per-bank tight-slot 捐赠，MbtbTightSlotWay=3）上，压力错配驱动的预测器资源重分配，能否以一个**足够简单、可验证**的机制，带来 **≥1% geomean ΔIPC**？

1% 阈值与简洁性都是判据的一部分。简洁性 = 机制价值的一半（用户对"工业界缺席"的 refined model：有收益的点物理不可行 / 验证太贵 / 复杂机制挤压后期空间）。

## 1. 已收敛的决策（Layer 2 + DA 检查点修订）

- **实验顺序**：收益上界实验（E1）与噪声/异质性审计（E0'）排在 frozen-z 守卫修复与 4B 之前。若上界不达 1% ⇒ 设计点死亡，项目转向（用户已拍板）。
- **Kill 判据**（DA-1 修订）：对 T7、T3 两个 donee 分别做静态容量添加，取最大值仍 <1% 才判死。
- **Live 判据**（DA-2）：静态过线时，动态闭环须回收 ≥70% 的静态收益。
- **先知照单执行仪器**：降级为可选诊断（时刻表陈旧洞 + 单次捐赠效应量可能低于异质性地板）。
- **静态实验 = 纯配置**：TAGE tableSizes/numways 是 SimObject 参数，无需改 C++/重编。
- **"噪声"概念修正**（用户挑战成立）：gem5 确定性 ⇒ 同 offset 无仪器噪声；offset 间差异 = 真实相位异质性。E0 重新定义为**效应跨段异质性**测量，配对设计（treatment@k vs base@k）。

## 2. 候选 workload 名单（DA-4，train-only）

依据 `runs/stage3_beta_replay/beta_events.csv` θ=1.5 fire/crossing 排名，剔除 held-out（gcc_ref32_O3_888、x264_pass2_9146、xz_combined_26239 虽排名靠前但冻结不可用）：

| workload | θ=1.5 fires | crossings | max_zd | 角色 |
|---|---|---|---|---|
| perlbench_spam_16137 | 22 | 59 | 4.44 | 错配最强（donee z 高达 2431）|
| gcc_pp_O3_2441 | 8 | 34 | 2.95 | |
| gcc_pp_O2_1869 | 8 | 29 | 3.03 | 工作马，基础设施最熟 |
| perlbench_diff_20021 | 7 | 11 | 3.72 | |
| mcf_17135 | 4 | 6 | 3.32 | live 验证锚点 |

## 3. Rolling-IPC 审计结果（2026-07-26，task #4）

方法：stage3 基线 run 的逐 phase MPKI（`topMispredictByPhase.csv`，100K inst/phase，边界@201）。**注意：行为代理，非 IPC 直接测量。**

| workload | 边界切换（前5 sample z） | sample CV | 振荡 | 趋势 | 判定 |
|---|---|---|---|---|---|
| perlbench_spam_16137 | −0.5..+0.1，干净 | 0.79 | 弱（r=.40@lag2） | −0.30σ | 单 offset 可解释；sample 段 MPKI 均值 3× warmup（真 regime 差异=错配怪物成因）|
| gcc_pp_O3_2441 | 干净 | 0.43 | 弱 | −0.76σ | 单 offset 可 |
| gcc_pp_O2_1869 | −1.8..−1.2，一侧偏移但 <2σ | 0.39 | 弱 | −0.30σ | 温和 regime 切换且持续（非 warmup 病理——warmup 不足会衰减回弹，此为一贯偏低）；单 offset 可 |
| perlbench_diff_20021 | 干净 | 0.74 | 弱 | −0.02σ | 单 offset 可 |
| mcf_17135 | 干净 | 0.18 | 强（r=.69@lag2 ≈ 200K 周期） | −0.06σ | 窗口覆盖 ~100 个振荡周期，采样极好 |

**结论**：5 个 workload 边界均无 >2σ 跳变（20M warmup 充分），sample 窗口无半周期骑行 ⇒ **E1 第一轮单 offset（offset 0）即可**；第二轮仅对落在 0.5%~2% 判据带或符号可疑者补 offset +1M（自适应，成本 25→25~35 组）。

## 4. E1 实验骨架（待 glr 批准）

几何约束：TAGE table 容量 = sets × ways，sets 须 2 的幂（index 掩码）；ways 无幂约束。T7 现状 2048 sets × 2 ways = 4096 entries。tight slot = 1024 entries（单 bank 1024 sets）。

两口径（不可精确 entry-for-entry，取可表达括号）：
- **保守口径 ≈ way 对 way**：T7 numways 2→3 ⇒ +2048 entries（+50%）
- **慷慨口径 ≈ bit 等价**：T7 sets 2048→4096 ⇒ +4096 entries（+100%）（BTB 胖 entry ~70b vs TAGE 瘦 entry ~18b，bit 预算 ≈ 3500-4000，4096 为其上界）

Run 矩阵：5 workloads × {base, T7+50%, T7+100%, T3+50%, T3+100%} × offset 0 = **25 组 40M run**。

判读：
- max(T7, T3) 最高口径 ΔIPC < 1% ⇒ **设计点死亡，转向**；
- 慷慨口径过线、保守口径不过线 ⇒ 瓶颈在使用模型（conserved storage 交付不出 bit 价值），本身是有价值的结论；
- 过线 ⇒ 修 frozen-z 守卫（方案 b），测动态恢复系数（须 ≥70%），再 4B。

## 5. 悬而未决

- E1 run 矩阵与判读规则的最终批准（Layer 3 收尾）。
- 若设计点死亡：转向方向清单（预注册，防事后钓鱼）——Layer 4 议题。
- frozen-z 守卫方案 (b) 与 T4 处置：挂起，待 E1 判生后才相关。

## 6. Layer 3 收尾与 E1 终判（2026-07-28，Claude Code session）

- E1 矩阵经 glr 批准按骨架执行：25 组（offset 0）+ 10 组自适应 round 2（offset +1M，2 个判据带 gcc）。
- **终判：KILL**。best-arm geomean +0.385%，全局最大单点 +0.745%（gcc_pp_O2 t3p100 off+1M），两个 offset 一致 <1%。SWAY tight slot 交付量（1024 entries）仅为保守口径（+2048）的一半，动态机制恢复系数再高也够不到 1%。
- 一致模式：T3 ≫ T7 donee（T7 普遍负收益）；mcf 对容量不敏感；gcc 效应跨 offset 可复现但结构性偏小。
- 完整记录：`sway-paper/docs/experiments/2026-07-28-sway-stage4-e1-upper-bound.md`；run 契约 `runs/stage4_e1/README.md`。
- 悬而未决关闭：E1 矩阵批准 ✓；frozen-z 守卫与 T4 处置随设计点死亡转为 archival。
- **Layer 4 待 glr**：转向方向清单预注册（characterization fallback / BTBP 叙事合并 / SWAY 线封存），未注册前不开新机制工作。

## 7. CORRIGENDUM（2026-07-29）：§6 的 KILL 判词作废，降为"判局执行无效"

GPT-5.6 (xhigh) 独立复核发现两个执行错误（Claude Code 已逐项盘证属实）：

1. **t3p100 张量下标 off-by-one**：实际改的是 index 2（T2）而非 index 3（T3）。预注册的 T3+100% **从未执行**；§6 中全部"t3p100"数字实为 T2+100%。
2. **tight slot 交付量低估 3×**：packing factor = ⌊70/19⌋ = 3（sway_realloc.hh + btb.test.cc:239），单 slot 实际交付 **3072** TAGE subentries（非 1024）。§4 骨架与 §6 判词中"+2048 = 2× 交付"实为 0.67×（欠交付，不构成上界）。

**修正后状态**：T7 kill 依然成立（两臂 ≤0）；T3 无有效过交付测量，+2048 时 +0.41~0.65% 且随容量上升——T3+100% 必须补跑才能终判。T2+100%（误标）+0.745% 是意外发现（T2 在 frozen donee_set 之外，归 Layer 4 候选）。**判词降级为"高度悲观，判局需一次性 E1′ 纠错（正确 T3+100%）"**，详见实验记录 CORRIGENDUM 段。§6 的"动态机制数学上不可能够到 1%"论证随错误 #2 一并失效。

## 8. E1′ 终判（2026-07-29）：设计点**勉强存活**——单 workload 过线，进入 DA-2 动态恢复判定

E1′（正确 T3 `tableSizes[3]=4096`，config.ini 逐项盘证下标无误）× 5 workloads × offset 0 + 判据带补 offset +1M：

| workload | T3+100% (off 0) | off +1M |
|---|---:|---:|
| gcc_pp_O2_1869 | **+1.074%** | **+1.032%** ✅ |
| gcc_pp_O3_2441 | +0.580% | +0.384% |
| perlbench_spam_16137 | +0.050% | — |
| perlbench_diff_20021 | +0.107% | — |
| mcf_17135 | −0.414% | — |

- **kill 判据不成立**：gcc_pp_O2 两个 offset 均 ≥1%（+1.074 / +1.032），按预注册进入 live 路径：frozen-z 守卫（方案 b）→ DA-2 动态恢复测试（≥70%）。
- **但效应集中于单一 workload**（gcc 家族），修正后 best-arm geomean ≈ +0.42%，Layer-1 的 "≥1% geomean" 仍未达；非单调性确认（perlbench_spam T3+50% > T3+100%；mcf 负单调）。
- **Layer 4 待 glr**：是否投入 frozen-z 守卫 + gcc_pp_O2 最小闭环动态恢复测试（预计 SWAY 实际交付 3072 entries < 静态 +4096，70% 恢复 ≈ +0.75% 单 workload），还是按"geomean 不达"就此止步。两条路都符合预注册；方向清单预注册要求不变。

## 9. E1-trace（2026-07-29）：srv regime 同样不给设计点一个家

glr 指出数据集多样性不足（= codex MAJOR-4），批准 30 srv × 5 arms（150 runs，taskpool tag `sway-stage4-e1trace-20260729`，全部 succeeded，契约 `runs/stage4_e1trace/README.md`）：

- ≥1% trace：**1/30**（srv_11 t3p50 +1.558%，且非单调 t3p100 +0.003%——阈值伪影）；冻结判读需 ≥3 → 未达
- family geomean of best-arm：**+0.173%**；median +0.114%；T3/T7 胜场 17/13（T7 在 server regime 同样无主场）
- **全证据链**：SPEC 1/5 过线（gcc_pp_O2 +1.07/+1.03 复现）+ srv 1/30 过线 = **2/35（5.7%），geomean ~+0.2-0.4%**。TAGE 容量故事无可防御宽基，T7 两 regime 皆死。
- 对 Layer-4 的含义：效应不泛化。DA-2 即使满分（gcc_pp_O2 单点 ~+0.75%）也救不了 geomean——"DA-2 vs 止步"的权衡向**止步**倾斜。
