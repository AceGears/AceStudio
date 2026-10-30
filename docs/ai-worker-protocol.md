# AI worker protocol (version 1)

The AI worker is a Python process (`python -m acestudio_worker`) started and supervised by
AceStudio (`acestudio::ai::WorkerSupervisor`, used by the desktop app and `acestudio-cli ai`).
They talk over the worker's **stdin/stdout**: one JSON object per line, UTF-8, JSON-RPC 2.0
framing. **stderr** is free-form text captured into AceStudio's log; the worker redirects
file descriptor 1 to stderr at start-up, so `print()` or native libraries can never corrupt the
protocol stream.

## Host → worker requests

| Method | Params | Result |
|---|---|---|
| `hello` | `{"protocol": 1}` | `{protocol, version, python, platform, executable, pid, capabilities, tasks}` — answered immediately. `capabilities.probed` is `false`; see the `capabilities` notification. |
| `job.submit` | `{job, task, params, dir}` | `{"accepted": true}`. Errors: `-32001` busy (one job at a time), `-32002` unknown task, `-32003` task unavailable (missing packages; message says what to install), `-32602` bad params. |
| `job.cancel` | `{job}` | `{"cancelling": bool}` — cooperative; the task stops at its next check. |
| `shutdown` | `{}` | `{}`, then the worker exits. |

`dir` is a job folder owned by the host: input images are PNG files there, and the worker
writes its outputs there too. Paths are UTF-8 strings.

## Worker → host notifications

| Method | Params |
|---|---|
| `capabilities` | Full capabilities after probing PyTorch/ONNX devices: `{numpy, pillow, torch, diffusers, onnxruntime, cuda, devices: [{index, name, vramMb}], onnxProviders: [...], probed: true}` |
| `job.progress` | `{job, progress?: 0..1, message, step?, steps?}` — rate-limited to ~20/s except for meaningful steps |
| `job.done` | `{job, outputs: {name: path}, info: {...}}` |
| `job.failed` | `{job, error, kind}` — `kind`: `error`, `oom`, `missing_dependency`, `missing_model`, `invalid_input` |
| `job.cancelled` | `{job}` |
| `log` | `{level: info|warning|error, message}` |

The worker marks itself free **before** sending `job.done/failed/cancelled`, so the host may
submit the next job immediately.

## Supervision policy (host side)

- **Start-up:** `hello` must be answered within 90 s, otherwise the worker is killed and
  treated as a crash.
- **Crash during a job** (non-zero exit, killed by the OS, e.g. out of memory): the job is
  retried once on a fresh worker; a second crash fails it with the exit code and the last 40
  lines of stderr. Exit code `-9`/`137` adds "The system may have run out of memory."
- **Cancel:** queued jobs are cancelled immediately. A running job gets `job.cancel`; if it is
  not cancelled within 10 s (stuck in native code), the worker is killed and restarted, and the
  job ends Cancelled (not retried).
- **Restart storms:** after 3 crashes within 2 minutes the worker is left in the Failed state,
  queued jobs are failed with an explanation, and only an explicit start/restart tries again.
- **Lifetime:** the worker exits when its stdin closes, so it never outlives AceStudio. On
  Windows it additionally runs in a job object with KILL_ON_JOB_CLOSE.

## Tasks

| Task | Needs | Inputs → outputs |
|---|---|---|
| `model.download` | stdlib | `{files: [{url, path, size, sha256}], dest_dir, license_accepted}` → files; `info.sha256` per file. Resumes `.part` files with HTTP Range; renames only after the SHA-256 matches; refuses without `license_accepted`; rejects absolute or `..` paths. |
| `runtime.install` | pip | `{profile}` — one of the profiles in `ai_worker/requirements/` (no arbitrary package names). `info.restartRequired`. |
| `background.remove` | numpy, Pillow, onnxruntime | `{input, model (.onnx), threshold?, device?}` → `image` (RGBA), `mask` |
| `upscale.esrgan` | numpy, Pillow, torch (`.pth`) or onnxruntime (`.onnx`) | `{input, model, tile?, device?}` → `image` (alpha resized with Lanczos) |
| `image.generate` | torch, diffusers, transformers | `{model_dir, prompt, negative_prompt?, width, height, steps, guidance, seed, count, lora?, vae_dir?, init_image?, strength?, device?, offload?, pixel?}` → `image0..N` (+ `raw0..N` with `pixel`); `info.seed` |
| `diag.*` | — | Test-only tasks (echo, sleep, crash, fail, stubborn), registered only when `ACESTUDIO_WORKER_DIAG=1`. |
