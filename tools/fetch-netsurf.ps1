param(
    [string]$Destination = "third_party/netsurf-reference"
)

$ErrorActionPreference = "Stop"
$revision = "a471a0d44274ec57fee5e5f30ae59fbd2ad02656"
$repository = "git://git.netsurf-browser.org/netsurf.git"
$repoRoot = Split-Path -Parent $PSScriptRoot
$patchDir = Join-Path $repoRoot "third_party/netsurf-patches"

function Ensure-GitCheckout {
    param(
        [string]$Path,
        [string]$Repository,
        [string]$Revision,
        [string]$Label
    )

    if (Test-Path -LiteralPath $Path) {
        $current = (& git -C $Path rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0) {
            throw "$Label at $Path is not a git checkout"
        }
        if ($current -ne $Revision) {
            Write-Host "Updating $Label from $current to $Revision"
            & git -C $Path fetch --all
            if ($LASTEXITCODE -ne 0) { throw "Could not fetch $Label" }
            & git -C $Path checkout --detach $Revision
            if ($LASTEXITCODE -ne 0) { throw "Could not check out $Label revision $Revision" }
        }
    } else {
        & git clone --no-checkout $Repository $Path
        if ($LASTEXITCODE -ne 0) {
            throw "Could not clone $Label"
        }
        & git -C $Path checkout --detach $Revision
        if ($LASTEXITCODE -ne 0) {
            throw "Could not check out $Label revision $Revision"
        }
    }

    # Drop local edits/line-ending noise so the pin + patch series is reproducible.
    & git -C $Path reset --hard $Revision
    if ($LASTEXITCODE -ne 0) { throw "Could not reset $Label to $Revision" }
    & git -C $Path clean -fd
    if ($LASTEXITCODE -ne 0) { throw "Could not clean $Label" }
}

function Apply-NetSurfPatches {
    param([string]$Path)

    if (!(Test-Path -LiteralPath $patchDir)) {
        Write-Host "No NetSurf patches directory at $patchDir"
        return
    }

    $patches = Get-ChildItem -LiteralPath $patchDir -Filter "*.patch" |
        Sort-Object Name
    foreach ($patch in $patches) {
        Write-Host "Applying $($patch.Name)"
        & git -C $Path apply --whitespace=nowarn -- $patch.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "Could not apply NetSurf patch $($patch.Name)"
        }
    }
}

Ensure-GitCheckout -Path $Destination -Repository $repository `
    -Revision $revision -Label "NetSurf reference"
Apply-NetSurfPatches -Path $Destination
Write-Host "NetSurf reference ready at $revision (with BuzzOS patches)"

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
    Ensure-GitCheckout -Path $path `
        -Repository "git://git.netsurf-browser.org/$name.git" `
        -Revision $libraries[$name] -Label $name
}

$zlibPath = Join-Path $workspace "zlib"
$zlibRevision = "51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf"
Ensure-GitCheckout -Path $zlibPath `
    -Repository "https://github.com/madler/zlib.git" `
    -Revision $zlibRevision -Label "zlib"
Write-Host "NetSurf core libraries ready"
