<#
.SYNOPSIS
  Start the interactive desktop host and input helper from an existing build.
.DESCRIPTION
  Logs and process IDs go in .local/dev-session. Close the test card normally;
  stop the recorded media/input/sink processes when done. This does not install
  a driver or service. Run elevated only when testing input into elevated apps.
#>
[CmdletBinding()]
param(
  [ValidateRange(1024,65535)][int]$Port = 50001,
  [switch]$NoInput,
  [switch]$Sink,
  [switch]$TestCard,
  [switch]$ScreenOff,  # black out this PC's screens while the headset is connected
  [switch]$NoAudio     # never send this PC's sound
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildRoot = Join-Path $workspace 'windows/build/src'
$sessionDir = Join-Path $workspace '.local/dev-session'
New-Item -ItemType Directory -Path $sessionDir -Force | Out-Null
$mediaExe = Join-Path $buildRoot 'media/wvd-media.exe'
if (!(Test-Path -LiteralPath $mediaExe)) { throw 'Build windows/ first (scripts/build.ps1).' }
$started = @()
function Start-HostComponent([string]$Executable, [string[]]$Arguments, [string]$Name) {
  $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $workspace `
    -WindowStyle Hidden -RedirectStandardOutput (Join-Path $sessionDir "$Name.stdout.log") `
    -RedirectStandardError (Join-Path $sessionDir "$Name.stderr.log") -PassThru
  return $process
}
try {
  $mediaArgs = @('--listen', "$Port", '--codec', 'auto')
  if ($ScreenOff) { $mediaArgs += '--screen-off' }
  if ($NoAudio) { $mediaArgs += '--no-audio' }
  if (!$NoInput) {
    $bytes = New-Object byte[] 8
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $rng.GetBytes($bytes)
    $rng.Dispose()
    $token = [BitConverter]::ToString($bytes).Replace('-', '')
    $inputProcess = Start-HostComponent (Join-Path $buildRoot 'input/wvd-input.exe') @('--token', $token) 'input'
    $started += $inputProcess
    $mediaArgs += @('--input', '\\.\pipe\wvd.input', '--input-token', $token)
  }
  $mediaProcess = Start-HostComponent $mediaExe $mediaArgs 'media'
  $started += $mediaProcess
  Start-Sleep -Milliseconds 700
  if ($mediaProcess.HasExited -or ($inputProcess -and $inputProcess.HasExited)) {
    throw "A host component exited; see $sessionDir"
  }
  if ($TestCard) {
    # The test card is an interactive window explicitly requested for testing.
    $started += Start-Process -FilePath (Join-Path $buildRoot 'tools/testcard/wvd-testcard.exe') `
      -WorkingDirectory $workspace -PassThru
  }
  if ($Sink) {
    $started += Start-HostComponent (Join-Path $buildRoot 'tools/diag/wvd-diag.exe') @('sink', '--port', "$Port") 'sink'
  }
  $started | Select-Object Id, ProcessName | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $sessionDir 'processes.json')
  $started | Select-Object Id, ProcessName
  Write-Host "Desktop host listening on TCP $Port. Logs: $sessionDir"
  if ($Sink) { Write-Host 'The local diagnostic receiver occupies the connection; stop it before connecting a headset.' }
} catch {
  foreach ($process in $started) { if (!$process.HasExited) { $process.Kill() } }
  throw
}
