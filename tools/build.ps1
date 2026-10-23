param(
    [ValidateSet('build', 'flash', 'monitor', 'flash-monitor', 'menuconfig')]
    [string]$Action = 'build',
    [string]$Port = '',
    [string]$IdfPath = '',
    [string]$ToolsPath = 'C:\Espressif'
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path $PSScriptRoot -Parent

if (-not $IdfPath) {
    $localIdf = Join-Path $projectRoot '.tools\esp-idf'
    $IdfPath = if (Test-Path -LiteralPath $localIdf) { 
        $localIdf 
        } else { 
            'C:\esp\v6.1\esp-idf' 
        }
}

if (-not (Test-Path -LiteralPath (Join-Path $IdfPath 'tools\idf.py'))) {
    throw "ESP-IDF was not found at $IdfPath."
}

$idfVersion = if ($IdfPath -match '[\\/]v(\d+\.\d+)[\\/]') { $Matches[1] } else { '6.1' }
$pythonEnv = Join-Path $ToolsPath "tools\python\v$idfVersion\venv"
$pythonExe = Join-Path $pythonEnv 'Scripts\python.exe'
if (-not (Test-Path -LiteralPath $pythonExe)) {
    throw "ESP-IDF's Python environment was not found at $pythonEnv. Run this project from an ESP-IDF terminal instead."
}
$env:IDF_PATH = $IdfPath
$env:PYTHONUTF8 = '1'
$env:IDF_TOOLS_PATH = $ToolsPath
$env:IDF_PYTHON_ENV_PATH = $pythonEnv
$env:PATH = "$(Join-Path $pythonEnv 'Scripts');$env:PATH"
. (Join-Path $IdfPath 'export.ps1')
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF environment activation failed.' }
Push-Location $projectRoot
try {
    if ($Action -ne 'monitor' -and -not (Test-Path -LiteralPath 'build\build.ninja')) {
        & $pythonExe (Join-Path $IdfPath 'tools\idf.py') reconfigure
        if ($LASTEXITCODE -ne 0) { throw 'Initial project configuration failed.' }
    }
    # Timestamps problems when compiling with a vpn
    $now = [DateTime]::UtcNow
    if (Test-Path -LiteralPath 'managed_components') {
        Get-ChildItem -LiteralPath 'managed_components' -File -Recurse |
            Where-Object { $_.LastWriteTimeUtc -gt $now } |
            ForEach-Object { $_.LastWriteTimeUtc = $now }
    }
    $idfArgs = @()
    if ($Port) { $idfArgs += @('-p', $Port) }
    switch ($Action) {
        'flash-monitor' { $idfArgs += @('flash', 'monitor') }
        default { $idfArgs += $Action }
    }
    & $pythonExe (Join-Path $IdfPath 'tools\idf.py') @idfArgs
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF $Action failed with exit code $LASTEXITCODE." }
} finally { Pop-Location }
