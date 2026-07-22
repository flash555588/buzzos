param(
    [string]$Destination = "third_party/netsurf-reference"
)

$ErrorActionPreference = "Stop"
$revision = "a471a0d44274ec57fee5e5f30ae59fbd2ad02656"
$repository = "git://git.netsurf-browser.org/netsurf.git"

if (Test-Path -LiteralPath $Destination) {
    $current = (& git -C $Destination rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $current -ne $revision) {
        throw "$Destination exists but is not the pinned NetSurf revision $revision"
    }
    Write-Host "NetSurf reference already present at $revision"
} else {
    & git clone --no-checkout $repository $Destination
    if ($LASTEXITCODE -ne 0) {
        throw "Could not clone NetSurf"
    }
    & git -C $Destination checkout --detach $revision
    if ($LASTEXITCODE -ne 0) {
        throw "Could not check out pinned NetSurf revision"
    }
    Write-Host "NetSurf reference ready at $revision"
}

$workspace = "third_party/netsurf-workspace"
$libraries = [ordered]@{
    buildsystem = "771d7cb0691831aa5962855015b2f25ec527a9ee"
    libparserutils = "6b0cbf086ca8eb8fe74b69f0c9ecf274eb2397ca"
    libwapcaplet = "c7c128d3eb3223b216c974471f82e9337fbcf4ba"
    libhubbub = "6651b8cf87a4aa87bcdb2ff024a02659cd3f9402"
    libdom = "f69781e1f062444b5af3f62d431d7d94018da53b"
    libcss = "104d87fde48b9e022cd3cdad28aeb4d8cc0a0c5a"
    libnsutils = "0bd39060740b6163bd50875326654a722df97eb2"
    libnsfb = "b701cdce7241c3747ccd78658a365db0983ebe24"
}
New-Item -ItemType Directory -Force $workspace | Out-Null
foreach ($name in $libraries.Keys) {
    $path = Join-Path $workspace $name
    if (!(Test-Path -LiteralPath $path)) {
        & git clone --no-checkout "git://git.netsurf-browser.org/$name.git" $path
        if ($LASTEXITCODE -ne 0) { throw "Could not clone $name" }
        & git -C $path checkout --detach $libraries[$name]
        if ($LASTEXITCODE -ne 0) { throw "Could not check out $name" }
    }
    $current = (& git -C $path rev-parse HEAD).Trim()
    if ($current -ne $libraries[$name]) {
        throw "$name is at $current, expected $($libraries[$name])"
    }
}

$zlibPath = Join-Path $workspace "zlib"
$zlibRevision = "51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf"
if (!(Test-Path -LiteralPath $zlibPath)) {
    & git clone --no-checkout https://github.com/madler/zlib.git $zlibPath
    if ($LASTEXITCODE -ne 0) { throw "Could not clone zlib" }
    & git -C $zlibPath checkout --detach $zlibRevision
    if ($LASTEXITCODE -ne 0) { throw "Could not check out zlib" }
}
$zlibCurrent = (& git -C $zlibPath rev-parse HEAD).Trim()
if ($zlibCurrent -ne $zlibRevision) {
    throw "zlib is at $zlibCurrent, expected $zlibRevision"
}
Write-Host "NetSurf core libraries ready"
