$ErrorActionPreference = 'Stop'

$RootDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $RootDir 'build-gcc' }
$BuildType = if ($env:CMAKE_BUILD_TYPE) { $env:CMAKE_BUILD_TYPE } else { 'Debug' }

if ($env:CMAKE_COMMAND) {
    $CMake = $env:CMAKE_COMMAND
} else {
    $CMakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($CMakeCommand) {
        $CMake = $CMakeCommand.Source
    } else {
        $CMake = 'C:\Program Files\CMake\bin\cmake.exe'
    }
}

if (-not (Test-Path -LiteralPath $RootDir)) {
    throw "Repository root was not found: $RootDir"
}
if (-not (Test-Path -LiteralPath $CMake)) {
    throw "CMake was not found at $CMake; set CMAKE_COMMAND"
}

& $CMake -S $RootDir -B $BuildDir -DCMAKE_BUILD_TYPE=$BuildType
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$BuildArgs = @('--build', $BuildDir)
if ($env:CMAKE_BUILD_PARALLEL_LEVEL) {
    $BuildArgs += @('--parallel', $env:CMAKE_BUILD_PARALLEL_LEVEL)
} else {
    $BuildArgs += @('--parallel', '2')
}
& $CMake @BuildArgs
exit $LASTEXITCODE
