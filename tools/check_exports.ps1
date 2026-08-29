param([string]$Dll = "..\Release\harmony.dll")
$ErrorActionPreference = "Stop"
if (-not (Test-Path $Dll)) { throw "No existe: $Dll" }
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) {
    Write-Host "Abre Developer PowerShell for Visual Studio y vuelve a ejecutar este script."
    exit 1
}
$output = & dumpbin.exe /exports $Dll
$output | Select-String "luaopen_mods_harmony"
if (-not ($output -match "luaopen_mods_harmony")) { throw "Falta el export luaopen_mods_harmony" }
Write-Host "OK: export luaopen_mods_harmony encontrado." -ForegroundColor Green
