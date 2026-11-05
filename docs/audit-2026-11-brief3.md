# Audit: animation, AI, cloud readiness, security (brief of 5 November 2026)

Findings come from the code and from runs on the test PC (RTX 4070 Laptop 8 GB,
32 GB RAM). Measurements are from the two benchmark runs and the frame-by-frame
reviews of 4 November.

## 1. How AceStudio animates today

| Technique | Used | Where |
|---|---|---|
| Simple 2D warping | Yes, until 4 Nov: rotation and scale of parts with nearest-neighbour sampling | `SpriteMotion` render() |
| Whole-pixel part moves | Yes, since 4 Nov, for walk, run, sprint, idle and flap | `PixelRig` |
| Mesh deformation | No | |
| Sprite rotation | Yes: RotSprite for 8-direction packs of swimmers, birds and ships | `rotateSprite` |
| Skeletal animation | A measured implicit rig only: legs, knees, arms and wings found from the silhouette. No user bones, no weights, no IK editor | `analyzeSubject` |
| Independent AI frames | Character Pack generates whole directional views with FLUX.2 klein; no AI key poses within a motion | AI Studio |
| Optical flow, frame interpolation | No | |
| Image-to-video models | No | |
| Procedural motion | Effects: fire (generated since 4 Nov), water, sway, flag, smoke, pulse | `Effects` |

## 2. Why the animations look flat (the brief's 15 points)

1. **Layer ordering.** A sprite is one flat layer. The rig orders legs and arms
   (far leg first, arms on top), but the far wing, the far arm and the far legs
   are painted into the same pixels as the body and cannot be separated by
   moving pixels.
2. **Depth.** None: there is no depth map and no notion of front and back
   beyond draw order.
3. **Joints.** Hips, knees and shoulders are guessed from the silhouette (the
   rows where the legs split, the columns beside the legs). It works for clear
   side views; it fails for cloaks, wide armour and touching legs. Fixes made
   on 4 Nov for three-quarter stances and front views.
4. **Skeletal deformation.** Until 4 Nov, rotation with resampling redrew 29-52 %
   of leg pixels per frame (boots changed shape, outlines broke). Now: whole-pixel
   row shifts, no redraw.
5. **Hidden body.** Not reconstructed at all. A wing raised in the drawing
   hides the shoulder and back; moving it reveals nothing (there is nothing to
   reveal), so earlier flaps left holes or squashed the wing over the body.
6. **Perspective.** None. A flap seen from the side is a change of
   perspective of the wing; vertical scaling of the drawn wing (until 4 Nov)
   and row folding (since) can only approximate it, which is why it reads as
   paper folding.
7. **Trajectories.** Sine curves until 4 Nov; key poses (contact, down,
   passing, up) since, with eased swing arcs.
8. **Keyframes.** Only one drawing exists. Every frame is a transformation of
   it. This is the root cause behind 5, 6 and 15.
9. **Interpolation.** No interpolation between drawings (there is only one).
10. **Proportions.** Stable since 4 Nov (tests: torso and boots are whole-pixel
    copies in every frame).
11. **Anchors.** Feet planted since 4 Nov (per-foot ground line, no sliding).
12. **Secondary motion.** Head lag only; capes, hair, tails and weapons move
    rigidly with the body.
13. **Occlusion.** Only between rig parts the rig knows; anything else is
    painted over in draw order.
14. **Anatomical constraints.** Leg length kept within 65-135 %; knees bend
    forward, hind legs at the hock. Nothing for wings, necks or tails.
15. **Species models.** Body types: biped, quadruped, blob, flyer, swimmer,
    object. Swimmers have one body wave for all fish; flyers one flap for all.

**Conclusion.** Bending one drawing cannot produce believable wings, turns or
fire; the brief's own analysis agrees. Fire is now generated (no AI). Wings,
turns and new views need new drawings: AI key poses or image-to-video, with
pixel cleanup and consistency checks, run on a cloud GPU (section 4).

## 3. AI inference today

- One local Python worker (JSON-RPC over stdio), one job at a time:
  FLUX.2 klein 4B (FP8 + CPU offload), SDXL, BiRefNet, Real-ESRGAN.
- Measured: a pixel sprite takes 9-11 s after the first (optimised pipeline),
  with a model load of 20-34 s. The device-wide VRAM peak hits the 8 GB limit,
  and system RAM filled to 31.5 of 31.5 GB during klein's cold load.
- Image-to-video (Wan 2.2 TI2V-5B, Apache 2.0, 34.2 GB) would need 24 GB of
  VRAM in its standard path; on 8 GB only with heavy offload, minutes per clip.

## 4. Cloud readiness

- The worker protocol is already job-based (submit, progress, cancel, done
  with outputs, failures with a kind). A remote worker can speak the same
  protocol over HTTPS; the supervisor's queue, cancel and retry logic is
  reusable.
- Missing: a remote transport, authentication, upload and download of job
  files, cost estimation and approval, and the cloud-side worker image.
- Provider comparison (prices checked 5 Nov 2026; they change):

| Provider | Model | Price examples | Idle billing | Notes |
|---|---|---|---|---|
| Modal | Serverless functions, per second | L4 $0.80/h, L40S $1.95/h, A100 80 GB $2.50/h | None when scaled down | $30/month free credit on the Starter plan; Python-defined deployment; secrets store |
| RunPod Serverless | Flex workers, per second | 24 GB (L4/A5000) $0.75/h, RTX 4090 $1.34/h, L40S $1.75/h, A100 $2.72/h | 5 s idle timeout, then none | Docker image; cold starts billed |
| Vast.ai | Marketplace instances, hourly | RTX 4090 $0.34-0.50/h, A100 80 GB $0.50-0.80/h | Storage billed while the instance exists | Hosts are not audited (SOC 2 covers the platform only); encrypt data, destroy instances |
| Lambda | On-demand instances, hourly | (not serverless) | Billed while running | No scale-to-zero: risk of paying for idle time |

## 5. Security and privacy today

- **Network calls made by AceStudio:** model downloads from the catalogue (only after
  approval, hashed), pip installs of the AI runtime from PyPI and the PyTorch
  index (only when the user sets up the runtime). Nothing else.
- **Local only:** the MCP bridge (local socket with an HMAC token) and the
  single-instance check (local socket).
- **Telemetry:** none in AceStudio. The worker sets `HF_HUB_OFFLINE`,
  `HF_HUB_DISABLE_TELEMETRY`, `TRANSFORMERS_OFFLINE` and `DO_NOT_TRACK`, and
  ONNX Runtime telemetry is disabled.
- **Licensing:** core `Licence` (signed keys, 14-day trial) and the app's
  `Licensing` module gate saving and exporting at 25 call sites. To be removed
  for personal use (section 11 of the brief).
- **Credentials:** none are stored today. The MCP bridge token sits in a
  user-only file.
