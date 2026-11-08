# AI worker before/after benchmark (agents/inference)

Procedure for measuring the `agents/inference` worker changes on the reference PC
(RTX 4070 Laptop 8 GB, 31.5 GB RAM). Nothing in this file has been run yet: the
changes were verified with unit tests only (no real models, no generation).

| Commit | Change | What to measure |
|---|---|---|
| `d2a63ef` | ONNX Runtime arena cap (`gpu_mem_limit`), HEURISTIC cuDNN search | `onnxSession`, `onnxRun`, `cutout`, device VRAM peak |
| `b2e2fde` | FLUX.2 klein staged load (text encoder cast to FP8 before the transformer loads) | cold `load` stage, process `peakRssGb`, system RAM peak |
| `9545514` | Idle trim/unload policy | worker VRAM and RAM after idle |
| `f468875` | OOM recovery (retry once with less memory) | correctness only (forced OOM) |
| `0a4d23d` | Real-ESRGAN cache, GPU-side tile stitching, background preview writes | upscale job time, SDXL step time with live preview |

"Before" is `dd5035c` (the branch point). "After" is the tip of `agents/inference`.

## 0. Rules (from `.claude/skills/gpu-profiling`)
- Close other GPU programs (browsers with video, games, OBS). Plugged in, Windows power plan
  *Best performance*, laptop vendor mode the same for both runs.
- Same app build for before and after; only the worker folder changes.
- 1 warm-up, then at least 3 measured runs per variant. Report the median and min–max.
- Never run two GPU benchmarks at once. Fully exit AceStudio between variants.
- Keep the Resource profile at **Balanced** for sections 2–4 (it is recorded in `report.json`
  under `machine.profile`), unless a section says otherwise.

## 1. Environment record
```powershell
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv
Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores
(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
& "$env:LOCALAPPDATA\AceStudio\ai\python\python.exe" -c "import torch, diffusers, transformers, onnxruntime; print(torch.__version__, diffusers.__version__, transformers.__version__, onnxruntime.__version__)"
```

## 2. Selecting the worker (before vs after)
The app loads the worker from `ACESTUDIO_WORKER_DIR` when it is set (`core/src/AiWorker.cpp`).
```powershell
# one-time: a checkout of the base commit next to this worktree
git -C "C:\Users\Danie\OneDrive\Documents\Claude Projects\AceStudio-worktrees\inference" worktree add ..\inference-before dd5035c

# BEFORE
$env:ACESTUDIO_WORKER_DIR = "C:\Users\Danie\OneDrive\Documents\Claude Projects\AceStudio-worktrees\inference-before\ai_worker"
& "<path to AceStudio.exe>"

# AFTER
$env:ACESTUDIO_WORKER_DIR = "C:\Users\Danie\OneDrive\Documents\Claude Projects\AceStudio-worktrees\inference\ai_worker"
& "<path to AceStudio.exe>"
```
Check the first worker log line in AceStudio's log (`AceStudio worker 0.10.0 ... ready`) and,
for "after", the presence of `Idle for` / `oomRetry` features is the quickest sanity check
that the right folder is loaded.

Optional knobs for the "after" worker (set before starting AceStudio):
- `ACESTUDIO_ORT_GPU_MEM_MB` — override the ONNX arena cap (`0` = no cap, the old behaviour).
- `ACESTUDIO_IDLE_UNLOAD_SECONDS` — worker-side model unload after this idle time.

## 3. Generation + cut-out + cold load (app Benchmark)
AI Studio › Generator › **Benchmark…** (`app/src/ai/Benchmark.cpp`). It restarts the worker
(cold load), then runs fixed cases with fixed seeds (char32, rpg64, monster128, dragon128,
campfire64, idle/walk/flap/fire animations, a 4-item pack) and writes `report.json` and
`report.md` plus one PNG per case to the chosen folder.

1. Start AceStudio with the BEFORE worker. Open Benchmark…, tick **both** pipelines
   (original and optimised), report folder `...\bench-before-1`, Start. Wait for "Done".
2. Exit AceStudio. Repeat into `bench-before-2` and `bench-before-3`. Each run restarts the
   worker, so each contributes one cold-load sample and eight warm samples per mode; do one
   extra unrecorded run first as the warm-up (disk cache).
3. Same with the AFTER worker into `bench-after-1..3`.
4. While each run is going, sample the GPU (separate PowerShell):
   ```powershell
   nvidia-smi --query-gpu=timestamp,utilization.gpu,memory.used,power.draw --format=csv -l 1 > gpu-<variant>-<n>.csv
   ```

Read from each `report.json` (per case, `worker.metrics` and `worker.*`):

| Metric | Field | Expected direction (after) |
|---|---|---|
| Cold load | first `gen` case of each mode: `worker.loadSeconds`, `worker.metrics.stages.load` | lower or equal |
| Process RAM peak, cold | first case: `worker.metrics.peakRssGb`; system: `system.ramPeakGb` | lower (target ≤ 20 GB) |
| Cut-out | `worker.metrics.stages.cutout`, `onnxSession`, `onnxRun` | `onnxSession` ≈ 0 on warm cases in **optimised** mode; first-run `onnxRun` lower (HEURISTIC search) |
| Diffusion | `worker.metrics.stages.diffusion`, `worker.secondsPerStep` | unchanged (within spread) — any slowdown > 10 % blocks |
| Device VRAM peak | `worker.metrics.peakDeviceVramGb`, `system.vramPeakGb` | lower or equal (target ≤ 7 GB) |
| Total | `appSeconds` | lower or equal |
| Quality | `qualityScore`, and the case PNGs | identical images for identical seeds (diff the PNGs; see §7) |

Note: in the **original** pipeline mode the worker still releases the cut-out session after
every job on the 8 GB card (`keepCutoutLoaded` is false and `optimize` is off), so
`onnxSession` stays at several seconds there by design; the arena cap only changes what the
optimised mode can keep.

## 4. Cut-out session kept next to diffusion
Purpose: confirm the capped session no longer pushes klein into shared memory.
1. AFTER worker, Balanced, optimisations on (Settings › Generation). Generate the same
   128×128 transparent character 5 times in a row (same seed).
2. In the log, each job after the first should say
   `Background removal kept on the GPU (x.x GB free)` and `... session 0.0 s, run ~1 s`.
3. `secondsPerStep` of jobs 2–5 must equal job 1's within the spread. A jump to tens of
   seconds per step means shared-memory spill: rerun with `ACESTUDIO_ORT_GPU_MEM_MB=1536`
   and record both.

## 5. Idle release
1. AFTER worker. Set the profile to **Eco** (host stops the worker after 2 min).
2. Generate one image, then leave the app idle. Sample every 10 s for 3 min:
   ```powershell
   nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv -l 10 > idle.csv
   Get-Counter '\Process(python*)\Working Set - Private' -SampleInterval 10 -MaxSamples 18
   ```
3. Expected: at ~60 s the worker's VRAM drops (idle trim: `empty_cache`), at 2 min the host
   stops the worker (unchanged behaviour). Target: ≤ 500 MB worker VRAM after 2 min.
4. Worker-side unload (needs the host change in the report, or the env var): start AceStudio
   with `ACESTUDIO_IDLE_UNLOAD_SECONDS=60` and the **Balanced** profile; after ~60 s the log
   shows `Idle for 60 s: released models (image.klein, onnx)` and RAM/VRAM drop while the
   worker process stays up. The next job then pays the model load but not the PyTorch import
   or the worker start (compare `appSeconds` with a job after a host stop).

## 6. Upscale and live preview (not in the Benchmark)
Real-ESRGAN (.pth): run the app's AI upscale (task `upscale.esrgan`) on an HD image of 512×512 with `RealESRGAN_x4plus.pth`,
5 times in a row, BEFORE and AFTER. Record `worker.metrics.wallSeconds` from the job info
(AceStudio log, `job.done`). Expected after: job 1 unchanged, jobs 2–5 faster by the model
load (~1 s, estimate) plus the per-tile host syncs (estimate: 5–15 % of the tile time).
Then a 2048×2048 input with `tile` 192 to check the 1 GB GPU-canvas limit path (output
8192² fp16 = 384 MB, stays on the GPU).

SDXL live preview: an SDXL generation (task `image.generate`), 30 steps, live preview every 5 steps,
1024², 3 runs each. Compare `worker.metrics.stages.diffusion`. Expected after: lower by the
PNG encode + write time per preview (estimate 20–80 ms each, 6 per image).

## 7. Quality check (fixed seeds)
For each case PNG in `bench-before-1` and `bench-after-1`:
```powershell
& "$env:LOCALAPPDATA\AceStudio\ai\python\python.exe" -c "import sys, numpy as np; from PIL import Image; a=np.asarray(Image.open(sys.argv[1]).convert('RGBA'),np.int16); b=np.asarray(Image.open(sys.argv[2]).convert('RGBA'),np.int16); print(sys.argv[1], 'max diff', np.abs(a-b).max(), 'pixels differing', int((a!=b).any(-1).sum()))" before.png after.png
```
Expected: identical (max diff 0). The staged load casts the same modules with the same
patterns; HEURISTIC cuDNN search may change the cut-out mask by rounding at most a few edge
pixels — any visible difference goes to `pixel-art-director` before merge.

## 8. Forced OOM (correctness, optional)
Run only on an otherwise idle GPU. Occupy VRAM from a second process, then generate:
```powershell
& "$env:LOCALAPPDATA\AceStudio\ai\python\python.exe" -c "import torch, time; x=torch.empty(int(3.5*2**30), dtype=torch.uint8, device='cuda'); print('holding 3.5 GB'); time.sleep(600)"
```
Expected with the AFTER worker: log warning `The GPU ran out of memory; retrying once with
tiled VAE, sliced VAE` and either success with `info.oomRetry`, or `job.failed` with
`kind: oom` and the message "also after a retry with ...". BEFORE: immediate failure.

## 9. Record
Add a Before/After/Δ/Runs table per metric to `docs/agents/PERFORMANCE_BASELINE.md`. A change
counts as an improvement only when the median moves beyond the run-to-run spread and the
quality check passes.
