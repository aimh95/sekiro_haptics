param(
    [string]$BuildDir = "build-feedback",
    [ValidateSet("Debug", "Release")][string]$Configuration = "Release"
)
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    & cmake -S . -B $BuildDir -A x64 -DSEKIRO_HAPTICS_BUILD_APPS=ON -DSEKIRO_HAPTICS_BUILD_TESTS=ON -DSEKIRO_HAPTICS_BUILD_DUALSENSE_TRANSPORT=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    & cmake --build $BuildDir --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed" }
    & ctest --test-dir $BuildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed; inspect failures before hardware use" }
    Write-Host "Build/tests finished. Hardware output has not been started."
    Write-Host "$BuildDir/apps/action_feedback/$Configuration/sekiro_action_feedback.exe --list-devices"
} finally {
    Pop-Location
}
