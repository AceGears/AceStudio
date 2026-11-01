# Security

Phase 8 security review of AceStudio 0.9.0 (2026-11-01): what an attacker can reach, how
each part defends itself, what the review found and fixed, and what remains.

## Reporting a vulnerability

Email [security contact to be set by AceGears] with the version, steps to reproduce and, if
possible, a sample file. Please do not open a public issue for a vulnerability.

## Threat model

AceStudio is a desktop app. The user is trusted; **files and network data are not**. An
attacker may hand the user a crafted project, image, palette, Lua script, AI model or
licence key, or may run code as *another* user on the same computer, or may host a web
page the user visits.

| Surface | Who can reach it | Defence |
|---|---|---|
| Project files (`.acestudio`, a ZIP) | anyone who sends a file | ZIP entry and total size limits (512 MB / 4 GB uncompressed), schema validation, image and frame limits; fuzzed |
| Images: PNG, GIF, APNG, WebP | anyone who sends a file | dimension limits (16384²), decoded-size budget for animations (1 GB, frames ≤ 10000); fuzzed |
| Palettes (`.gpl`, `.pal`, `.hex`, `.txt`, `.ase`, `.act`) | anyone who sends a file | bounds-checked parser; fuzzed |
| Lua scripts | anyone who sends a script the user runs | sandbox: no io/os/package/debug/native modules, text-only `load`, files only inside allowed folders, `require` only of `.lua` files there, memory limit covering images and documents, time limit and cancel that `pcall` cannot swallow; fuzzed |
| MCP server, stdio | the AI client the user configured | file tools limited to `--root` folders; every request validated; malformed input returns JSON-RPC errors; fuzzed |
| MCP server, HTTP (`--http`) | processes on this computer; web pages via DNS rebinding | loopback only; Host and Origin must be localhost (DNS rebinding); optional bearer token compared in constant time; header 64 KB and body 64 MB limits; strict Content-Length and chunk parsing |
| App bridge (named pipe / local socket) | processes of the **same user** | the pipe is created for the current user only (`UserAccessOption`); off with Scripts › Allow AI Assistants |
| MCP task files (`<data>/mcp-tasks`) | the same user | random 128-bit task ids; ids are validated before they become file names |
| AI model downloads | network attacker, compromised host | HTTPS; every file's SHA-256 pinned in the catalogue; manifest paths checked for `..` and absolute paths; models load with `use_safetensors=True` / `torch.load(weights_only=True)`, never pickle |
| AI Python packages (pip) | compromised PyPI package | versions pinned in `ai_worker/requirements`, installed into AceStudio's own environment; hashes are **not** pinned (see residual risks) |
| Licence keys | anyone | Ed25519 signatures (Monocypher, RFC 8032 vectors in the tests); only the public key ships; parser fuzzed |
| Bundled Python | the installer | python.org's own SHA-256 checked at build time; pip wheel hash pinned |

No telemetry is sent. The app makes network connections only for model and package
downloads the user starts.

## Review results

Fuzzing: libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer (clang 18) on nine
targets (`tests/fuzz`): project files, PNG, GIF, APNG, WebP, palettes, licence keys, Lua
scripts and MCP messages. Each target ran 10 minutes from seed files; the targets that found
problems ran 10 more minutes after the fix.

Found and fixed:

1. **MCP: malformed parameters ended the server** (fuzzing). A request whose `params` was
   not an object made `subscriptions/listen` and other handlers throw; an uncaught exception
   terminates the process. `McpServer::handle()` and `listenRequest()` now turn any parsing
   exception into a JSON-RPC error.
2. **MCP HTTP: a bad `Content-Length` ended the server** (review). A non-numeric value made
   `std::stoll` throw; a negative one was accepted. Now digits only, with 400 for anything
   else, 413 above 64 MB, 431 for headers above 64 KB, and chunk sizes checked against the
   same limit (no integer overflow).
3. **GIF import could exhaust memory** (fuzzing). A 600-byte GIF declared a large screen and
   many frames; stb_image keeps all frames at screen size and asked for over 2 GB. The GIF's
   block structure is now read first and the import refused above 1 GB of decoded pixels or
   10000 frames. The same budget now applies to WebP and APNG.
4. **Bearer token compared with `!=`** (review): now constant-time.

Third-party notes: UBSan reports "applying zero offset to null pointer" inside miniz's
inflate (`miniz.c:474`, `miniz_tinfl.c:202`) on APNG input. This is pointer arithmetic on a
null pointer with offset 0, not a memory access; it has no effect on x86-64 or ARM64 and is
left in the upstream code.

## Residual risks

- **Licence enforcement is client-side.** Keys cannot be forged without AceGears' secret key,
  but the 14-day trial start is kept in the user's data folder and the registry; someone who
  deletes both gets a new trial. A patched binary can skip the check. The source repository
  must stay private, or anyone can build without the check.
- **pip packages are pinned by version, not hash.** A compromised release of a pinned
  version on PyPI would be installed. Moving to `--require-hashes` lock files is planned.
- **The installer and executables are not code-signed yet.** Windows SmartScreen will warn,
  and users cannot verify the publisher. AceGears needs a code-signing certificate (an EV or
  OV certificate, or Azure Trusted Signing) before the first public release.
- **Lua scripts can use CPU and memory up to their limits** (30 s, 512 MB by default) and
  can read files in the allowed folders. Running an untrusted script means trusting it with
  those folders.
- **The app bridge trusts every process of the same user.** That is the same trust boundary
  as the user's own files; turn the bridge off when not using an AI assistant.
- **Development-key builds** accept keys anyone can make (the development secret is public).
  Only builds made with `ACESTUDIO_LICENCE_PUBLIC_KEY` set may be sold (`tools/licence`).

## Running the fuzzers

    cmake -S . -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
          -DACESTUDIO_FUZZ=ON -DACESTUDIO_BUILD_APP=OFF -DACESTUDIO_BUILD_TESTS=OFF -DACESTUDIO_BUILD_BENCH=OFF
    cmake --build build-fuzz
    build-fuzz/tests/fuzz/fuzz_gif corpus/gif -max_total_time=600 -rss_limit_mb=2048

Targets: `fuzz_project`, `fuzz_png`, `fuzz_gif`, `fuzz_apng`, `fuzz_webp`, `fuzz_palette`,
`fuzz_licence`, `fuzz_lua`, `fuzz_mcp`. Crashing inputs found so far are kept as regression
tests in `tests/fuzz/regressions/`.
