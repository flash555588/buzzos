param(
    [string]$Output = "build/downloads/doom1.wad"
)

$ErrorActionPreference = "Stop"
$url = "https://www.jbserver.com/downloads/games/doom/misc/shareware/doom1.wad.zip"
$zipHash = "C1D1F430E623B5B02693A2AB42988F951FB66AE3BD3ADD06E557BDF36AF0E24F"
$wadHash = "1D7D43BE501E67D927E415E0B8F3E29C3BF33075E859721816F652A526CAC771"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $root

function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "")
        } finally {
            $sha.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

$outputPath = [IO.Path]::GetFullPath($Output)
$downloadDir = Split-Path $outputPath -Parent
New-Item -ItemType Directory -Force $downloadDir | Out-Null
$zip = Join-Path $downloadDir "doom1.wad.zip"
$unpack = Join-Path $downloadDir "doom1-wad-unpack"

if (!(Test-Path -LiteralPath $zip) -or
    (Get-Sha256 $zip) -ne $zipHash) {
    Invoke-WebRequest -UseBasicParsing $url -OutFile $zip
}
if ((Get-Sha256 $zip) -ne $zipHash) {
    throw "Doom shareware archive checksum mismatch"
}
Remove-Item -LiteralPath $unpack -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -LiteralPath $zip -DestinationPath $unpack
$wad = Join-Path $unpack "DOOM1.WAD"
if ((Get-Sha256 $wad) -ne $wadHash) {
    throw "doom1.wad checksum mismatch"
}
Copy-Item -LiteralPath $wad -Destination $outputPath -Force
Remove-Item -LiteralPath $unpack -Recurse -Force
Write-Host "Doom shareware WAD ready: $outputPath"
