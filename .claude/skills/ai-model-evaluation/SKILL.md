---
name: ai-model-evaluation
description: Compare candidate AI models (diffusion, adapters, segmentation, upscalers, background removal) for an AceStudio task on quality, speed, peak VRAM, RAM and license, using fixed prompts and seeds, and write a reproducible recommendation. Use before adding, replacing or re-configuring any model.
---

# AI Model Evaluation

## 1. Define the task
Task name, input/output, target resolution, style (pixel/HD), acceptance criteria, VRAM budget (≤ 7 GB peak on the 8 GB RTX 4070 Laptop unless offload is documented).

## 2. Shortlist (max 4)
For each candidate record: exact repo/model id, revision/commit, file format (**safetensors required**), size, SHA-256, license (read the actual license text — note output-use restrictions, attribution, non-commercial clauses), date checked.

## 3. Fixed evaluation set
Store under `tests/ai_eval/<task>/`: ≥ 12 prompts covering the asset classes, 3 seeds each, reference images when the task is conditioned. Never change the set mid-comparison.

## 4. Run
- Same machine, same driver, same pinned package versions; record them (`nvidia-smi`, `pip freeze` of the worker venv).
- Warm-up 1 run, then measure ≥ 3 runs: cold load time, per-image latency (median), peak VRAM (`torch.cuda.max_memory_allocated` + `nvidia-smi` peak), RAM, CPU%.
- Only one GPU-heavy evaluation at a time; release models between candidates (`del pipe; torch.cuda.empty_cache()`).

## 5. Judge quality
Build side-by-side grids (same prompt/seed per row). Score with `pixel-art-quality` / `animation-quality` metrics where applicable plus blind review by `pixel-art-director` or `animation-director`. Note systematic failures (anatomy, hands, palette, grid).

## 6. Report → `docs/evals/<date>-<task>.md`
| Model | License | VRAM peak | Load s | s/img | Quality score | Failures |
Recommendation, rejected options with reasons, exact config (resolution, steps, sampler, CFG, adapters), and grid paths. Update `docs/agents/AI_MODEL_STRATEGY.md`.

## Rules
No paid APIs or cloud runs without user approval. No `trust_remote_code` without `cybersecurity-engineer` review. No pickle checkpoints.
