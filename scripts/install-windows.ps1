<#
.SYNOPSIS
  Installs niriun-obs.dll into OBS Studio on Windows.

.PARAMETER Scope
  User     (default) %APPDATA%\obs-studio\plugins\niriun-obs; no administrator rights needed.
  Machine  <ObsDir>\obs-plugins\64bit; run from an elevated PowerShell.

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
    $binDir = Join-Path $ObsDir "obs-plugins\64bit"
    $dataDir = Join-Path $ObsDir "data\obs-plugins\niriun-obs\locale"
}

New-Item -ItemType Directory -Force -Path $binDir, $dataDir | Out-Null
Copy-Item $Dll $binDir -Force
Copy-Item (Join-Path $locale "*.ini") $dataDir -Force
Write-Host "Installed to $binDir. Restart OBS Studio, then Sources > + > Niriun."
