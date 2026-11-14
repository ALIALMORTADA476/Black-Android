# PowerShell Environment Verification Script for BLACK Enhanced Edition
# Stage 2.1: Environment Recovery & Genuine Build Validation

$ErrorActionPreference = "Continue"

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " BLACK Enhanced Edition — Deep Environment Audit" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# 1. Operating System and Architecture
Write-Host "`n--- [1] OS & System Architecture ---" -ForegroundColor Yellow
Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, OSArchitecture | Format-Table -AutoSize

# 2. Environment Variables Audit
Write-Host "`n--- [2] Key Environment Variables ---" -ForegroundColor Yellow
$envVars = @("JAVA_HOME", "ANDROID_HOME", "ANDROID_SDK_ROOT", "NDK_HOME", "CMAKE_HOME", "VSINSTALLDIR", "PATH")
foreach ($v in $envVars) {
    $val = [System.Environment]::GetEnvironmentVariable($v, "Process")
    $userVal = [System.Environment]::GetEnvironmentVariable($v, "User")
    $machineVal = [System.Environment]::GetEnvironmentVariable($v, "Machine")
    Write-Host "$v : " -NoNewline
    if ($val) { Write-Host "$val (Effective)" -ForegroundColor Green }
    else { Write-Host "<NOT SET> (Effective)" -ForegroundColor Red }
    if ($userVal) { Write-Host "   User: $userVal" -ForegroundColor Gray }
    if ($machineVal) { Write-Host "   Machine: $machineVal" -ForegroundColor Gray }
}

# 3. Java / JDK Deep Probe
Write-Host "`n--- [3] Java / JDK Installations ---" -ForegroundColor Yellow
# Test java in path
try {
    $javaPath = (Get-Command java -ErrorAction SilentlyContinue).Source
    Write-Host "java in PATH: $javaPath" -ForegroundColor Green
    $javaVer = (java -version 2>&1) | Out-String
    Write-Host $javaVer -ForegroundColor Gray
} catch {
    Write-Host "java command failed or not found in PATH" -ForegroundColor Red
}

# Search common Java paths
$javaLocations = @(
    "C:\Program Files\Java",
    "C:\Program Files (x86)\Java",
    "C:\Program Files\Eclipse Adoptium",
    "C:\Program Files\Android\Android Studio\jbr",
    "C:\Program Files\Android\Android Studio\jre",
    "$env:LOCALAPPDATA\Programs\Eclipse Adoptium",
    "$env:LOCALAPPDATA\Android\Sdk\jbr",
    "E:\Java",
    "E:\jdk*",
    "D:\Java",
    "D:\jdk*"
)
Write-Host "Scanning disk for JDK installations..."
foreach ($loc in $javaLocations) {
    if (Test-Path $loc) {
        Get-ChildItem -Path $loc -Recurse -Depth 2 -Filter "javac.exe" -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "Found javac: $($_.FullName)" -ForegroundColor Green
            try {
                $verOut = (& $_.FullName -version 2>&1) | Out-String
                Write-Host "  Version: $verOut" -ForegroundColor Gray
            } catch {}
        }
    }
}

# 4. Android SDK, NDK, and CMake Probe
Write-Host "`n--- [4] Android SDK, NDK & CMake ---" -ForegroundColor Yellow
$sdkCandidates = @(
    "$env:LOCALAPPDATA\Android\Sdk",
    "C:\Android\Sdk",
    "C:\Android",
    "E:\Android\Sdk",
    "E:\Android",
    "D:\Android\Sdk",
    "D:\Android"
)
foreach ($sdk in $sdkCandidates) {
    if (Test-Path $sdk) {
        Write-Host "Discovered Android SDK candidate: $sdk" -ForegroundColor Green
        # Check platforms
        $platformsDir = Join-Path $sdk "platforms"
        if (Test-Path $platformsDir) {
            Write-Host "  Platforms found:"
            Get-ChildItem $platformsDir | Select-Object -ExpandProperty Name | ForEach-Object { Write-Host "    - $_" -ForegroundColor Cyan }
        } else {
            Write-Host "  Platforms folder missing." -ForegroundColor Red
        }
        # Check NDK
        $ndkDir = Join-Path $sdk "ndk"
        if (Test-Path $ndkDir) {
            Write-Host "  NDKs found:"
            Get-ChildItem $ndkDir | Select-Object -ExpandProperty Name | ForEach-Object { Write-Host "    - $_" -ForegroundColor Cyan }
        } else {
            Write-Host "  ndk folder missing." -ForegroundColor Red
        }
        # Check CMake
        $cmakeDir = Join-Path $sdk "cmake"
        if (Test-Path $cmakeDir) {
            Write-Host "  SDK CMakes found:"
            Get-ChildItem $cmakeDir | Select-Object -ExpandProperty Name | ForEach-Object { Write-Host "    - $_" -ForegroundColor Cyan }
        } else {
            Write-Host "  sdk/cmake folder missing." -ForegroundColor Red
        }
        # Check platform-tools
        $adb = Join-Path $sdk "platform-tools\adb.exe"
        if (Test-Path $adb) { Write-Host "  adb: $adb" -ForegroundColor Green }
    }
}

# 5. Visual Studio 2022 & C++ Compilers
Write-Host "`n--- [5] Visual Studio & C++ Compilers ---" -ForegroundColor Yellow
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    Write-Host "vswhere.exe located at: $vswhere" -ForegroundColor Green
    & $vswhere -products * -all -format text | Out-String | Write-Host
} else {
    Write-Host "vswhere.exe NOT found at $vswhere" -ForegroundColor Red
}

# Check cl.exe, clang.exe, gcc.exe in PATH and system
$compilerCommands = @("cl", "clang", "clang++", "gcc", "g++", "cmake", "ninja", "make", "msbuild")
foreach ($cmd in $compilerCommands) {
    $found = Get-Command $cmd -ErrorAction SilentlyContinue
    if ($found) {
        Write-Host "$cmd : $($found.Source)" -ForegroundColor Green
        try {
            $ver = (& $cmd --version 2>&1) | Select-Object -First 1
            Write-Host "   $ver" -ForegroundColor Gray
        } catch {}
    } else {
        Write-Host "$cmd : <NOT FOUND>" -ForegroundColor Red
    }
}

# Search Program Files for MSVC / Clang / MinGW
$extraCompilerPaths = @(
    "C:\Program Files\Microsoft Visual Studio",
    "C:\Program Files (x86)\Microsoft Visual Studio",
    "C:\Program Files\LLVM",
    "C:\LLVM",
    "C:\msys64",
    "C:\MinGW",
    "C:\w64devkit",
    "E:\LLVM",
    "E:\msys64",
    "E:\Visual Studio",
    "D:\LLVM",
    "D:\msys64"
)
foreach ($p in $extraCompilerPaths) {
    if (Test-Path $p) {
        Write-Host "Found tool directory: $p" -ForegroundColor Green
        Get-ChildItem -Path $p -Recurse -Depth 4 -Filter "*clang*.exe" -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "  Found compiler: $($_.FullName)" -ForegroundColor Cyan
        }
        Get-ChildItem -Path $p -Recurse -Depth 4 -Filter "cl.exe" -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "  Found compiler: $($_.FullName)" -ForegroundColor Cyan
        }
    }
}

# 6. Git, Python, and Gradle
Write-Host "`n--- [6] Git, Python, and Gradle ---" -ForegroundColor Yellow
$tools = @("git", "python", "py")
foreach ($t in $tools) {
    $found = Get-Command $t -ErrorAction SilentlyContinue
    if ($found) {
        $ver = (& $t --version 2>&1) | Out-String
        Write-Host "$t : $($found.Source) -> $ver" -ForegroundColor Green
    } else {
        Write-Host "$t : <NOT FOUND>" -ForegroundColor Red
    }
}

# Test Gradle wrapper in project
$gradlew = "e:\ai\black\gradlew.bat"
if (Test-Path $gradlew) {
    Write-Host "Project gradlew exists: $gradlew" -ForegroundColor Green
}

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host " Audit Complete" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

