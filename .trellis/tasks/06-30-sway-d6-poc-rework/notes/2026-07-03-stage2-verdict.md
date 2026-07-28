# Stage 2 baseline sweep verdict

## Sweep Summary
| family | attempted | completed | failed |
|---|---:|---:|---:|
| spec17 | 36 | 36 | 0 |
| champsim_btb | 30 | 0 | 30 |
| cbp_int | 4 | 4 | 0 |

Champsim BTB-intensive additionally excluded `cvp1_public_compute_int_32` before scheduling as the prompt-specified known-broken trace. The other 30 selected champsim traces all aborted at `Fetch::lookupAndUpdateNextPC`, so the target analysis below is based on the 40 completed SPEC17 + CBP-int runs.

## Direction x Family Target Count
| direction | SPEC17 | champsim-btb | CBP-int | total |
|---|---:|---:|---:|---:|
| MBTB->ITTAGE | 18 | 0 | 2 | 20 |
| ITTAGE->MBTB | 18 | 0 | 2 | 20 |
| MBTB->TAGE | 18 | 0 | 2 | 20 |
| ITTAGE->TAGE | 18 | 0 | 2 | 20 |

## Top Target Workloads Per Direction
- MBTB->ITTAGE: `spec17/gcc_pp_O2_1869`, `spec17/gcc_pp_O3_2441`, `spec17/gcc_small_O3_5489`, `spec17/gcc_ref32_O3_888`, `spec17/cam4_118108`, `spec17/xz_combined_26239`, `spec17/xz_cld_5977`, `spec17/gcc_ref32_O5_889`, `spec17/perlbench_diff_20021`, `spec17/x264_seek_80312`, `spec17/x264_pass1_17232`, `spec17/omnetpp_21279`, `spec17/x264_pass2_9146`, `spec17/xalancbmk_6102`, `spec17/leela_60438`, `spec17/povray_89004`, `spec17/wrf_13221`, `spec17/perlbench_spam_16137`, `cbp_int/int_14_trace`, `cbp_int/int_13_trace`
- ITTAGE->MBTB: `spec17/gcc_pp_O2_1869`, `spec17/gcc_pp_O3_2441`, `spec17/gcc_small_O3_5489`, `spec17/gcc_ref32_O3_888`, `spec17/cam4_118108`, `spec17/gcc_ref32_O5_889`, `spec17/perlbench_diff_20021`, `spec17/x264_seek_80312`, `cbp_int/int_14_trace`, `spec17/x264_pass2_9146`, `spec17/deepsjeng_88772`, `spec17/x264_pass1_17232`, `spec17/leela_60438`, `spec17/omnetpp_21279`, `spec17/exchange2_101257`, `spec17/xalancbmk_6102`, `spec17/blender_26411`, `spec17/povray_89004`, `spec17/perlbench_split_8843`, `cbp_int/int_12_trace`
- MBTB->TAGE: `spec17/omnetpp_21279`, `spec17/deepsjeng_88772`, `spec17/x264_pass1_17232`, `spec17/gcc_pp_O3_2441`, `spec17/exchange2_101257`, `spec17/gcc_pp_O2_1869`, `spec17/gcc_small_O3_5489`, `spec17/leela_60438`, `spec17/xalancbmk_6102`, `spec17/gcc_ref32_O5_889`, `spec17/perlbench_spam_16137`, `spec17/xz_combined_26239`, `spec17/blender_26411`, `cbp_int/int_13_trace`, `spec17/gcc_ref32_O3_888`, `spec17/xz_cld_5977`, `spec17/povray_89004`, `spec17/x264_pass2_9146`, `spec17/x264_seek_80312`, `cbp_int/int_14_trace`
- ITTAGE->TAGE: `spec17/omnetpp_21279`, `spec17/deepsjeng_88772`, `spec17/x264_pass1_17232`, `spec17/gcc_pp_O3_2441`, `spec17/exchange2_101257`, `spec17/gcc_pp_O2_1869`, `spec17/gcc_small_O3_5489`, `spec17/leela_60438`, `spec17/xalancbmk_6102`, `spec17/gcc_ref32_O5_889`, `spec17/perlbench_spam_16137`, `spec17/xz_combined_26239`, `spec17/blender_26411`, `cbp_int/int_13_trace`, `spec17/gcc_ref32_O3_888`, `spec17/xz_cld_5977`, `spec17/povray_89004`, `spec17/x264_pass2_9146`, `spec17/x264_seek_80312`, `cbp_int/int_14_trace`

## Recommendation
- Proceed to Stage 3 offline decision-procedure simulation: every legal direction has 20 target workloads, above the >=5 gate.
- Treat champsim coverage as unresolved: all 30 scheduled BTB-intensive traces hit the same `Fetch::lookupAndUpdateNextPC` assertion class as the known broken trace, so Stage 3 should either use SPEC17+CBP first or fix/replace champsim trace-mode coverage before making a non-SPEC coverage claim.
- The B-b/B-c whole-run cut is a workload-picking filter only; it does not validate the phase-level beta controller.
