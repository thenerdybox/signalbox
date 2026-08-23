<#
    Builds the plugin, runs the harness, compiles the installer and refreshes
    the staging copy - in that order, in one step.

    This exists because doing those four things by hand has already shipped a
    stale installer twice: the DLL gets rebuilt, the installer does not get
    recompiled (or the staged copy does not get replaced), and the resulting
    setup.exe quietly delivers old code that looks fine and behaves like the
    bug was never fixed. The only reliable defence is to never do these steps
    separately. Run this instead.

    The plugin hash printed at the end is the one that matters: it is the hash
    of the exact binary the installer just packaged. Read it, not the installer
    hash - Inno stamps a timestamp into every setup.exe, so two compiles of
    identical content produce different installer hashes. A changed installer
    hash proves nothing; a changed plugin hash proves the code moved.
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'RelWithDebInfo',
    [string]$BuildDir      = 'build_x64'
)

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

$iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) {
    # Inno Setup's own installer offers both locations, so check the other one.
    $iscc = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
}
if (-not (Test-Path $iscc)) { throw "Inno Setup 6 not found. Install it, or edit the path in this script." }

Write-Host '--- building plugin ---' -ForegroundColor Cyan
cmake --build $BuildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }

Write-Host '--- running harness ---' -ForegroundColor Cyan
& "$BuildDir\$Configuration\signalbox-harness.exe"
if ($LASTEXITCODE -ne 0) { throw "Harness failed (exit $LASTEXITCODE). Not building an installer around a failing build." }

$dll = "$BuildDir\rundir\$Configuration\signalbox.dll"
if (-not (Test-Path $dll)) { throw "Expected plugin binary missing: $dll" }
$dllHash = (Get-FileHash $dll -Algorithm SHA256).Hash

Write-Host '--- compiling installer ---' -ForegroundColor Cyan
& $iscc 'installer.iss'
if ($LASTEXITCODE -ne 0) { throw "Installer compile failed (exit $LASTEXITCODE)." }

# installer.iss sources [Files] straight out of the build directory, so the
# installer that just compiled necessarily contains the DLL hashed above.
$setup = Get-ChildItem 'setup\SignalBox-Setup-*.exe' | Sort-Object LastWriteTime | Select-Object -Last 1
if ($setup.LastWriteTime -lt (Get-Item $dll).LastWriteTime) {
    throw "Installer is older than the plugin it should contain. Refusing to stage it."
}

# Staging folder is named for the version that was just built, and created if
# it does not exist yet.
#
# This used to pick whichever folder sorted last and copy into that, which
# quietly staged a 0.2.0 installer into a folder called v0.1.0-scaffold and
# left the older setup.exe sitting beside it. Two installers in one folder,
# one of them stale, is exactly how someone reinstalls the build they were
# trying to replace and then concludes the fix did not work.
if ($setup.Name -match '^SignalBox-Setup-(.+)\.exe$') {
    $stagedVersion = $Matches[1]
} else {
    throw "Cannot determine a version from '$($setup.Name)' - refusing to guess where to stage it."
}

$stagingPath = Join-Path 'Builds-Staging' "v$stagedVersion"
New-Item -ItemType Directory -Force -Path $stagingPath | Out-Null

Copy-Item $setup.FullName -Destination (Join-Path $stagingPath $setup.Name) -Force
$stagedHash = (Get-FileHash (Join-Path $stagingPath $setup.Name) -Algorithm SHA256).Hash
if ($stagedHash -ne (Get-FileHash $setup.FullName -Algorithm SHA256).Hash) {
    throw "Staged copy does not match the installer that was just built."
}

# An installer for a DIFFERENT version still sitting in staging is a trap, so
# say it out loud rather than leaving it to be found by running one.
$strays = Get-ChildItem 'Builds-Staging' -Recurse -Filter 'SignalBox-Setup-*.exe' |
    Where-Object { $_.Name -ne $setup.Name }
if ($strays) {
    Write-Host ''
    Write-Host 'Older installers still staged (delete these when you no longer need them):' -ForegroundColor Yellow
    $strays | ForEach-Object { Write-Host ("  {0}" -f $_.FullName) -ForegroundColor Yellow }
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host ("  plugin    {0}  {1}" -f $dllHash.Substring(0,16), $dll)
Write-Host ("  installer {0}  {1}" -f (Get-FileHash $setup.FullName -Algorithm SHA256).Hash.Substring(0,16), $setup.FullName)
Write-Host ("  staged    {0}  {1}" -f $stagedHash.Substring(0,16), (Resolve-Path $stagingPath).Path)
