# Script to compile and run native C++ tests for BLACK Enhanced Edition
$ErrorActionPreference = "Stop"

$toolBin = "e:\ai\black\.tools\w64devkit\bin"
$env:PATH = "$toolBin;$env:PATH"

Write-Host "Verifying compiler..." -ForegroundColor Cyan
& "$toolBin\g++.exe" --version | Select-Object -First 1
& "$toolBin\cmake.exe" --version | Select-Object -First 1

$repoRoot = "e:\ai\black"
$testsDir = "$repoRoot\tests"
$buildDir = "$testsDir\build"

if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "`nConfiguring CMake for native tests..." -ForegroundColor Cyan
& "$toolBin\cmake.exe" -S $testsDir -B $buildDir -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$toolBin\ninja.exe" -DCMAKE_C_COMPILER="$toolBin\gcc.exe" -DCMAKE_CXX_COMPILER="$toolBin\g++.exe"

Write-Host "`nBuilding test_settings_core..." -ForegroundColor Cyan
& "$toolBin\cmake.exe" --build $buildDir --config Release

Write-Host "`nRunning native test_settings_core.exe..." -ForegroundColor Cyan
& "$buildDir\test_settings_core.exe"

