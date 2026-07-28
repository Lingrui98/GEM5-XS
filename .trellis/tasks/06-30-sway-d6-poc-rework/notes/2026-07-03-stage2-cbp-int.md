# Stage 2 CBP int baseline sweep

## Sweep Manifest
- Manifest: `runs/stage2_baseline_expanded/workload_manifest.tsv`
- Worklist: `runs/stage2_baseline_expanded/worklists/cbp_int.lst`
- Output dir: `runs/stage2_baseline_expanded/cbp_int`
- Attempted: 4
- Completed: 4
- Failed: 0

## Failure List
No failures.

## Top-5 Workloads Per Direction
### MBTB->ITTAGE
| rank | id | donee_score | donor_score | target | tage_best |
|---:|---|---:|---:|---:|---|
| 1 | `int_14_trace` | 1.50118e-05 | 0.130776 | 1 | tage_t7 |
| 2 | `int_13_trace` | 4.97988e-06 | 0.0914187 | 1 | tage_t7 |
| 3 | `int_12_trace` | 0 | 0.713191 | 0 | tage_t3 |
| 4 | `int_15_trace` | 0 | 0 | 0 | tage_t3 |

### ITTAGE->MBTB
| rank | id | donee_score | donor_score | target | tage_best |
|---:|---|---:|---:|---:|---|
| 1 | `int_14_trace` | 0.000156192 | 0.71596 | 1 | tage_t7 |
| 2 | `int_12_trace` | 1.55205e-06 | 0.388769 | 1 | tage_t3 |
| 3 | `int_13_trace` | 6.2481e-07 | 0.6864 | 0 | tage_t7 |
| 4 | `int_15_trace` | 0 | 0 | 0 | tage_t3 |

### MBTB->TAGE
| rank | id | donee_score | donor_score | target | tage_best |
|---:|---|---:|---:|---:|---|
| 1 | `int_13_trace` | 0.0382534 | 0.0914187 | 1 | tage_t7 |
| 2 | `int_14_trace` | 2.17436e-05 | 0.130776 | 1 | tage_t7 |
| 3 | `int_12_trace` | 0 | 0.713191 | 0 | tage_t3 |
| 4 | `int_15_trace` | 0 | 0 | 0 | tage_t3 |

### ITTAGE->TAGE
| rank | id | donee_score | donor_score | target | tage_best |
|---:|---|---:|---:|---:|---|
| 1 | `int_13_trace` | 0.0382534 | 0.6864 | 1 | tage_t7 |
| 2 | `int_14_trace` | 2.17436e-05 | 0.71596 | 1 | tage_t7 |
| 3 | `int_12_trace` | 0 | 0.388769 | 0 | tage_t3 |
| 4 | `int_15_trace` | 0 | 0 | 0 | tage_t3 |
