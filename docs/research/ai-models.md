# AI model licensing survey

Checked 2026-10-29 against Hugging Face model cards, LICENSE files and GitHub repos.
VRAM figures marked "est." are estimates from parameter counts (FP16 ≈ 2 bytes/param),
not measurements. Nothing here is legal advice; counsel must review the OpenRAIL use
restrictions and Black Forest Labs terms before shipping.

Legend: OUT = commercial use of outputs; PROD = use inside a paid product;
REDIST = may ship weights in the installer.

## Recommended default set (permissive, no revenue cap)

| Role | Model | License | Notes |
|---|---|---|---|
| Text-to-image (primary) | SDXL base 1.0 | CreativeML OpenRAIL++-M | Most mature on 12 GB; ControlNet + IP-Adapter ecosystem. Attachment A use restrictions must be passed to users (EULA). est. 7–9 GB @1024px |
| Pixel-art style | nerijs/pixel-art-xl (SDXL LoRA) | CreativeML OpenRAIL-M | Generate at 8× target size, nearest-neighbour downscale |
| Text-to-image (modern option) | FLUX.2 [klein] 4B | Apache-2.0 | Card: ~13 GB VRAM → FP8 or offload on 12 GB. Diffusers support may require main branch (UNVERIFIED in a stable release). BFL self-hosted terms not yet read |
| Pixel-art LoRA for klein | Limbicnation/pixel-art-lora | Apache-2.0 | Author claims CC0 training data |
| Alternative base | Z-Image-Turbo (6B) + tarn59 pixel LoRA | Apache-2.0 | Needs offload/quantization on 12 GB |
| Background removal | BiRefNet / BiRefNet_lite | MIT | ONNX exports available; run in ONNX Runtime |
| Click-to-segment | SAM 2.1 | Apache-2.0 | Optional cc_torch component license not checked |
| HD upscaling | Real-ESRGAN x4plus / x4plus_anime_6B | BSD-3-Clause | Keep copyright notice. Pixel art uses integer nearest-neighbour instead (no model) |
| Small upscaler | Swin2SR classical x4 | Apache-2.0 | |
| Pose ControlNet | xinsir/controlnet-openpose-sdxl-1.0 | Apache-2.0 | |
| Pose detection | DWPose ONNX (yzd-v/DWPose) | Apache-2.0 | Only needed to extract pose from images; editor can author skeletons directly |
| Reference/identity | h94/IP-Adapter (plus_sdxl_vit-h) + LAION OpenCLIP ViT-H-14 | Apache-2.0 / MIT | |

## Excluded (non-commercial or unclear)

| Model | Reason |
|---|---|
| FLUX.1 [dev] and LoRAs built on it | FLUX.1-dev Non-Commercial License |
| FLUX.2 [klein] 9B | FLUX Non-Commercial License |
| BRIA RMBG-1.4 / RMBG-2.0 | Non-commercial / CC BY-NC 4.0 without paid BRIA agreement |
| 4x-UltraSharp | CC BY-NC-SA 4.0 |
| CMU OpenPose; lllyasviel/Annotators openpose weights | Academic/non-commercial; Annotators weights unlicensed ("other") |
| thibaud/controlnet-openpose-sdxl-1.0 | Inherits OpenPose terms |
| IP-Adapter-FaceID, InstantID, InsightFace-based tools | Research-only / non-commercial dependencies |
| ai-forever/Real-ESRGAN mirror | No license stated (UNVERIFIED) |
| LoRAs tagged "other" without license text | Unknown terms, possible IP risk |

## Conditional

| Model | Condition |
|---|---|
| SD 3.5 Medium / Large | Stability AI Community License: commercial only under US$1M annual revenue; requires NOTICE and visible "Powered by Stability AI". Large (~16 GB) does not fit 12 GB unquantized |
| Qwen-Image (20B) | Apache-2.0 but ~41 GB est.; impractical locally |

## Runtimes

- ONNX Runtime — MIT
- Hugging Face Diffusers — Apache-2.0. Supports SDXL, SD3.5, FLUX.1; FLUX.2-klein and Z-Image have pipelines (stable-release status partly UNVERIFIED).
- BiRefNet, SAM 2, Real-ESRGAN, DWPose run outside Diffusers (ONNX or their own code).

## Open items

- Read BFL "Self-Hosted Commercial License Terms" and Usage Policy for FLUX.2-klein-4B.
- Verify text-encoder sub-licenses inside FLUX.2-klein and Z-Image repos.
- Verify IP-Adapter availability/licensing for FLUX.2-klein and Z-Image.
- FLUX and SD3.5 repos are gated on Hugging Face: runtime download needs a user token, or mirror the files (permitted by Apache/OpenRAIL).

## Sources

- https://huggingface.co/stabilityai/stable-diffusion-xl-base-1.0/blob/main/LICENSE.md
- https://huggingface.co/black-forest-labs/FLUX.2-klein-4B
- https://huggingface.co/black-forest-labs/FLUX.1-schnell
- https://huggingface.co/black-forest-labs/FLUX.1-dev/blob/main/LICENSE.md
- https://huggingface.co/Tongyi-MAI/Z-Image-Turbo
- https://huggingface.co/stabilityai/stable-diffusion-3.5-medium/blob/main/LICENSE.md
- https://huggingface.co/nerijs/pixel-art-xl
- https://huggingface.co/Limbicnation/pixel-art-lora
- https://huggingface.co/tarn59/pixel_art_style_lora_z_image_turbo
- https://huggingface.co/ZhengPeng7/BiRefNet
- https://huggingface.co/facebook/sam2.1-hiera-large
- https://huggingface.co/briaai/RMBG-1.4
- https://huggingface.co/briaai/RMBG-2.0
- https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE
- https://huggingface.co/caidas/swin2SR-classical-sr-x4-64
- https://openmodeldb.info/models/4x-UltraSharp
- https://huggingface.co/xinsir/controlnet-openpose-sdxl-1.0
- https://huggingface.co/yzd-v/DWPose
- https://github.com/CMU-Perceptual-Computing-Lab/openpose/blob/master/LICENSE
- https://huggingface.co/h94/IP-Adapter
- https://huggingface.co/h94/IP-Adapter-FaceID
- https://github.com/microsoft/onnxruntime/blob/main/LICENSE
- https://github.com/huggingface/diffusers/blob/main/LICENSE
