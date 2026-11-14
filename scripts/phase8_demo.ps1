param(
    [switch]$SkipBuild,
    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'
$RootDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $RootDir 'build-gcc' }
$Binary = Join-Path $BuildDir 'bank.exe'
$CTest = 'C:\Program Files\CMake\bin\ctest.exe'

function Invoke-Step {
    param([string]$Label, [scriptblock]$Action)
    Write-Host "`n=== $Label ===" -ForegroundColor Cyan
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

Push-Location $RootDir
try {
    if (-not $SkipBuild) {
        Invoke-Step 'Build' { & (Join-Path $RootDir 'scripts\build.ps1') }
    }
    if (-not (Test-Path -LiteralPath $Binary)) {
        throw "bank executable was not found at $Binary"
    }

    Invoke-Step 'Startup recovery' { & $Binary '--recover' }
    if (-not $SkipCheck) {
        Invoke-Step 'Consistency checker' {
            & $Binary '--check' '--baseline-file' (Join-Path $RootDir 'results\baseline.txt')
        }
    }
    Invoke-Step 'Safe live simulation' {
        & $Binary '--accounts' '50' '--clients' '20' '--ops' '100' '--workers' '4' '--seed' '42' '--verbose'
    }
    Invoke-Step 'Unsafe lost-update test' {
        & $CTest '--test-dir' $BuildDir '--output-on-failure' `
            '-R' 'UnsafeRaceTest.R2ReportsLostUpdatesAcrossTwentyRuns'
    }
    Invoke-Step 'Safe synchronization tests' {
        & $CTest '--test-dir' $BuildDir '--output-on-failure' `
            '-R' 'SynchronizationTest.R1|SynchronizationTest.R3|SynchronizationTest.D2'
    }

    Write-Host "`n=== Unsupported live crash step ===" -ForegroundColor Yellow
    Write-Host 'X2 process-kill recovery is documented but has no automated harness yet.'
    Write-Host 'See docs\DEMO_RUNBOOK.md for the evidence boundary and next step.'
} finally {
    Pop-Location
}
