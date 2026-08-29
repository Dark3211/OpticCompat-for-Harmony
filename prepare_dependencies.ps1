param(
    [string]$ChimeraPath = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Destination = Join-Path $ProjectRoot "third_party\lua"
$LuaHeader = Join-Path $Destination "lua.h"

function Test-Lua551([string]$HeaderPath) {
    if (-not (Test-Path $HeaderPath)) { return $false }
    $text = Get-Content -Raw -LiteralPath $HeaderPath
    return ($text -match 'LUA_VERSION_MAJOR_N\s+5') -and
           ($text -match 'LUA_VERSION_MINOR_N\s+5') -and
           ($text -match 'LUA_VERSION_RELEASE_N\s+1')
}

if ((-not $Force) -and (Test-Lua551 $LuaHeader)) {
    Write-Host "Lua 5.5.1 ya esta preparado." -ForegroundColor Green
    exit 0
}

if (Test-Path $Destination) {
    Get-ChildItem -LiteralPath $Destination -Force | Remove-Item -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

if ($ChimeraPath) {
    $Source = Join-Path $ChimeraPath "src\lua"
    if (-not (Test-Lua551 (Join-Path $Source "lua.h"))) {
        throw "La carpeta indicada no contiene src\lua de Chimera con Lua 5.5.1."
    }
    Copy-Item -Path (Join-Path $Source "*") -Destination $Destination -Recurse -Force
}
else {
    $TempRoot = Join-Path $env:TEMP ("OpticCompat_" + [Guid]::NewGuid().ToString("N"))
    $Zip = Join-Path $TempRoot "chimera-hardened.zip"
    $Extract = Join-Path $TempRoot "source"
    New-Item -ItemType Directory -Force -Path $TempRoot, $Extract | Out-Null
    try {
        $Url = "https://github.com/Dark3211/chimera/archive/refs/heads/chimera-hardened.zip"
        Write-Host "Descargando Lua 5.5.1 desde Dark3211/chimera (chimera-hardened)..."
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Zip
        Expand-Archive -LiteralPath $Zip -DestinationPath $Extract -Force
        $RepoRoot = Get-ChildItem -LiteralPath $Extract -Directory | Select-Object -First 1
        if (-not $RepoRoot) { throw "No se pudo localizar la raiz del repositorio descargado." }
        $Source = Join-Path $RepoRoot.FullName "src\lua"
        if (-not (Test-Lua551 (Join-Path $Source "lua.h"))) {
            throw "La rama descargada ya no contiene Lua 5.5.1. No se continuara para evitar ABI incorrecta."
        }
        Copy-Item -Path (Join-Path $Source "*") -Destination $Destination -Recurse -Force
    }
    finally {
        Remove-Item -LiteralPath $TempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (-not (Test-Lua551 $LuaHeader)) {
    throw "No se pudo preparar Lua 5.5.1."
}
Write-Host "OK: Lua 5.5.1 copiado a third_party\lua" -ForegroundColor Green
