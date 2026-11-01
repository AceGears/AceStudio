# Puts the embeddable Python (and a pip wheel) into dist\python for the
# installer and the zip. Both downloads are verified: Python against the
# SHA-256 in python.org's own release manifest, pip against the pinned hash.
param([string]$Dist = "dist")
$ErrorActionPreference = "Stop"
$PyVersion = "3.12.10"
$PipWheel  = "pip-26.2.2-py3-none-any.whl"
$PipUrl    = "https://files.pythonhosted.org/packages/3f/06/3e4420302363fbb71be3c7f35ce042f54bfca330e3e66b3a2e3c4cca579f/$PipWheel"
$PipSha256 = "fd7f4dfa8190e014013b9f08f3e973f4e4c26ffd179a204895533a7254813a70"

$tmp = Join-Path $env:RUNNER_TEMP "python-bundle"
New-Item -ItemType Directory -Force $tmp | Out-Null
$manifest = Invoke-RestMethod "https://www.python.org/ftp/python/$PyVersion/windows-$PyVersion.json"
$entry = $manifest.versions | Where-Object { $_.id -like "pythonembed-*-64" } | Select-Object -First 1
if (-not $entry) { throw "python.org manifest has no 64-bit embeddable entry" }
$zip = Join-Path $tmp "python-embed.zip"
Invoke-WebRequest $entry.url -OutFile $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
$want = $entry.hash.sha256.ToLower()
Write-Host "Python $PyVersion embeddable: $($entry.url) sha256 $hash"
if ($hash -ne $want) { throw "Python download hash $hash does not match python.org's manifest ($want)" }

$py = Join-Path $Dist "python"
if (Test-Path $py) { Remove-Item -Recurse -Force $py }
Expand-Archive $zip -DestinationPath $py
$pip = Join-Path $py $PipWheel
Invoke-WebRequest $PipUrl -OutFile $pip
$ph = (Get-FileHash $pip -Algorithm SHA256).Hash.ToLower()
if ($ph -ne $PipSha256) { throw "pip wheel hash $ph does not match $PipSha256" }
& (Join-Path $py "python.exe") -c "import sys, struct; print(sys.version, struct.calcsize('P') * 8, 'bit')"
if ($LASTEXITCODE -ne 0) { throw "the bundled Python does not run" }
