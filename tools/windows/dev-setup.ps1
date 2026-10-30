# AceStudio developer setup and build for Windows 10/11 (x64).
#
# What it does (asks before starting):
#   1. Installs Visual Studio 2022 Build Tools (C++ workload) with winget, if missing.
#      This needs one administrator prompt. About 6-8 GB.
#   2. Installs Python 3.12 with winget, if no Python 3.10+ is found.
#   3. Downloads Qt 6.8.3 (MSVC 2022, LGPL) into this folder with aqtinstall. About 1 GB.
#   4. Builds AceStudio (Release), runs all tests, stages a runnable copy in .\dist.
#   5. Writes everything to .\logs\setup-<time>.log and a short .\SUMMARY.txt.
#
# Everything except Build Tools and Python stays inside this folder: delete
# the folder to remove it. Run with: right-click > Run with PowerShell,
# or double-click RUN-SETUP.cmd.

$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src = Join-Path $Root 'src'
$Build = Join-Path $Root 'build'
$Dist = Join-Path $Root 'dist'
$QtVersion = '6.8.3'
$QtDir = Join-Path $Root "Qt\$QtVersion\msvc2022_64"
New-Item -ItemType Directory -Force (Join-Path $Root 'logs') | Out-Null
$Log = Join-Path $Root ("logs\setup-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$Summary = New-Object System.Collections.Generic.List[string]

function Say($msg) { Write-Host $msg; Add-Content -Path $Log -Value $msg }
function Step($name, $ok, $detail) {
    $line = ('{0,-34} {1}  {2}' -f $name, $(if ($ok) { 'OK  ' } else { 'FAIL' }), $detail)
    $Summary.Add($line); Say $line
}
function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
function Run-Logged($title, $cmdline) {
    Say "`n=== $title ===`n> $cmdline"
    cmd /c "($cmdline) 2>&1" | Tee-Object -FilePath $Log -Append | Out-Host
    return $LASTEXITCODE
}

Write-Host ''
Write-Host 'AceStudio developer setup' -ForegroundColor Cyan
Write-Host "Folder: $Root"
Write-Host 'This installs VS 2022 Build Tools and Python if missing (system-wide, via winget),'
Write-Host 'downloads Qt into this folder, then builds and tests AceStudio.'
Write-Host 'Licences: VS Build Tools (Microsoft licence terms, shown by winget), Python (PSF), Qt (LGPLv3).'
$answer = Read-Host 'Continue? (y/n)'
if ($answer -ne 'y') { exit 1 }

Say ("Started " + (Get-Date -Format o))
Say ("Windows: " + [Environment]::OSVersion.VersionString)
try { Say ("GPU: " + ((& nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader) -join '; ')) } catch { Say 'GPU: nvidia-smi not available' }

# --- 1. Visual Studio Build Tools -------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
function Find-VS {
    if (-not (Test-Path $vswhere)) { return $null }
    $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($p) { return $p.Trim() } else { return $null }
}
$vs = Find-VS
if (-not $vs) {
    Say 'Installing Visual Studio 2022 Build Tools (C++). Accept the administrator prompt; this takes a while...'
    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget --accept-package-agreements --accept-source-agreements `
        --override '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended' 2>&1 | Tee-Object -FilePath $Log -Append | Out-Host
    $vs = Find-VS
}
Step 'Visual Studio C++ Build Tools' ([bool]$vs) "$vs"
if (-not $vs) { Say 'Cannot continue without the C++ compiler.'; $Summary | Set-Content (Join-Path $Root 'SUMMARY.txt'); Read-Host 'Press Enter to close'; exit 1 }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

# --- 2. Python -----------------------------------------------------------------
function Rest($arr) { if ($arr.Length -gt 1) { return $arr[1..($arr.Length - 1)] } else { return @() } }
function Find-Python {
    foreach ($c in @(@('py', '-3'), @('python'))) {
        try {
            $rest = Rest $c
            $v = & $c[0] @rest -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])' 2>$null
            if ($LASTEXITCODE -eq 0 -and $v -match '^3\.(1[0-9]|[2-9][0-9])') { return @{ cmd = $c; version = $v.Trim() } }
        } catch {}
    }
    return $null
}
$py = Find-Python
if (-not $py) {
    Say 'Installing Python 3.12...'
    winget install --id Python.Python.3.12 -e --source winget --accept-package-agreements --accept-source-agreements 2>&1 | Tee-Object -FilePath $Log -Append | Out-Host
    Refresh-Path
    $py = Find-Python
}
Step 'Python 3.10+' ([bool]$py) $(if ($py) { $py.version } else { 'not found' })
if (-not $py) { Say 'Cannot continue without Python (needed for Qt download and AI tests).'; $Summary | Set-Content (Join-Path $Root 'SUMMARY.txt'); Read-Host 'Press Enter to close'; exit 1 }
$pyExe = $py.cmd[0]; $pyArgs = @(Rest $py.cmd)

# --- 3. Qt ---------------------------------------------------------------------
if (-not (Test-Path (Join-Path $QtDir 'bin\Qt6Widgets.dll'))) {
    $toolsVenv = Join-Path $Root 'tools-venv'
    & $pyExe @pyArgs -m venv $toolsVenv
    $tpy = Join-Path $toolsVenv 'Scripts\python.exe'
    Run-Logged 'aqtinstall' "`"$tpy`" -m pip install --disable-pip-version-check -q aqtinstall" | Out-Null
    Run-Logged "Qt $QtVersion" "`"$tpy`" -m aqt install-qt windows desktop $QtVersion win64_msvc2022_64 --outputdir `"$(Join-Path $Root 'Qt')`"" | Out-Null
}
$qtOk = Test-Path (Join-Path $QtDir 'bin\Qt6Widgets.dll')
Step "Qt $QtVersion (MSVC 2022)" $qtOk $QtDir
if (-not $qtOk) { $Summary | Set-Content (Join-Path $Root 'SUMMARY.txt'); Read-Host 'Press Enter to close'; exit 1 }

# --- 4. Build and test ---------------------------------------------------------
$env:CMAKE_BUILD_PARALLEL_LEVEL = [Environment]::ProcessorCount
$t0 = Get-Date
$code = Run-Logged 'Configure' "call `"$vcvars`" >nul && cmake -S `"$Src`" -B `"$Build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=`"$QtDir`""
Step 'Configure (CMake)' ($code -eq 0) ''
if ($code -eq 0) {
    $code = Run-Logged 'Build' "call `"$vcvars`" >nul && cmake --build `"$Build`""
    Step 'Build' ($code -eq 0) ('{0:N0} s' -f ((Get-Date) - $t0).TotalSeconds)
}
if ($code -eq 0) {
    $env:Path = "$QtDir\bin;$env:Path"
    $code = Run-Logged 'C++ tests' "call `"$vcvars`" >nul && ctest --test-dir `"$Build`" --output-on-failure"
    $tests = (Select-String -Path $Log -Pattern 'tests passed, \d+ tests failed out of \d+' | Select-Object -Last 1).Line
    Step 'C++ tests (ctest)' ($code -eq 0) "$tests"
    $code2 = Run-Logged 'AI worker tests' "cd /d `"$(Join-Path $Src 'ai_worker')`" && $pyExe $($pyArgs -join ' ') -m unittest discover -s tests -v"
    Step 'AI worker tests (Python)' ($code2 -eq 0) ''

    # Stage a runnable copy (Qt DLLs next to the exe: LGPL dynamic linking).
    New-Item -ItemType Directory -Force $Dist | Out-Null
    Copy-Item (Join-Path $Build 'app\AceStudio.exe'), (Join-Path $Build 'cli\acestudio-cli.exe') $Dist -Force
    Run-Logged 'windeployqt' "`"$QtDir\bin\windeployqt.exe`" --release --no-translations `"$Dist\AceStudio.exe`"" | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $Dist 'ai_worker') | Out-Null
    foreach ($d in 'acestudio_worker', 'requirements', 'catalog') { Copy-Item -Recurse -Force (Join-Path $Src "ai_worker\$d") (Join-Path $Dist 'ai_worker') }
    Copy-Item -Recurse -Force (Join-Path $Root 'samples') $Dist -ErrorAction SilentlyContinue
    Step 'Staged app in dist\' (Test-Path (Join-Path $Dist 'Qt6Widgets.dll')) $Dist
    Run-Logged 'CLI smoke' "`"$Dist\acestudio-cli.exe`" version && `"$Dist\acestudio-cli.exe`" ai status" | Out-Null
}

Say ("Finished " + (Get-Date -Format o))
$Summary | Set-Content (Join-Path $Root 'SUMMARY.txt')
Write-Host ''
Write-Host 'Summary' -ForegroundColor Cyan
$Summary | ForEach-Object { Write-Host $_ }
Write-Host "`nLog: $Log"
Write-Host "Run the app: $Dist\AceStudio.exe"
Read-Host 'Press Enter to close'
