# Performance Baseline

Machine: RTX 4070 Laptop 8 GB (driver 621.14), Core Ultra 9 185H, 31.5 GB RAM. Source: AceStudio's own benchmark folders from 2026-11-03 and 2026-11-04 (worker 0.10.0). Each run has 8 warm cases per mode; values are medians [min–max].

| Metric | 11-03 1024² | 11-03 512² | 11-04 1024² | 11-04 512² |
|---|---|---|---|---|
| Warm total | 23.4 [22.8–23.9] s | 18.3 [17.4–19.6] s | 29.0 [25.5–31.1] s | 9.8 [9.1–10.8] s |
| Diffusion (4 steps) | 14.6 s | 8.5 s | 15.2 s | 8.4 s |
| s/step | 3.66 | 2.11 | 3.79 | 2.10 |
| Cut-out | 8.0 s | 9.0 s | 12.5 s | 1.05 s |
| Cold model load | 31.9–33.8 s | 20.3–20.9 s | | |
| Device VRAM peak | 8.00 GB | | | |
| RAM peak (cold) | | | 31.51 GB | |

## Targets (proposed, to be confirmed after phase 5 profiling)
| Metric | Target |
|---|---|
| Warm 1024² generate + cut-out | ≤ 15 s |
| Cold load | ≤ 15 s, with RAM peak ≤ 20 GB |
| VRAM peak | ≤ 7 GB |
| Idle worker in Eco mode | ≤ 500 MB VRAM after 2 min |
| Regression | > 10% fails |

Not yet measured: app startup, preview fps, idle CPU.
