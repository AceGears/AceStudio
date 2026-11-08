---
name: gpu-profiling
description: Collect repeatable CPU, GPU, RAM and VRAM measurements for AceStudio (app startup, AI worker jobs, previews, idle) on Windows with an NVIDIA GPU, and record before/after comparisons in docs/agents/PERFORMANCE_BASELINE.md. Use for any performance claim, optimization, or regression check.
---

# GPU / System Profiling

## 1. Record the environment
```powershell
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv
Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores
(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
```
Plus: AceStudio version, worker package versions, power plan, other GPU apps closed.

## 2. Sample during a run
GPU (1 s interval, to CSV):
```powershell
nvidia-smi --query-gpu=timestamp,utilization.gpu,memory.used,power.draw --format=csv -l 1 > gpu.csv
```
Process CPU/RAM (AceStudio + python worker):
```powershell
Get-Counter '\Process(AceStudio)\% Processor Time','\Process(python*)\% Processor Time','\Process(AceStudio)\Working Set - Private' -SampleInterval 1 -MaxSamples 60
```
Inside the worker (Python), around each stage:
```python
torch.cuda.reset_peak_memory_stats(); t=time.perf_counter()
...  # stage
torch.cuda.synchronize(); dt=time.perf_counter()-t; peak=torch.cuda.max_memory_allocated()
```

## 3. Protocol
- Warm-up 1 run, then ≥ 3 measured runs (5 for < 1 s operations). Report **median** and min–max.
- Separate cold (first model load) from warm numbers.
- Idle test: 2 min after last job — CPU %, VRAM held by worker, RAM.
- Startup: time from process start to main window responsive (use a log timestamp or UI automation), cold and warm.
- Never run two GPU-heavy benchmarks at once; stop the worker between runs.

## 4. Record → `docs/agents/PERFORMANCE_BASELINE.md`
| Metric | Before | After | Δ | Runs | Notes |
A change is an improvement only if the median improves beyond the run-to-run spread and quality is unchanged (fixed-seed comparison).
