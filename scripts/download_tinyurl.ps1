$Url     = 'https://github.com/scavengerDeeluxe/ResetBootDevice/raw/refs/heads/master/scripts/ResetBootDevice.zip'
$WorkDir = 'X:\ResetBootDevice'
$ZipFile = Join-Path $WorkDir 'ResetBootDevice.zip'
$Extract = Join-Path $WorkDir 'Extracted'

$ErrorActionPreference = 'Stop'

if (Test-Path $WorkDir) {
    Remove-Item $WorkDir -Recurse -Force
}

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
New-Item -ItemType Directory -Path $Extract -Force | Out-Null

Write-Host 'Downloading ResetBootDevice...'

try {
    Invoke-WebRequest -Uri $Url -OutFile $ZipFile -UseBasicParsing
}
catch {
    throw "Download failed: $($_.Exception.Message)"
}

Write-Host 'Extracting...'

Add-Type -AssemblyName System.IO.Compression.FileSystem

[System.IO.Compression.ZipFile]::ExtractToDirectory(
    $ZipFile,
    $Extract
)

$Exe = Get-ChildItem -Path $Extract -Filter 'ResetBootDevice.exe' -File -Recurse |
    Select-Object -First 1

if (-not $Exe) {
    throw 'ResetBootDevice.exe was not found in the extracted archive.'
}

Write-Host "Running: $($Exe.FullName)"

Push-Location $Exe.DirectoryName

try {
    & $Exe.FullName
    $ExitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}

Write-Host "ResetBootDevice.exe exited with code $ExitCode"

exit $ExitCode
