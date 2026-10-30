# Optional: exercises AceStudio's AI features on this PC's GPU and records
# timings. Run after dev-setup.ps1. Asks before each download:
#   ONNX runtime packages (~50 MB), BiRefNet Lite model (224 MB, MIT),
#   PyTorch CUDA + Diffusers (~3 GB), Real-ESRGAN x4plus (67 MB, BSD-3),
#   SDXL Base 1.0 (~7 GB, CreativeML OpenRAIL++-M: read the licence first).
# AI files go to %LOCALAPPDATA%\AceStudio\ai (delete that folder to remove them).

$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Cli = Join-Path $Root 'dist\acestudio-cli.exe'
$Samples = Join-Path $Root 'samples'
$Out = Join-Path $Root 'ai-results'
New-Item -ItemType Directory -Force $Out, (Join-Path $Root 'logs') | Out-Null
$Log = Join-Path $Root ("logs\ai-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$Summary = New-Object System.Collections.Generic.List[string]
function Say($m) { Write-Host $m; Add-Content -Path $Log -Value $m }
function Ask($q) { (Read-Host "$q (y/n)") -eq 'y' }
function Cli($title, [string[]]$a) {
    Say "`n=== $title ===`n> acestudio-cli $($a -join ' ')"
    $t = Measure-Command { & $Cli @a 2>&1 | Tee-Object -FilePath $Log -Append | Out-Host }
    $ok = $LASTEXITCODE -eq 0
    $line = ('{0,-40} {1}  {2:N1} s' -f $title, $(if ($ok) { 'OK  ' } else { 'FAIL' }), $t.TotalSeconds)
    $Summary.Add($line); Say $line
    return $ok
}
if (-not (Test-Path $Cli)) { Write-Host 'Run dev-setup.ps1 first.'; Read-Host 'Press Enter'; exit 1 }
try { Say ("GPU: " + ((& nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader) -join '; ')) } catch { Say 'GPU: nvidia-smi not available' }

if (Ask 'Install the ONNX runtime profile (onnx-windows, ~50 MB) and BiRefNet Lite (224 MB, MIT licence)?') {
    Cli 'Setup onnx-windows' @('ai', 'setup', '--profile', 'onnx-windows', '--yes') | Out-Null
    Cli 'Download BiRefNet Lite' @('ai', 'download', 'birefnet-lite', '--accept-license', 'MIT') | Out-Null
    Cli 'Remove background (first run)' @('ai', 'remove-bg', "$Samples\scene.png", '--out', "$Out\scene-cutout.png", '--force') | Out-Null
    Cli 'Remove background (second run)' @('ai', 'remove-bg', "$Samples\scene.png", '--out', "$Out\scene-cutout.png", '--force') | Out-Null
}
if (Ask 'Install PyTorch with CUDA + Diffusers (diffusion-cuda, ~3 GB) and Real-ESRGAN x4plus (67 MB, BSD-3)?') {
    Cli 'Setup diffusion-cuda' @('ai', 'setup', '--profile', 'diffusion-cuda', '--yes') | Out-Null
    Cli 'AI status after CUDA setup' @('ai', 'status') | Out-Null
    Cli 'Download Real-ESRGAN x4plus' @('ai', 'download', 'realesrgan-x4plus', '--accept-license', 'BSD-3-Clause') | Out-Null
    Cli 'Upscale x4 (256 -> 1024)' @('ai', 'upscale', "$Samples\scene-256.png", '--out', "$Out\scene-x4.png", '--force') | Out-Null
    Write-Host ''
    Write-Host 'SDXL Base 1.0 is licensed under CreativeML Open RAIL++-M:'
    Write-Host '  https://huggingface.co/stabilityai/stable-diffusion-xl-base-1.0/blob/main/LICENSE.md'
    Write-Host 'It allows commercial use but forbids the uses listed in its Attachment A.'
    if (Ask 'Have you read it, and do you accept it and want to download SDXL (~7 GB)?') {
        if (Cli 'Download SDXL Base 1.0' @('ai', 'download', 'sdxl-base-1.0', '--accept-license', 'CreativeML-OpenRAIL++-M')) {
            Cli 'Generate 1024x1024, 30 steps (first, loads model)' @('ai', 'generate', '--prompt', 'a knight with a red shield, game character, side view', '--out', "$Out\sdxl-1.png", '--seed', '1', '--force') | Out-Null
            Cli 'Generate 1024x1024, 30 steps (second)' @('ai', 'generate', '--prompt', 'a treasure chest, game item, isometric', '--out', "$Out\sdxl-2.png", '--seed', '2', '--force') | Out-Null
            Cli 'Generate pixel art 64x64' @('ai', 'generate', '--prompt', 'a green slime monster, game sprite', '--out', "$Out\sdxl-pixel.png", '--pixel', '64x64', '--colors', '16', '--seed', '3', '--force') | Out-Null
        }
    }
}
try { Say ("GPU after: " + ((& nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader) -join '; ')) } catch {}
$Summary | Set-Content (Join-Path $Root 'AI-SUMMARY.txt')
Write-Host ''; Write-Host 'Summary' -ForegroundColor Cyan; $Summary | ForEach-Object { Write-Host $_ }
Write-Host "`nResults: $Out`nLog: $Log"
Read-Host 'Press Enter to close'
