# Comprehensive Prerequisites Audit for Stage 3
$ErrorActionPreference = "Continue"

Write-Host "============================================================"
Write-Host " BLACK Enhanced Edition - Stage 3 Prerequisites Audit"
Write-Host "============================================================"

# Resolve User / Machine env vars into current process if needed
$userAndroidHome = [System.Environment]::GetEnvironmentVariable("ANDROID_HOME", "User")
$userJavaHome = [System.Environment]::GetEnvironmentVariable("JAVA_HOME", "User")

if (-not $env:ANDROID_HOME -and $userAndroidHome) {
    $env:ANDROID_HOME = $userAndroidHome
}
if (-not $env:JAVA_HOME -and $userJavaHome) {
    $env:JAVA_HOME = $userJavaHome
}

Write-Host "Effective ANDROID_HOME: $env:ANDROID_HOME"
Write-Host "Effective JAVA_HOME:    $env:JAVA_HOME"

# 1. Android SDK
$android34 = Join-Path $env:ANDROID_HOME "platforms\android-34"
Write-Host "Platform android-34: $(Test-Path $android34) -> $android34"

# 2. Android NDK
$ndkDir = Join-Path $env:ANDROID_HOME "ndk\28.2.13676358"
$ndkClang = Join-Path $ndkDir "toolchains\llvm\prebuilt\windows-x86_64\bin\clang-cl.exe"
Write-Host "NDK 28.2.13676358:   $(Test-Path $ndkDir) -> $ndkDir"
Write-Host "NDK clang-cl.exe:    $(Test-Path $ndkClang) -> $ndkClang"

# 3. Android CMake
$cmakeExe = Join-Path $env:ANDROID_HOME "cmake\3.31.6\bin\cmake.exe"
Write-Host "CMake 3.31.6:        $(Test-Path $cmakeExe) -> $cmakeExe"

# 4. Java JDK
$javacExe = Join-Path $env:JAVA_HOME "bin\javac.exe"
Write-Host "JDK javac.exe:       $(Test-Path $javacExe) -> $javacExe"
if (Test-Path $javacExe) {
    & $javacExe -version
}

# 5. Visual Studio 2022
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
Write-Host "vswhere.exe:         $(Test-Path $vswhere) -> $vswhere"
if (Test-Path $vswhere) {
    $vsDir = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    Write-Host "Discovered VS Dir:   $vsDir"
    $devCmd = Join-Path $vsDir "Common7\Tools\VsDevCmd.bat"
    Write-Host "VsDevCmd.bat:        $(Test-Path $devCmd) -> $devCmd"
}

# 6. Clang
$clangPath = "C:\Program Files\LLVM\bin\clang.exe"
Write-Host "Host Clang:          $(Test-Path $clangPath)"
if (Test-Path $clangPath) {
    & $clangPath --version | Select-Object -First 1
}

# 7. ISO File
$isoPath = "E:\ai\black\assets\black.iso"
Write-Host "Game ISO:            $(Test-Path $isoPath) -> $isoPath"
if (Test-Path $isoPath) {
    $iso = Get-Item $isoPath
    Write-Host "ISO Size:            $($iso.Length) bytes ($([math]::Round($iso.Length / 1GB, 2)) GB)"
}

