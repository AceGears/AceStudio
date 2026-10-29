# Technology stack research

Checked 2026-10-29 against official sources. Items marked UNVERIFIED could not be confirmed.

## Qt 6

- **Current LTS: Qt 6.12** (released 2026-09-30, 5-year maintenance). 6.8 LTS patch releases after its open-source window are commercial-only, so open-source users should track the newest LTS and move forward on schedule.
- LGPLv3 (usable with dynamic linking): Widgets, Quick and other Essentials, WebSockets, Image Formats, Shader Tools *runtime*.
- **GPLv3-only — must not be linked:** Qt Graphs, Qt Charts, Qt Virtual Keyboard, Qt Quick 3D, Qt Lottie, Qt Quick Timeline, Qt Canvas Painter, Qt HTTP Server, Qt Network Authorization. Commercial-only: Insight Tracker, VNC Server.
- Qt Shader Tools `qsb` CLI is GPLv3: build-time only, never shipped.
- Qt Advanced Docking System: LGPL-2.1, Qt 6 supported, v5.1.1.
- Enforcement: `cmake/QtLicenseGuard.cmake` fails configuration if a GPL-only module is linked.
- Sources: https://qt.io/blog/qt-6.12-released, https://doc.qt.io/qt-6.12/qt-releases.html, https://doc.qt.io/qt-6/qtmodules.html, https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System

## Model Context Protocol

- Current spec **2026-07-28**: stateless requests; version/capabilities in `_meta`; `server/discover` RPC; no `initialize` handshake. 2025-11-25 and earlier use the handshake; the spec defines fallback.
- Transports: stdio (newline-delimited JSON-RPC) and Streamable HTTP (single endpoint, POST, JSON or SSE replies, `MCP-Protocol-Version` header).
- Official SDKs: TypeScript, Python, C#, Go, Rust, Ruby (tier 1); Java; Swift, PHP, Kotlin. **No official C++ SDK.**
- Python `mcp` 2.3.0 (2026-10-02) and TypeScript `@modelcontextprotocol/server` 2.4.0 implement 2026-07-28.
- Client configuration:
  - Claude Code: `claude mcp add`; project `.mcp.json`, user `~/.claude.json`; key `mcpServers`.
  - Cursor: `.cursor/mcp.json` or `~/.cursor/mcp.json`; key `mcpServers`.
  - VS Code: `.vscode/mcp.json` or user `mcp.json`; key **`servers`**.
  - OpenAI Codex: `~/.codex/config.toml` or `.codex/config.toml`; table `[mcp_servers.<name>]`.
- Sources: https://modelcontextprotocol.io/specification/versioning, https://modelcontextprotocol.io/docs/sdk, https://pypi.org/project/mcp/, https://code.claude.com/docs/en/mcp, https://cursor.com/docs/context/mcp, https://code.visualstudio.com/docs/agents/reference/mcp-configuration

## Lua

- Lua (MIT). 5.4.9 is the final 5.4; 5.5.1 current. sol2 v3.3.0 (MIT) — Lua 5.5 support UNVERIFIED.
- Decision: Lua 5.4.9 + sol2 3.3.0.

## Windows packaging

- MSIX: mandatory signing, containerised file/registry writes, differential updates.
- WiX 7.0.0: source MS-RL, but **binary releases carry the Open Source Maintenance Fee EULA** (payment required above US$10k revenue).
- Inno Setup 7.1.0: free for commercial use (keep notices; commercial license is requested, not required).
- Decision: signed Inno Setup EXE first; MSIX later for Store/enterprise.

## Libraries

| Library | License | Use |
|---|---|---|
| nlohmann/json 3.12.0 | MIT | JSON |
| miniz 3.1.2 | MIT | ZIP container, deflate, PNG write |
| stb_image | MIT / public domain | PNG decode |
| libwebp | BSD-3 | WebP / animated WebP (Phase 3) |
| giflib 6.1.3 | MIT-style | GIF (Phase 3) |
| Catch2 3.16.1 | BSL-1.0 | Tests only |
| zlib 1.3.2 | zlib | if needed |

libpng 1.6.59 has no APNG support; APNG will be written directly (acTL/fcTL/fdAT chunks).

## Engine export formats

- Godot 4.7 stable: text resources `[gd_resource type="SpriteFrames" format=3 ...]`; `load_steps` deprecated since 4.6. SpriteFrames `animations` = array of {name, speed, loop, frames[{texture, duration}]}. TileSet per-tile key format UNVERIFIED — confirm by inspecting a file saved by Godot.
- Aseprite JSON (hash/array) is read by Phaser's `load.aseprite`; it is the broadest interchange target.
- Unity: editor script using `ISpriteEditorDataProvider` (requires `com.unity.2d.sprite`).
- GameMaker: no official `.yy` schema → export marked experimental.
- Priority: Aseprite JSON → Godot → Unity importer → GameMaker (experimental).

## RotSprite

Public description (Xenowhirl): 8× upscale with similarity-aware Scale2x, rotate + downscale with nearest-neighbour, optional single-pixel detail restoration; introduces no new colours. Implement from the description only.
Source: https://en.wikipedia.org/wiki/Pixel-art_scaling_algorithms
