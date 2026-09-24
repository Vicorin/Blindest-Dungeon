# Builds DarkestAccess.exe (the one-click launcher) using MSVC.

param(
    [string]$Version = "0.0.0"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found: $vswhere" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC C++ tools not found. Is the VCTools workload installed?" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

New-Item -ItemType Directory -Force "$root\build" | Out-Null

# ---- Generate the version resource ----
$nums = @(0, 0, 0, 0)
$split = $Version -split '\.'
for ($i = 0; $i -lt [Math]::Min(4, $split.Count); $i++) {
    $n = 0; [void][int]::TryParse($split[$i], [ref]$n); $nums[$i] = $n
}
$fileVer = "$($nums[0]),$($nums[1]),$($nums[2]),$($nums[3])"
# ASCII .rc (rc.exe, like PowerShell 5.1, is unhappy with stray non-ASCII bytes).
$rc = @"
#include <winver.h>
VS_VERSION_INFO VERSIONINFO
 FILEVERSION $fileVer
 PRODUCTVERSION $fileVer
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS 0x40004L
 FILETYPE 0x1L
 FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "Blindest Dungeon"
            VALUE "FileDescription", "Darkest Dungeon screen-reader accessibility launcher"
            VALUE "FileVersion", "$Version"
            VALUE "InternalName", "DarkestAccess"
            VALUE "OriginalFilename", "DarkestAccess.exe"
            VALUE "ProductName", "Blindest Dungeon"
            VALUE "ProductVersion", "$Version"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
"@
Set-Content -Path "$root\build\version.rc" -Value $rc -Encoding ascii

$bat = @"
@echo off
call "$vcvars" >nul 2>nul
cd /d "$root\build"
rc /nologo /fo version.res version.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /I "$root\..\Mod\lib\prism" ^
   /Fe:DarkestAccess.exe "$root\launcher.cpp" version.res ^
   /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib winhttp.lib version.lib advapi32.lib
if errorlevel 1 exit /b 1
"@
$batPath = Join-Path $env:TEMP "dd_launcher_build.bat"
Set-Content -Path $batPath -Value $bat -Encoding ascii
cmd /c "`"$batPath`""
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }
Write-Host "`n[+] Build OK: $root\build\DarkestAccess.exe" -ForegroundColor Green
Get-ChildItem "$root\build\DarkestAccess.exe" | Select-Object Name,Length
