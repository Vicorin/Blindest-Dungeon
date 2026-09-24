# Builds BlindestDungeonInstaller.exe (the install / update / repair / uninstall manager).

param([switch]$Trace)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found: $vswhere" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC C++ tools not found. Is the VCTools workload installed?" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

New-Item -ItemType Directory -Force "$root\build" | Out-Null

$traceDef = ""
if ($Trace) { $traceDef = "/DBDI_TRACE " }

$bat = @"
@echo off
call "$vcvars" >nul 2>nul
cd /d "$root\build"
cl /nologo /O2 /MT /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE $traceDef^
   /Fe:BlindestDungeonInstaller.exe "$root\installer.cpp" ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED ^
   /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
   winhttp.lib advapi32.lib shell32.lib user32.lib gdi32.lib ole32.lib oleaut32.lib shlwapi.lib
if errorlevel 1 exit /b 1
"@
$batPath = Join-Path $env:TEMP "dd_installer_build.bat"
Set-Content -Path $batPath -Value $bat -Encoding ascii
cmd /c "`"$batPath`""
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }

# Prove the elevation manifest is inside the exe, not merely requested.
$exe = "$root\build\BlindestDungeonInstaller.exe"
$text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($exe))
if (-not $text.Contains("requireAdministrator")) {
    throw "The built exe has NO embedded requireAdministrator manifest -- it would fail under Program Files without a UAC prompt."
}

Write-Host "`n[+] Build OK (embedded requireAdministrator manifest verified): $exe" -ForegroundColor Green
Get-ChildItem $exe | Select-Object Name,Length
