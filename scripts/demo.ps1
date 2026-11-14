$ErrorActionPreference = 'Stop'

$RootDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $RootDir 'build-gcc' }
& (Join-Path $RootDir 'scripts\build.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Binary = Join-Path $BuildDir 'bank.exe'
if (-not (Test-Path -LiteralPath $Binary)) {
    throw "bank executable was not produced at $Binary"
}

$RunArgs = @($args)
if ($RunArgs.Count -eq 0) {
    $RunArgs = @('--accounts', '50', '--clients', '10', '--ops', '10',
        '--workers', '4', '--seed', '42', '--verbose')
}
Push-Location $RootDir
try {
    & $Binary @RunArgs
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
