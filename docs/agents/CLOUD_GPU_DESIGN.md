# Remote GPU design (proposal, nothing provisioned)

Status: **design only**. No cloud account was opened and no provider was contacted, priced or used. Building or running any of this needs the user's approval, because it costs money and opens a network service.
Owner: `cloud-gpu-engineer`. Reviewers: `cybersecurity-engineer` (has a veto), `backend-architect` (IPC), `acestudio-chief-architect`.
Inputs: audit `AUDIT_2026-11-07.md` §cloud feasibility, `ai_worker/acestudio_worker/{server,jobs,netguard}.py`, `core/src/AiWorker.cpp`.

## 1. Goal and non-goals
**Goal.** Let a user run the heavy AI jobs on a GPU they control: their own second PC, or a rented cloud GPU. Example jobs are `image.generate` and `image.klein`; FLUX.2 Klein which needs more than this laptop's 8 GB VRAM for full quality. Everything else stays local, and the app still works fully offline.

**Non-goals:**
- No AceStudio-run service.
- No accounts.
- No telemetry.
- No shared multi-tenant server.
- The remote side never receives project files, only the inputs of one job.

## 2. Why the worker cannot simply be exposed
These are the findings from the 2026-11-07 audit, checked against the code:
- **The protocol is JSON-RPC over stdio.** `server.py` reads stdin and writes stdout. There is no transport security and no authentication, because the parent process is trusted.
- **Tasks take absolute local paths.** Examples are `model_dir`, `dest_dir` and the input image paths, and `Context.path()` writes into a local `job_dir`. A remote caller could read or write any path the worker can reach.
- **Jobs run one at a time per process.** A second job gets `ERR_BUSY`.
- **`runtime.install` and `model.download` change the machine.** Remotely, they would be remote code execution or a download relay.

So the worker must never listen on a network port itself.

## 3. Design: gateway sidecar in front of an unchanged worker
```
AceStudio (host)                                   Remote GPU box (user-controlled)
┌──────────────┐  HTTPS + TLS pinning + token      ┌────────────────────────────────┐
│ AiWorker     │ ───────────────────────────────▶  │ ace-gateway (new, small)        │
│ RemoteBackend│  POST /v1/jobs (inputs inline)    │  - auth, limits, allow-list     │
│ (new)        │  GET  /v1/jobs/{id}/events (SSE)  │  - per-job sandbox dir          │
│              │  GET  /v1/jobs/{id}/outputs/{n}   │  - path rewriting               │
└──────────────┘  DELETE /v1/jobs/{id} (cancel)    │  - spawns worker over stdio ──▶ │ acestudio_worker (unchanged)
                                                   └────────────────────────────────┘
```

### 3.1 Gateway (`cloud/gateway/`, Python stdlib + one ASGI server)
- **Transport:**
  - HTTPS only, with TLS 1.3 preferred and TLS 1.2 as the minimum. The gateway creates a self-signed certificate at first start.
  - The host pins its SHA-256 fingerprint, shown once as a pairing code. Optionally, a reverse proxy can supply a real certificate.
  - Plain HTTP is never accepted, even on a LAN.
- **Authentication:**
  - One 256-bit random bearer token per paired host, created by `ace-gateway pair`.
  - The token is stored hashed (SHA-256) on the gateway and in the Windows Credential Manager on the host. It is never written to settings files, logs or docs.
  - The token is compared in constant time, can be revoked one at a time, and failed attempts are rate-limited per IP.
- **Allow-list:**
  - Only inference tasks are allowed: `image.generate`, `image.klein`, `background.remove` and `upscale.esrgan` (the task names in `ai_worker/acestudio_worker/tasks/`).
  - `runtime.install`, `model.download` and any task not on the list are refused with 403.
  - Models are installed on the box by its owner with the normal CLI. The gateway reports which models exist (`GET /v1/models`) and lists only catalogue IDs, never paths.
- **Inputs only, no paths:**
  - A request carries job parameters plus input images as base64 PNG. Each image is limited to 16384² pixels and 64 MB, and each request to 256 MB in total.
  - The gateway writes the inputs into a fresh `jobs/<random-id>/in/`. It rewrites every path parameter to point inside that directory, or to the box's own model folder chosen by catalogue ID, and rejects any client-supplied path.
  - Images are decoded and re-encoded through Pillow with size limits before the worker sees them, so a malformed image is caught in the gateway, not in the worker.
- **Isolation:**
  - The worker runs as an unprivileged user in a container (read-only image, model folder mounted read-only, only `jobs/` writable, `--network none` for the worker container).
  - `ACESTUDIO_OFFLINE=1` is set as well, so `netguard` blocks outbound connections even if the container rule were wrong.
- **Lifecycle:**
  - One worker per GPU and one job at a time. Further jobs queue with a visible position, up to 8 per token.
  - Cancel maps to the worker's existing cancel request.
  - Job folders are deleted when the outputs are fetched, or after 1 hour, whichever comes first. Nothing about the job content is logged beyond its ID, task name, duration and status.
- **Idle cost control:** after N minutes with no jobs (default 15), the gateway can run a configurable shutdown hook, for example to stop the instance. Paid instances should never idle silently.

### 3.2 Host side (`core/src/AiWorker.cpp` + new `RemoteBackend`)
- **Backend interface.** `AiWorker` gains a backend interface with two implementations:
  - `LocalProcess`: today's behaviour.
  - `Remote`: speaks the HTTP API above and turns server-sent events back into the same progress and preview signals the UI already uses.
- **Settings › Generation › Run AI on.** The choices are "This PC" (the default) or "Remote GPU". The remote option asks for the address and pairing code, and offers a "Test connection" button that shows the GPU name, VRAM and installed models.
- **Network access.** "Disable all network access" still wins: with it on, the remote backend refuses to connect, and the UI says why.
- **Per-job fallback.** If the remote box is unreachable, the job fails with a clear error and an offer to run it locally. Jobs never silently move to the local PC or away from it.
- **Privacy.** The privacy page lists the remote address as a connection AceStudio makes, and says what is sent: the prompt, settings and input images of that job only.

## 4. Threat model summary
| Threat | Mitigation |
|---|---|
| Someone on the internet finds the port | TLS + bearer token, rate-limited auth, no unauthenticated endpoints (not even `/health` reveals versions) |
| Token theft from the host | Credential Manager storage, per-host revocation, token never in logs or settings files |
| MITM / fake gateway | certificate fingerprint pinned at pairing |
| Path traversal / arbitrary file read | client paths rejected; gateway rewrites to per-job sandbox; container mounts |
| Malicious image input | gateway re-encodes with limits; worker keeps its own limits |
| Remote code execution via install/download tasks | not on the allow-list; worker container has no network |
| Cost overrun on a rented GPU | idle shutdown hook; host shows "remote GPU is running" while connected; nothing is created automatically |
| Leaking prompts or images | job folders deleted after fetch or 1 h; no content logging |

## 5. Work packages (each needs approval before any paid step)
1. **Gateway prototype, local only.** Run it on this PC against the local worker, connecting to 127.0.0.1 with a self-signed certificate. Testing: unit tests for auth, allow-list, path rewriting and limits, plus an end-to-end test with a tiny `upscale.esrgan` job. No cost.
2. **Host `RemoteBackend`.** Add the backend plus the Settings UI. CI builds it, and the end-to-end test runs against the local gateway. No cost.
3. **Container image.** A Dockerfile (CUDA runtime base, pinned digests, `pip --require-hashes`), built locally or in CI. Pushing to a registry requires approval.
4. **Remote trial.** On the user's own second PC (free), or on a rented GPU **only after the user approves a provider and a spending limit**.

## 6. Open questions for the user
- Is the first target the user's own second PC on the LAN (no cost), or a rented cloud GPU? If cloud, which provider and what monthly cap?
- Is a self-signed certificate with fingerprint pairing acceptable, or is a real domain and certificate wanted?
