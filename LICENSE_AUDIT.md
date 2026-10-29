# License Audit

Distribution model: commercial, closed-source Windows application. Qt used under LGPLv3 with
dynamic linking. Last reviewed 2026-10-29. Not legal advice — have counsel review before the
first paid release.

## Software bill of materials — shipped components

| Component | Version | License | Linkage | Obligations | Status |
|---|---|---|---|---|---|
| Qt (Core, Gui, Widgets) | 6.12 LTS target (6.4 used for local compile checks) | LGPLv3 | Dynamic | Ship license text; allow replacing Qt DLLs; offer Qt source or point to it; no static linking; note modifications (none) | ✅ Compatible |
| nlohmann/json | 3.12.0 | MIT | Header-only | Include copyright + license text | ✅ |
| miniz | 3.1.2 | MIT | Static | Include copyright + license text | ✅ |
| stb_image | commit 003501f | MIT / Public Domain (dual) | Compiled in | None required (MIT notice included anyway) | ✅ |
| libwebp (libwebp, libwebpdemux, libwebpmux) | 1.6.0 | BSD-3-Clause | Static | Include copyright notice, licence text and the PATENTS grant file in the notices; do not use WebM project names to endorse the product | ✅ |

## Build/test-only components (not shipped)

| Component | Version | License | Notes |
|---|---|---|---|
| Catch2 | 3.16.1 | BSL-1.0 | Tests only |
| CMake | ≥ 3.25 | BSD-3 | Build tool |
| jurplel/install-qt-action, ilammy/msvc-dev-cmd | v4 / v1 | MIT | CI only |

## Qt modules that must never be linked (GPL-only or commercial-only in open-source Qt)

Graphs, Charts, Data Visualization, Virtual Keyboard, Quick 3D (+Physics), Lottie, Quick
Timeline, Canvas Painter, HTTP Server, Network Authorization, Insight Tracker, VNC Server.
Enforced by `cmake/QtLicenseGuard.cmake`. The Shader Tools `qsb` CLI (GPLv3) may run at build
time only and must not ship.

## Planned dependencies (pre-cleared, not yet integrated)

| Component | License | Phase |
|---|---|---|
| Lua 5.4.9 | MIT | 7 |
| sol2 3.3.0 | MIT | 7 |
| ONNX Runtime | MIT | 4 |
| PyTorch | BSD-3 | 4 |
| Hugging Face Diffusers | Apache-2.0 | 4 |
| Inno Setup 7.x | Free for commercial use (keep notices) | 8 |
| Qt Advanced Docking System | LGPL-2.1 (dynamic link) | optional |

GIF is written by AceStudio's own encoder (LZW; the LZW patents expired in 2004) and read by
stb_image, so giflib is not needed.

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

## Generated content

AceStudio will not claim generated assets are copyrightable, exclusive or free of third-party
rights. Model-specific terms (e.g. OpenRAIL use restrictions) will be shown in the Model Manager
and the EULA.

## Open items

1. Read Black Forest Labs "Self-Hosted Commercial License Terms" for FLUX.2-klein-4B.
2. Verify text-encoder sub-licenses inside FLUX.2-klein and Z-Image repositories.
3. Draft EULA incorporating OpenRAIL Attachment A use restrictions.
4. Add `licenses/` folder with full third-party license texts to the installer (Phase 8).
5. Fonts and icons: none bundled yet; any added must be audited here first.
