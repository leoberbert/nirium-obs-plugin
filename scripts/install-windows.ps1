<#
.SYNOPSIS
  Installs niriun-obs.dll into OBS Studio on Windows.

.PARAMETER Scope
  User     (default) %APPDATA%\obs-studio\plugins\niriun-obs; no administrator rights needed.
  Machine  <ObsDir>, run from an elevated PowerShell (not needed for a portable OBS you can write to). The layout follows the
           OBS version: OBS 33+ (has a "core" folder) gets plugins\niriun-obs\; older gets obs-plugins\64bit + data\obs-plugins.

.PARAMETER Dll
  The built plugin. Default: build\RelWithDebInfo\niriun-obs.dll next to this repository's root.

.EXAMPLE
  scripts\install-windows.ps1
  scripts\install-windows.ps1 -Scope Machine
#>
param(
    [ValidateSet("User", "Machine")][string]$Scope = "User",
    [string]$Dll = (Join-Path $PSScriptRoot "..\build\RelWithDebInfo\niriun-obs.dll"),
    [string]$ObsDir = "C:\Program Files\obs-studio"
)

$ErrorActionPreference = "Stop"
$Dll = (Resolve-Path $Dll).Path
$locale = (Resolve-Path (Join-Path $PSScriptRoot "..\data\locale")).Path

if ($Scope -eq "User") {
    $root = Join-Path $env:APPDATA "obs-studio\plugins\niriun-obs"
    $binDir = Join-Path $root "bin\64bit"
    $dataDir = Join-Path $root "data\locale"
} else {
    $oldBin = Join-Path $ObsDir "obs-plugins\64bit"
    $oldData = Join-Path $ObsDir "data\obs-plugins\niriun-obs"
    $newRoot = Join-Path $ObsDir "plugins\niriun-obs"
    if (Test-Path (Join-Path $ObsDir "core")) {
        $binDir = $newRoot
        $dataDir = Join-Path $newRoot "data\locale"
        # An OBS 32 copy left behind would make the plugin load twice.
        Remove-Item (Join-Path $oldBin "niriun-obs.dll") -Force -ErrorAction SilentlyContinue
        Remove-Item $oldData -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        $binDir = $oldBin
        $dataDir = Join-Path $oldData "locale"
        Remove-Item $newRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

New-Item -ItemType Directory -Force -Path $binDir, $dataDir | Out-Null
Copy-Item $Dll $binDir -Force
Copy-Item (Join-Path $locale "*.ini") $dataDir -Force
Write-Host "Installed to $binDir. Restart OBS Studio, then Sources > + > Niriun."
