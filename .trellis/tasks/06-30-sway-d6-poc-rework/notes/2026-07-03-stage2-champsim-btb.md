# Stage 2 Champsim BTB-intensive baseline sweep

## Sweep Manifest
- Manifest: `runs/stage2_baseline_expanded/workload_manifest.tsv`
- Worklist: `runs/stage2_baseline_expanded/worklists/champsim_btb.lst`
- Output dir: `runs/stage2_baseline_expanded/champsim_btb`
- Attempted: 30
- Completed: 0
- Failed: 30
- Excluded before scheduling: `cvp1_public_compute_int_32` (known trace-new failure).

## Failure List
| status | id | reason |
|---|---|---|
| excluded | `cvp1_public_compute_int_32` | known-broken trace-new Fetch::lookupAndUpdateNextPC |
| abort | `cvp1_public_srv_0` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_1` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_10` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_11` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_12` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_13` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_14` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_15` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_16` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_17` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_18` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_19` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_2` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_20` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_21` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_22` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_23` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_24` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_25` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_26` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_3` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_32` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_40` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_41` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_42` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_43` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_44` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_45` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_47` | Fetch::lookupAndUpdateNextPC stream range assertion |
| abort | `cvp1_public_srv_50` | Fetch::lookupAndUpdateNextPC stream range assertion |

## Top-5 Workloads Per Direction
No completed workloads with analyzable stats.
