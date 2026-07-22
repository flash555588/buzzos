# Shared QEMU path / accelerator selection for BuzzOS host scripts.
# Prefer MSYS2 mingw64 qemu-system-x86_64 with WHPX; fall back to TCG / i386.

function Resolve-BuzzosQemu {
    param(
        [string]$Preferred = ""
    )

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($Preferred)) {
        $candidates += $Preferred.Trim('"')
    }
    if (-not [string]::IsNullOrWhiteSpace($env:QEMU)) {
        $candidates += $env:QEMU.Trim('"')
    }
    $candidates += @(
        "C:\msys64\mingw64\bin\qemu-system-x86_64.exe",
        "C:\msys64\ucrt64\bin\qemu-system-x86_64.exe",
        "C:\msys64\clang64\bin\qemu-system-x86_64.exe",
        "D:\msys64\mingw64\bin\qemu-system-x86_64.exe",
        "D:\msys64\ucrt64\bin\qemu-system-x86_64.exe",
        "C:\Program Files\qemu\qemu-system-x86_64.exe",
        "D:\Program Files\qemu\qemu-system-x86_64.exe",
        "qemu-system-x86_64",
        "C:\Program Files\qemu\qemu-system-i386.exe",
        "D:\Program Files\qemu\qemu-system-i386.exe",
        "qemu-system-i386"
    )

    $path = $null
    foreach ($c in $candidates) {
        if ([string]::IsNullOrWhiteSpace($c)) { continue }
        $c = $c.Trim().Trim('"')
        if (Test-Path -LiteralPath $c) {
            $path = (Resolve-Path -LiteralPath $c).Path
            break
        }
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source) {
            $path = $cmd.Source
            break
        }
    }

    if (-not $path) {
        throw "QEMU not found. Install mingw-w64-x86_64-qemu (MSYS2) or set QEMU=..."
    }

    $accel = "tcg,tb-size=512"
    # Prefer a conservative model: "-cpu max" under WHPX on recent Intel
    # (APX/MPX) often aborts with "Unexpected VP exit code 4".
    $cpu = "qemu64"
    $name = [IO.Path]::GetFileNameWithoutExtension($path)
    if ($name -match "x86_64") {
        try {
            $help = & $path -accel help 2>&1 | Out-String
            if ($help -match "(?m)^whpx\s*$" -or $help -match "(?m)^whpx\b") {
                $accel = "whpx"
                $cpu = "qemu64"
            }
        } catch {
            # Keep TCG fallback.
        }
    } else {
        $cpu = "max"
    }

    [pscustomobject]@{
        Path  = $path
        Accel = $accel
        Cpu   = $cpu
    }
}
