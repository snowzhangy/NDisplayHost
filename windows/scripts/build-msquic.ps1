<#
.SYNOPSIS
  Builds the pinned MsQuic dependency from an x64 Visual Studio developer shell.
#>
$ErrorActionPreference = 'Stop'

$revision = '4504c53564948c8075d6854a08c77e574c5cd3f9'
$upstream = 'https://github.com/microsoft/msquic.git'
$source = Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party/msquic'
$build = Join-Path $source 'build'

if (!(Test-Path -LiteralPath $source)) {
    & git init $source
    if ($LASTEXITCODE) { throw 'Could not initialize the MsQuic checkout.' }
}
if (!(Test-Path -LiteralPath (Join-Path $source '.git'))) {
    throw "Expected a Git checkout at $source. Move the existing directory and retry."
}
$head = & git -C $source rev-parse --verify --quiet HEAD
if (!$head) {
    & git -C $source fetch --depth=1 $upstream $revision
    if ($LASTEXITCODE) { throw 'Could not fetch the pinned MsQuic revision.' }
    & git -C $source checkout --detach $revision
    if ($LASTEXITCODE) { throw 'Could not check out the pinned MsQuic revision.' }
} elseif ($head -ne $revision) {
    throw "Expected MsQuic revision $revision in $source, found $head."
}
& git -C $source diff --quiet HEAD --
if ($LASTEXITCODE) { throw "Expected clean tracked MsQuic sources in $source." }
& git -C $source submodule update --init --depth 1 -- submodules/xdp-for-windows
if ($LASTEXITCODE) { throw 'Could not fetch the pinned XDP headers for MsQuic.' }

& cmake -S $source -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release -DQUIC_TLS_LIB=schannel `
    -DQUIC_BUILD_TEST=OFF -DQUIC_BUILD_TOOLS=OFF -DQUIC_BUILD_PERF=OFF `
    -DQUIC_STATIC_LINK_CRT=OFF -DQUIC_STATIC_LINK_PARTIAL_CRT=OFF `
    -DCMAKE_C_FLAGS=/wd28020 -DQUIC_SOURCE_LINK=OFF -DQUIC_PGO=OFF
if ($LASTEXITCODE) { throw 'MsQuic configuration failed.' }
& cmake --build $build --parallel 4
if ($LASTEXITCODE) { throw 'MsQuic build failed.' }
