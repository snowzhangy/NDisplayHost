<#
.SYNOPSIS
  Lab-only install of the WVD IddCx driver + software device.
.DESCRIPTION
  Test signing path only (§4.5). Public installs use attestation signing via
  Partner Center; do NOT run this for production.

  Steps:
    1. Build the driver with the WDK first (see driver/README.md).
    2. Ensure test signing is enabled (reboot required if toggled).
    3. Import the lab certificate into LocalMachine Root + TrustedPublisher.
    4. pnputil add/install the INF.
    5. Start wvd-owner (creates the Root\WVDIDD software device; the driver
       binds to it and a virtual monitor arrives).

  Requires an elevated prompt. Secure Boot may need to be disabled for
  load-time signing on some builds.
#>
[CmdletBinding()]
param(
  [string]$InfPath = "$PSScriptRoot\..\driver\x64\RelWithDebInfo\wvdidd.inf",
  [string]$CertPath,           # .cer for the lab signing cert (optional)
  [switch]$SkipDriver,         # only (re)start the owner service
  [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  (New-Object Security.Principal.WindowsPrincipal $id
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) { throw "run from an elevated prompt" }

$root = Split-Path -Parent $PSScriptRoot
$owner = Join-Path $root 'build\src\owner\wvd-owner.exe'

if ($Uninstall) {
  Write-Host "stopping owner + removing device + driver" -ForegroundColor Yellow
  & $owner --stop
  $dev = Get-PnpDevice -Class Display | Where-Object { $_.FriendlyName -like '*WVD*' }
  if ($dev) { & pnputil /remove-device $dev.InstanceId }
  $oem = (Get-WindowsDriver -Online | Where-Object {
            $_.OriginalFileName -like '*wvdidd*'}).PublishedInfPath
  if ($oem) { & pnputil /delete-driver $oem /uninstall /force }
  return
}

if (-not $SkipDriver) {
  if (-not (Test-Path $InfPath)) {
    throw "driver INF not found: $InfPath`nBuild it with the WDK first (driver/README.md)."
  }

  # 1. Test signing on (reboot if we just turned it on).
  $ts = & bcdedit /enum "{current}" | Select-String 'testsigning'
  if (-not ($ts -match 'Yes')) {
    Write-Host "enabling test signing (a reboot is required)" -ForegroundColor Yellow
    & bcdedit /set testsigning on
    throw "test signing enabled — reboot, then re-run this script."
  }

  # 2. Trust the lab cert if provided.
  if ($CertPath -and (Test-Path $CertPath)) {
    Import-Certificate -CertFilePath $CertPath -CertStoreLocation Cert:\LocalMachine\Root
    Import-Certificate -CertFilePath $CertPath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
  }

  # 3. Add + install the driver package.
  Write-Host "pnputil add/install $InfPath" -ForegroundColor Cyan
  & pnputil /add-driver $InfPath /install
  if ($LASTEXITCODE) { throw "pnputil failed ($LASTEXITCODE)" }
}

# 4. Install + start the owner service (creates the software device → monitor).
Write-Host "installing + starting wvd-owner" -ForegroundColor Cyan
& $owner --install
& $owner --start
Write-Host "done — a 'WVD Virtual Display' should appear in Display Settings." -ForegroundColor Green
