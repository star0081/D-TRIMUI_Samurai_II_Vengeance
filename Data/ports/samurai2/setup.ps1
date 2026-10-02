# Extract Samurai II 1.1.4 APK into this port. Game files stay gitignored.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if ((Split-Path -Leaf $PSScriptRoot) -eq "samurai2") {
  # script lives in Data/ports/samurai2
} else {
  $Root = $PSScriptRoot
}
$Port = $PSScriptRoot
if ((Split-Path -Leaf $Port) -ne "samurai2") {
  $Port = Join-Path $Root "Data\ports\samurai2"
}
$Apk = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $Port))) "Samurai+II+-+Vengeance+v.1.1.4.apk"
if (-not (Test-Path $Apk)) {
  $found = Get-ChildItem -Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $Port))) -Filter *.apk | Select-Object -First 1
  if ($found) { $Apk = $found.FullName } else { throw "APK not found" }
}
$GameData = Join-Path $Port "gamedata"
$Libs = Join-Path $Port "gamefiles\android-libs"
New-Item -ItemType Directory -Force -Path $GameData, $Libs | Out-Null
$DestApk = Join-Path $GameData "samurai2-1.1.4.apk"
if (-not (Test-Path $DestApk)) {
  Copy-Item $Apk $DestApk
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($DestApk)
try {
  foreach ($name in @("lib/armeabi-v7a/libmain.so", "lib/armeabi-v7a/libunity.so", "lib/armeabi-v7a/libmono.so")) {
    $entry = $zip.GetEntry($name)
    if (-not $entry) { throw "missing $name" }
    $out = Join-Path $Libs (Split-Path $name -Leaf)
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $out, $true)
  }
  foreach ($entry in $zip.Entries) {
    if (-not $entry.FullName.StartsWith("assets/")) { continue }
    if ($entry.FullName.EndsWith("/")) { continue }
    $out = Join-Path $Port ($entry.FullName -replace "/", "\")
    $dir = Split-Path $out -Parent
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $out, $true)
  }
} finally {
  $zip.Dispose()
}
Set-Content -Path (Join-Path $Port "gamefiles\.ready-1.1.4") -Value "Samurai II 1.1.4 101040" -Encoding ascii
Write-Host "setup: ok $DestApk"
