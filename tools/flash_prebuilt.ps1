param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$Python = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
)
$ErrorActionPreference = 'Stop'
$image = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\keychain-full.bin'
if (-not (Test-Path -LiteralPath $image)) { throw "Firmware image is missing: $image" }
if (-not (Test-Path -LiteralPath $Python)) { throw 'Pass -Python with an executable that has esptool installed.' }
$contents = [IO.File]::ReadAllBytes($image)
if ($contents.Length -le 0x10000) { throw 'The merged firmware image is incomplete.' }
$bootFile = [IO.Path]::GetTempFileName()
$appFile = [IO.Path]::GetTempFileName()
try {
    $bootBytes = New-Object byte[] 0x9000
    [Array]::Copy($contents, 0, $bootBytes, 0, $bootBytes.Length)
    $appBytes = New-Object byte[] ($contents.Length - 0x10000)
    [Array]::Copy($contents, 0x10000, $appBytes, 0, $appBytes.Length)
    [IO.File]::WriteAllBytes($bootFile, $bootBytes)
    [IO.File]::WriteAllBytes($appFile, $appBytes)
    & $Python -m esptool --chip esp32s3 --port $Port --baud 460800 --after watchdog-reset write-flash 0x0 $bootFile 0x10000 $appFile
    if ($LASTEXITCODE -ne 0) { throw 'Flashing failed. Check the port, USB cable, power switch, and BOOT/RESET sequence.' }
} finally {
    Remove-Item -LiteralPath $bootFile -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $appFile -Force -ErrorAction SilentlyContinue
}
Write-Output 'Flashing finished. Press RESET once if the application does not start.'
