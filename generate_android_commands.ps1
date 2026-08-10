#!/usr/bin/env pwsh
# Generates compile_commands.json for Android using NDK toolchain
# Run from project root: .\generate_android_commands.ps1

$ErrorActionPreference = "Stop"

$AndroidNdk = if ($env:ANDROID_NDK) { $env:ANDROID_NDK } else { "$env:ANDROID_HOME\ndk\26.1.10909125" }
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$RaymobDir = "$ProjectRoot\raymob"
$BuildDir = "$RaymobDir\.cxx"

Write-Host "Using NDK: $AndroidNdk"
Write-Host "Project root: $ProjectRoot"

if (-not (Test-Path $AndroidNdk)) {
    Write-Error "NDK not found at: $AndroidNdk"
    Write-Host "Set ANDROID_NDK environment variable to your NDK path"
    exit 1
}

New-Item -ItemType Directory -Force -Path "$BuildDir\compile_commands" | Out-Null

cmake -S "$RaymobDir\app\src\main\cpp" `
    -B "$BuildDir\compile_commands" `
    -G Ninja `
    -D CMAKE_TOOLCHAIN_FILE="$AndroidNdk\build\cmake\android.toolchain.cmake" `
    -D CMAKE_SYSTEM_NAME=Android `
    -D CMAKE_SYSTEM_VERSION=24 `
    -D ANDROID_ABI="arm64-v8a" `
    -D PLATFORM=Android `
    -D APP_LIB_NAME="raymob" `
    -D GLFW_BUILD_X11=ON `
    -D GLFW_BUILD_WAYLAND=ON `
    -D PROJECT_ROOT="$ProjectRoot" `
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

Write-Host "compile_commands.json generated at: $BuildDir\compile_commands\compile_commands.json"
