# License Audit

Distribution model: commercial, closed-source Windows application. Qt used under LGPLv3 with
dynamic linking. Last reviewed 2026-11-01 (0.9.0). Not legal advice — have counsel review before the
first paid release.

## Software bill of materials — shipped components

| Component | Version | License | Linkage | Obligations | Status |
|---|---|---|---|---|---|
| Qt (Core, Gui, Widgets) | 6.12 LTS target (6.4 used for local compile checks) | LGPLv3 | Dynamic | Ship license text; allow replacing Qt DLLs; offer Qt source or point to it; no static linking; note modifications (none) | ✅ Compatible |
| nlohmann/json | 3.12.0 | MIT | Header-only | Include copyright + license text | ✅ |
| miniz | 3.1.2 | MIT | Static | Include copyright + license text | ✅ |
| stb_image | commit 003501f | MIT / Public Domain (dual) | Compiled in | None required (MIT notice included anyway) | ✅ |
| Lua | 5.4.9 (github.com/lua/lua tag v5.4.9) | MIT | Static, compiled as C++ | Include copyright + licence text (Lua.org, PUC-Rio) | ✅ |
| sol2 | 3.3.0 (github.com/ThePhD/sol2 tag v3.3.0) | MIT | Header-only | Include copyright + licence text | ✅ |
| libwebp (libwebp, libwebpdemux, libwebpmux) | 1.6.0 | BSD-3-Clause | Static | Include copyright notice, licence text and the PATENTS grant file in the notices; do not use WebM project names to endorse the product | ✅ |
| Monocypher | 4.0.3 | BSD-2-Clause or CC0-1.0 (dual; we use BSD-2-Clause) | Static (licence-key signature check) | Include copyright + licence text | ✅ |
| Python (embeddable package) | 3.12.10, SHA-256 checked against python.org's release manifest in CI | PSF License Agreement (+ the licences of its bundled parts, all in its LICENSE.txt) | Separate program in `python\`, unmodified | Keep `python\LICENSE.txt`; state that it is included (Third-Party Notices) | ✅ |
| pip (wheel) | 26.2.2, SHA-256 pinned | MIT | Separate program, unpacked into the user's private AI environment on first AI setup | Keep licence text (`python\pip-LICENSE.txt`) | ✅ |
| Microsoft Visual C++ runtime (msvcp140, vcruntime140, concrt140) | from the VS 2022 redist folder | Visual Studio distributable code terms | App-local DLLs | Redistribute only the listed files, unmodified, with our application | ✅ |

| AceStudio AI worker (`ai_worker/`, Python source) | 0.5.0 | Proprietary (ours) | Shipped as source next to the executable | Includes an independent implementation of the RRDBNet architecture compatible with the published Real-ESRGAN (BSD-3) weights | ✅ |

## Installed on demand by the user (not shipped, not redistributed)

AI Studio › Set Up AI Runtime installs these with pip into a private environment on the
user's machine, after an explicit confirmation. AceStudio does not redistribute them.

| Package | Pinned version | License | Notes |
|---|---|---|---|
| numpy | 2.5.4 | BSD-3-Clause | |
| Pillow | 13.0.0 | MIT-CMU (HPND) | |
| onnxruntime / onnxruntime-directml | 1.31.1 / 1.24.4 | MIT | Usage telemetry is disabled by the worker (`disable_telemetry_events`) |
| torch | 2.15.0 | BSD-3-Clause | CUDA builds pull NVIDIA runtime wheels under the NVIDIA software licence, downloaded by the user from the PyTorch index |
| diffusers, transformers, accelerate, safetensors, peft | 0.42.1, 5.22.0, 1.15.0, 0.8.0, 0.21.2 | Apache-2.0 | |

Models are listed in `ai_worker/catalog/models.json` with their licence, obligations and
source; the user must accept the licence in the app (or with `--accept-license`) before a
download starts. The catalogue only contains models whose licences allow commercial use.

## Build/test-only components (not shipped)

| Component | Version | License | Notes |
|---|---|---|---|
| Catch2 | 3.16.1 | BSL-1.0 | Tests only |
| @modelcontextprotocol/conformance, @modelcontextprotocol/sdk (npm) | 0.2.0-alpha.12, 1.32.2 | MIT | MCP tests only (`tests/mcp`); installed by CI, not shipped |
| Claude Code, Codex CLIs (npm) | 2.1.316, 0.166.0 | Anthropic commercial terms; Apache-2.0 | Run once to check the client setups; not used by the build, not shipped |
| com.unity.2d.tilemap.extras | 9.0.1 | Unity Companion License | Added to the throwaway Unity check project only. AceStudio's Rule Tile `.asset` files reference its script GUID but contain none of its code |
| mcp (Python SDK, PyPI) | 2.3.0 | MIT | MCP tests only (`tests/mcp`); installed by CI, not shipped |
| Godot, Unity | 4.6.2, 6000.6.7f1 | MIT; Unity's own licence (the user's install) | Engine import checks only (`tests/engine`), never redistributed |
| CMake | ≥ 3.25 | BSD-3 | Build tool |
| Inno Setup | 6.x (the runner's, or Chocolatey's) | Inno Setup licence (free for commercial use; keep its copyright notice in the installer, which it adds itself) | Builds the installer; its setup stub runs on users' machines, as its licence allows |
| clang / libFuzzer, AddressSanitizer, UBSan | LLVM 18 | Apache-2.0 with LLVM exception | Fuzzing builds only (`tests/fuzz`) |
| Pillow | 12.3 | MIT-CMU | Wrote the reference GIFs and their decoded frames in `tests/data/gif` |
| AceStudio 0.1.0 / 0.5.0 / 0.8.1 | — | Ours | Built from git history to write `tests/data/compat` |
| jurplel/install-qt-action, ilammy/msvc-dev-cmd | v4 / v1 | MIT | CI only |

## Qt modules that must never be linked (GPL-only or commercial-only in open-source Qt)

Graphs, Charts, Data Visualization, Virtual Keyboard, Quick 3D (+Physics), Lottie, Quick
Timeline, Canvas Painter, HTTP Server, Network Authorization, Insight Tracker, VNC Server.
Enforced by `cmake/QtLicenseGuard.cmake`. The Shader Tools `qsb` CLI (GPLv3) may run at build
time only and must not ship.

## Planned dependencies (pre-cleared, not yet integrated)

| Component | License | Phase |
|---|---|---|
| Qt Advanced Docking System | LGPL-2.1 (dynamic link) | optional |

GIF is written and read by AceStudio's own encoder and decoder (LZW; the LZW patents expired in
2004), so giflib is not needed. stb_image is compiled for PNG only (0.9.0): fuzzing found a
use-after-free in its GIF loader, so AceStudio no longer uses it for GIF (SECURITY.md).

Rejected: WiX binary releases (Open Source Maintenance Fee EULA above US$10k revenue) unless
built from source or the fee is budgeted.

## AI models

See `docs/research/ai-models.md` for the full survey with sources. Summary:

- **Default set (commercial-compatible, no revenue cap):** SDXL base 1.0 (OpenRAIL++-M — use
  restrictions must be passed to users in the EULA), nerijs/pixel-art-xl (OpenRAIL-M),
  FLUX.2-klein-4B (Apache-2.0; BFL self-hosted terms still to be read), BiRefNet (MIT),
  SAM 2.1 (Apache-2.0), Real-ESRGAN (BSD-3), xinsir ControlNet OpenPose SDXL (Apache-2.0),
  DWPose ONNX (Apache-2.0), h94/IP-Adapter (Apache-2.0) + LAION OpenCLIP (MIT).
- **Excluded:** FLUX.1-dev and derivatives, FLUX.2-klein-9B, BRIA RMBG-1.4/2.0, 4x-UltraSharp,
  CMU OpenPose and lllyasviel/Annotators openpose weights, thibaud OpenPose ControlNet,
  IP-Adapter-FaceID, InstantID/InsightFace.
- **Conditional:** SD 3.5 (Stability Community License; commercial only under US$1M revenue
  and requires "Powered by Stability AI").
- Models are never bundled in the base installer and never downloaded without user approval.

### Models in the shipped catalog (`ai_worker/catalog/models.json`)

Every file is pinned to a Hugging Face / GitHub commit with a SHA-256; the Models dialog shows
the licence, source and obligations and requires the user to tick "I have read the licence and
accept its terms" before each download.

| Catalog id | Model | Licence | Pinned commit | Obligations |
|---|---|---|---|---|
| birefnet-lite / birefnet-general | ZhengPeng7 BiRefNet (ONNX) | MIT | release asset | keep notice |
| realesrgan-x4plus / -anime | xinntao Real-ESRGAN | BSD-3-Clause | release asset | keep notice |
| sdxl-base-1.0 | stabilityai SDXL base 1.0 | CreativeML OpenRAIL++-M | 462165984030 | pass Attachment A use restrictions to users |
| pixel-art-xl | nerijs/pixel-art-xl LoRA | CreativeML OpenRAIL-M | 8bf4a4d9ea28 | pass use restrictions to users |
| taesdxl | madebyollin/taesdxl (live preview decoder) | MIT | b20258aaef75 | keep notice |
| controlnet-openpose-sdxl | xinsir/controlnet-openpose-sdxl-1.0 | Apache-2.0 | 23f966cd5cfd | keep licence + NOTICE; state changes |
| ip-adapter-plus-sdxl | h94/IP-Adapter (ip-adapter-plus_sdxl_vit-h) + OpenCLIP ViT-H/14 encoder | Apache-2.0 (encoder weights: LAION OpenCLIP, MIT) | 018e402774ae | keep licence + NOTICE |

Phase 5 deliberately uses AceStudio's own pose skeleton editor and procedural cycles instead
of an OpenPose/DWPose *detector*, so no CMU OpenPose weights (non-commercial) are needed.

## Generated content

Phase 6 (0.7.0) adds no third-party code, assets or models: tilesets, templates, UI kits,
atlases and parallax layers are generated by AceStudio's own code from the user's images or
from the already-audited SDXL models.

Engine export writes three C# scripts into Unity projects (`AceStudioAutoTile.cs`,
`AceStudioWangSet.cs`, `AceStudioDualGrid.cs`, from `core/unity/`). They are AceStudio's own
code, contain no third-party code and use only Unity's built-in Tilemap module. Each file's
header lets users change and ship it with their games; the EULA must say the same.

AceStudio will not claim generated assets are copyrightable, exclusive or free of third-party
rights. Model-specific terms (e.g. OpenRAIL use restrictions) will be shown in the Model Manager
and the EULA.

## Open items

1. Read Black Forest Labs "Self-Hosted Commercial License Terms" for FLUX.2-klein-4B.
2. Verify text-encoder sub-licenses inside FLUX.2-klein and Z-Image repositories.
3. Draft EULA incorporating OpenRAIL Attachment A use restrictions.
4. ~~Add a licences folder with the full third-party texts to the installer~~ — done in 0.9.0
   (`packaging/licences`, installed to `{app}\licences`).
6. Have the EULA draft (`packaging/windows/EULA.txt`) and the privacy policy draft
   (`PRIVACY.md`) reviewed by counsel; fill in the publisher's legal name and address.
5. Fonts and icons: none bundled yet; any added must be audited here first.
