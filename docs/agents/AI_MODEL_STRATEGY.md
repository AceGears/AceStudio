# AI Model Strategy

Owner: `ai-research-engineer`. This is the current state as observed; no evaluations have been run yet.

| Task | Model in use (0.10.0) | Notes |
|---|---|---|
| Image generation | FLUX.2 Klein 4B (`flux2-klein-4b`, 15 GB on disk), 4 steps, fp8 storage, model offload; `pixel-art-klein-lora` | Saturates 8 GB VRAM and 31.5 GB RAM on cold load |
| Image generation (0.8.1 path) | SDXL, fp16, 30 steps, optional ControlNet/IP-Adapter/LoRA, TAESD previews | No fp16-fix VAE |
| Background removal | BiRefNet (ONNX; CUDA → DirectML → CPU) | Session rebuild cost 7–10 s per job unless cached |
| Upscale | Real-ESRGAN (torch fp16 or ONNX, 192 px tiles) | Model reloaded every job |
| Pose / animation keyframes | OpenPose maps produced; no rendered frames observed | Pipeline incomplete |

## Next evaluations (`ai-model-evaluation` skill)
1. Part segmentation for rigging (SAM-family vs BiRefNet-derived masks) on the dragon, fish and knight sprites.
2. Inpainting of occluded part regions within 7 GB.
3. Pose- or reference-conditioned keyframes versus rig-driven animation, for quality and cost.
4. Verify the licences of FLUX.2 Klein 4B and its LoRA for this use.
