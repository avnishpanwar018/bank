$ErrorActionPreference = 'Stop'

$RootDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $RootDir 'build-gcc' }
$Baseline = if ($env:BASELINE_FILE) {
    $env:BASELINE_FILE
} else {
    Join-Path $RootDir 'results\baseline.txt'
}

& (Join-Path $RootDir 'scripts\build.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not (Test-Path -LiteralPath $Baseline)) {
    throw "Baseline file was not found at $Baseline"
}

$Binary = Join-Path $BuildDir 'bank.exe'
Push-Location $RootDir
try {
    & $Binary '--recover' '--check' '--baseline-file' $Baseline @args
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
