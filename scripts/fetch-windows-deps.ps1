<#
.SYNOPSIS
  Prepares everything the Windows build needs, matching an installed (or unpacked) OBS Studio:
  the libobs / obs-frontend-api headers, the FFmpeg headers and import libraries for the DLLs OBS ships.

.DESCRIPTION
  Output layout (pass -DNIRIUN_DEPS_DIR=<OutDir> to CMake):
    <OutDir>/obs-studio/libobs, <OutDir>/obs-studio/frontend/api   headers (git sparse checkout, OBS tag)
    <OutDir>/ffmpeg/libavcodec, libavutil                          headers (git sparse checkout, FFmpeg tag)
    <OutDir>/generated/obsconfig.h, libavutil/avconfig.h           files normally produced by each project's build
    <OutDir>/lib/obs.lib obs-frontend-api.lib avcodec.lib avutil.lib   import libraries made from the DLLs

  Needs git and Visual Studio 2022 (dumpbin.exe and lib.exe).

.EXAMPLE
  scripts\fetch-windows-deps.ps1
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNIRIUN_DEPS_DIR="$PWD\deps"
#>
param(
    [string]$ObsDir = "C:\Program Files\obs-studio",
    [string]$OutDir = (Join-Path $PSScriptRoot "..\deps"),
    [string]$ObsTag = "",
    [string]$FfmpegTag = ""
)

$ErrorActionPreference = "Stop"
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$bin = Join-Path $ObsDir "bin\64bit"
if (-not (Test-Path (Join-Path $bin "obs.dll"))) { throw "obs.dll not found in $bin. Pass -ObsDir." }

New-Item -ItemType Directory -Force -Path $OutDir, "$OutDir\lib", "$OutDir\generated\libavutil" | Out-Null

# ---- versions -------------------------------------------------------------------------------------------
if (-not $ObsTag) { $ObsTag = (Get-Item (Join-Path $bin "obs64.exe")).VersionInfo.ProductVersion.Split("-")[0] }
$avcodec = Get-ChildItem $bin -Filter "avcodec-*.dll" | Select-Object -First 1
if (-not $avcodec) { throw "avcodec-*.dll not found in $bin" }
$avVersion = [version]$avcodec.VersionInfo.FileVersion
Write-Host "OBS $ObsTag, $($avcodec.Name) $avVersion"

if (-not $FfmpegTag) {
    # Newest FFmpeg release whose libavcodec is not newer than the one OBS ships (headers must not be ahead of the DLL).
    foreach ($tag in @("n8.1.1", "n8.1", "n8.0.1", "n8.0", "n7.1.1", "n7.1", "n7.0")) {
        $base = "https://raw.githubusercontent.com/FFmpeg/FFmpeg/$tag/libavcodec"
        $hMajor = (Invoke-WebRequest -UseBasicParsing "$base/version_major.h").Content
        $hMinor = (Invoke-WebRequest -UseBasicParsing "$base/version.h").Content
        $maj = [int]([regex]::Match($hMajor, "LIBAVCODEC_VERSION_MAJOR\s+(\d+)").Groups[1].Value)
        $min = [int]([regex]::Match($hMinor, "LIBAVCODEC_VERSION_MINOR\s+(\d+)").Groups[1].Value)
        if ($maj -eq $avVersion.Major -and $min -le $avVersion.Minor) { $FfmpegTag = $tag; break }
    }
    if (-not $FfmpegTag) { throw "No FFmpeg tag matches libavcodec $avVersion; pass -FfmpegTag." }
}
Write-Host "FFmpeg headers: $FfmpegTag"

# ---- headers --------------------------------------------------------------------------------------------
function Sparse-Clone($url, $tag, $dest, $paths) {
    if (Test-Path "$dest\.git") { Write-Host "$dest exists, keeping it"; return }
    git clone --quiet --depth 1 --branch $tag --filter=blob:none --sparse $url $dest
    git -C $dest sparse-checkout set @paths
}
Sparse-Clone "https://github.com/obsproject/obs-studio.git" $ObsTag "$OutDir\obs-studio" @("libobs", "frontend/api")
Sparse-Clone "https://github.com/FFmpeg/FFmpeg.git" $FfmpegTag "$OutDir\ffmpeg" @("libavcodec", "libavutil")

Set-Content -Encoding ascii "$OutDir\generated\libavutil\avconfig.h" @"
#ifndef AVUTIL_AVCONFIG_H
#define AVUTIL_AVCONFIG_H
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
#endif
"@
Set-Content -Encoding ascii "$OutDir\generated\obsconfig.h" @"
#pragma once
#define OBS_VERSION "$ObsTag"
#define OBS_DATA_PATH "../../data"
#define OBS_INSTALL_PREFIX ""
#define OBS_PLUGIN_PATH "../../obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"
"@

# ---- import libraries -----------------------------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ tools was not found." }
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" | Sort-Object Name | Select-Object -Last 1
$tools = Join-Path $msvc.FullName "bin\Hostx64\x64"
$dumpbin = Join-Path $tools "dumpbin.exe"
$libexe = Join-Path $tools "lib.exe"

function Make-ImportLib($dll, $name) {
    $def = "$OutDir\lib\$name.def"
    $names = & $dumpbin /nologo /exports $dll | ForEach-Object {
        if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $Matches[1] }
    }
    if (-not $names) { throw "No exports found in $dll" }
    $lines = @("LIBRARY $([System.IO.Path]::GetFileName($dll))", "EXPORTS") + $names
    Set-Content -Encoding ascii $def $lines
    & $libexe /nologo /machine:x64 "/def:$def" "/out:$OutDir\lib\$name.lib" | Out-Null
    Write-Host ("{0}.lib: {1} exports" -f $name, $names.Count)
}

Make-ImportLib (Join-Path $bin "obs.dll") "obs"
Make-ImportLib (Join-Path $bin "obs-frontend-api.dll") "obs-frontend-api"
Make-ImportLib $avcodec.FullName "avcodec"
$avutil = Get-ChildItem $bin -Filter "avutil-*.dll" | Select-Object -First 1
Make-ImportLib $avutil.FullName "avutil"

Write-Host "Done. Configure with: cmake -S . -B build -G `"Visual Studio 17 2022`" -A x64 -DNIRIUN_DEPS_DIR=`"$OutDir`""
