---
name: cloud-deployment
description: Plan, build, test and tear down an AceStudio remote GPU inference worker securely and cost-consciously — local container test first, explicit user approval before any paid resource, authenticated HTTPS, inputs-only uploads, idle shutdown and verified teardown. Use for any Cloud Mode / remote GPU work.
---

# Cloud GPU Deployment

## 0. Approval gate (mandatory)
Before any command that creates, starts, resizes or bills a cloud resource, post in chat: provider, instance type, region, hourly price (with source + date), expected duration, max cost, and teardown plan. Proceed only on an explicit "yes" from the user. Never create accounts or enter payment details.

## 1. Design checklist
- Same `acestudio_worker` code in a container (`cloud/Dockerfile`), pinned CUDA base image digest and pinned requirements with hashes.
- Transport: HTTPS with certificate validation; per-session bearer token (random 256-bit), short TTL; no public unauthenticated port; provider firewall restricted where supported.
- Uploads: only the job's prompt/parameters and explicitly attached reference images. Never project folders.
- Results: validated (size caps, PNG/WebP header, SHA-256 echoed by worker); remote copies deleted after download.
- Lifecycle: idle shutdown (e.g. 10 min), hard max session (e.g. 2 h), health endpoint, cancellation endpoint, idempotency keys for retries.
- Credentials: provider API key in Windows Credential Manager (never in settings.json, logs, docs, or the container image).

## 2. Local rehearsal (free)
Build and run the container locally (GPU or CPU), point the client at `https://localhost` with a self-signed cert pinned for the test, and run: submit, progress, cancel, network drop, timeout, malformed result, cleanup.

## 3. Paid test (only after approval)
Run the smallest instance that fits; record start/stop timestamps, actual cost, latency and transfer sizes; verify teardown in the provider console/API (no instances, volumes or buckets left).

## 4. Record
`docs/CLOUD_ARCHITECTURE.md`: provider comparison, chosen design, security review sign-off from `cybersecurity-engineer`, measured costs.
