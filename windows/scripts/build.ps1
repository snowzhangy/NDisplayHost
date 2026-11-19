<#
.SYNOPSIS
  Build the WVD user-mode components (owner, media, input, tools).
.DESCRIPTION
  Configures with CMake + Ninja (MSVC) and builds. The IddCx driver under
  driver/ is NOT built here — it needs the WDK (see driver/README.md).
.PARAMETER Clean   Wipe build/ and reconfigure.
.PARAMETER Nvenc   Path to an NVIDIA Video Codec SDK dir to enable NVENC.
.PARAMETER Config  Debug | Release | RelWithDebInfo (default).
#>
[CmdletBinding()]
param(
  [switch]$Clean,
  [string]$Nvenc,
  [ValidateSet('Debug','Release','RelWithDebInfo')]
  [string]$Config = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'

if ($Clean -and (Test-Path $build)) {
  Write-Host "removing $build" -ForegroundColor Yellow
  Remove-Item -Recurse -Force $build
}

$cfgArgs = @('-S', $root, '-B', $build, '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=' + $Config)
if ($Nvenc) { $cfgArgs += @('-DWVD_ENABLE_NVENC=ON', ('-DWVD_NVENC_SDK_DIR=' + $Nvenc)) }

Write-Host "== configure ==" -ForegroundColor Cyan
& cmake @cfgArgs
if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }

Write-Host "== build ==" -ForegroundColor Cyan
& cmake --build $build
if ($LASTEXITCODE) { throw "cmake build failed ($LASTEXITCODE)" }

Write-Host "`nBinaries:" -ForegroundColor Green
Get-ChildItem -Recurse $build -Filter 'wvd-*.exe' | ForEach-Object { $_.FullName }
