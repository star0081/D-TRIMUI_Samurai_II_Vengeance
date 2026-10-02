# Build Shadowgun 32-bit runtime + GLES2 client with Zig 0.13
$ErrorActionPreference = "Stop"
$Zig = "D:\Sandbox Cursor\PortMaster\tsp-eaprules\zig-windows-x86_64-0.13.0\zig.exe"
if (-not (Test-Path $Zig)) {
    throw "Zig 0.13 not found: $Zig"
}

$Root = Split-Path -Parent $PSScriptRoot
if ((Split-Path -Leaf $PSScriptRoot) -ne "src") {
    $Root = $PSScriptRoot
    $Src = Join-Path $Root "src"
} else {
    $Src = $PSScriptRoot
    $Root = Split-Path -Parent $Src
}

$Rt = Join-Path $Src "runtime"
$Gl = Join-Path $Src "glbridge"
$OutRt = Join-Path $Root "samurai2_runtime"
$GlOut = Join-Path $Root "glbridge"

New-Item -ItemType Directory -Force -Path $GlOut | Out-Null

$RtFiles = @(
    (Join-Path $Rt "main.c"),
    (Join-Path $Rt "elf32_loader.c"),
    (Join-Path $Rt "compat_bridge.c"),
    (Join-Path $Rt "crash_trace.c"),
    (Join-Path $Rt "initializer_trace.c"),
    (Join-Path $Rt "softfp_bridge.c"),
    (Join-Path $Rt "softfp_symbols.c"),
    (Join-Path $Rt "symbol_probe.c"),
    (Join-Path $Rt "relocation_probe.c"),
    (Join-Path $Rt "opensl_bridge.c"),
    (Join-Path $Rt "platform_probe.c"),
    (Join-Path $Rt "android_native.c"),
    (Join-Path $Rt "jni_unity.c"),
    (Join-Path $Rt "collider_debug.c"),
    (Join-Path $Rt "zip_obb.c"),
    (Join-Path $Rt "puff.c"),
    (Join-Path $Rt "bionic_setjmp.S")
)

Write-Host "compile samurai2_runtime (armhf PIE)..."
Select-String -Path (Join-Path $Rt "build_id.h") -Pattern "SG_RUNTIME_BUILD" | ForEach-Object { Write-Host $_.Line.Trim() }
& $Zig cc -target arm-linux-gnueabihf -mcpu=cortex_a7 -O2 -s `
    -fPIC -fPIE `
    -D_GNU_SOURCE -fno-unwind-tables -fno-asynchronous-unwind-tables `
    -I $Rt -o $OutRt @RtFiles -ldl -lpthread -lm
if ($LASTEXITCODE -ne 0) { throw "runtime compile failed" }

Write-Host "compile glbridge client (armhf)..."
$Client = Join-Path $GlOut "libEGL.so.1"
& $Zig cc -target arm-linux-gnueabihf -shared -fPIC -O2 -s `
    -fno-unwind-tables -fno-asynchronous-unwind-tables `
    -I $Gl -I $Src -o $Client `
    (Join-Path $Gl "client.c") (Join-Path $Gl "client_xport.c") -lm
if ($LASTEXITCODE -ne 0) { throw "glbridge client compile failed" }

foreach ($name in @("libEGL.so", "libGLESv2.so", "libGLESv2.so.2")) {
    Copy-Item $Client (Join-Path $GlOut $name) -Force
}

Write-Host "compile glbridge server (aarch64)..."
$Present = Join-Path $Root "samurai2_present"
& $Zig cc -target aarch64-linux-gnu.2.17 -O2 -s `
    -fno-unwind-tables -fno-asynchronous-unwind-tables `
    -I $Gl -o $Present (Join-Path $Gl "server.c") -ldl
if ($LASTEXITCODE -ne 0) { throw "glbridge server compile failed" }

Write-Host "ok"
Write-Host " runtime $OutRt"
Write-Host " present $Present"
Write-Host " glbridge $Client"
Select-String -Path (Join-Path $Rt "build_id.h") -Pattern "#define SG_RUNTIME_BUILD" | ForEach-Object { Write-Host $_.Line.Trim() }
