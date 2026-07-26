param(
    [string]$Image = "build/buzzos.img",
    [Alias("Qemu")]
    [string]$QemuPath = "",
    [string]$SerialLog = "build/serial-gui-smoke.log",
    [string]$TestImage = "build/buzzos-gui-test.img",
    [string]$OutDir = "build/gui-smoke",
    [string]$PythonPath = "",
    [ValidateSet("none", "dsound", "sdl", "wav")]
    [string]$AudioDriver = "none",
    [ValidateSet("std", "virtio")]
    [string]$Graphics = "std",
    [int]$DisplayWidth = 1600,
    [int]$DisplayHeight = 900,
    [ValidateRange(0, 9)]
    [int]$DisplayModeKey = 0,
    [int]$TimeoutSeconds = 45
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "Resolve-BuzzosQemu.ps1")
$qemuInfo = Resolve-BuzzosQemu -Preferred $QemuPath
$QemuPath = $qemuInfo.Path
$QemuAccel = $qemuInfo.Accel
$QemuCpu = $qemuInfo.Cpu

if ([string]::IsNullOrWhiteSpace($PythonPath)) {
    $PythonPath = $env:PYTHON
}
if ([string]::IsNullOrWhiteSpace($PythonPath)) {
    $PythonPath = "python"
}

function Get-FreeTcpPort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}

function Read-SerialLog {
    if (!(Test-Path -LiteralPath $SerialLog)) {
        return ""
    }
    try {
        return Get-Content -LiteralPath $SerialLog -Raw -ErrorAction Stop
    } catch {
        return ""
    }
}

function Fail-WithLog([string]$Message) {
    $log = Read-SerialLog
    $tailStart = [Math]::Max(0, $log.Length - [Math]::Min($log.Length, 3000))
    throw "$Message`n$($log.Substring($tailStart))"
}

function Wait-ForLog([string]$Pattern, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        if ($script:qemuProcess -and $script:qemuProcess.HasExited) {
            Fail-WithLog "QEMU exited early with code $($script:qemuProcess.ExitCode)."
        }
        Start-Sleep -Milliseconds 300
        $log = Read-SerialLog
    } until ($log -match $Pattern -or (Get-Date) -gt $deadline)

    if ($log -notmatch $Pattern) {
        Fail-WithLog "Timed out waiting for serial output: $Pattern"
    }
}

function Send-Hmp([string]$Line) {
    $script:writer.WriteLine($Line)
    Start-Sleep -Milliseconds 65
}

function Key-Name([char]$Ch) {
    switch ($Ch) {
        " " { return "spc" }
        "/" { return "slash" }
        "-" { return "minus" }
        "." { return "dot" }
        "@" { return "shift-2" }
        default { return [string]$Ch }
    }
}

function Send-Key([string]$Name) {
    Send-Hmp ("sendkey " + $Name)
}

function Capture-Screen([string]$Name, [string]$PpmPath, [string]$PngPath) {
    $lastError = ""
    for ($attempt = 1; $attempt -le 4; $attempt++) {
        Remove-Item -LiteralPath $PpmPath -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $PngPath -ErrorAction SilentlyContinue

        if ($attempt -gt 1) {
            Start-Sleep -Milliseconds 350
        }
        Send-Hmp ("screendump " + ($PpmPath -replace "\\", "/"))
        $deadline = (Get-Date).AddSeconds(5)
        $item = $null
        do {
            Start-Sleep -Milliseconds 100
            $item = Get-Item -LiteralPath $PpmPath -ErrorAction SilentlyContinue
        } until (($item -and $item.Length -gt 1000) -or (Get-Date) -gt $deadline)

        if (!($item -and $item.Length -gt 1000)) {
            $lastError = "missing screenshot file"
            continue
        }

        Start-Sleep -Milliseconds 250
        try {
            Convert-And-Assert-Ppm $PpmPath $PngPath $Name -Quiet
            return @{ Name = $Name; Ppm = $PpmPath; Png = $PngPath }
        } catch {
            $lastError = $_.Exception.Message
        }
    }

    Fail-WithLog "Could not capture a valid $Name screenshot after retries: $lastError"
}

function Type-Text([string]$Text) {
    foreach ($ch in $Text.ToCharArray()) {
        Send-Key (Key-Name $ch)
    }
}

function Type-Command([string]$Text) {
    Type-Text $Text
    Send-Key "ret"
    Start-Sleep -Milliseconds 1000
    if ($Text -eq "gui" -and $DisplayModeKey -gt 0) {
        Set-GuiDisplayMode
    }
}

function Press-Many([string]$Key, [int]$Count) {
    for ($i = 0; $i -lt $Count; $i++) {
        Send-Key $Key
    }
}

function Get-PpmDimensions([string]$PpmPath) {
    $reader = [IO.File]::OpenText((Resolve-Path -LiteralPath $PpmPath))
    try {
        if ($reader.ReadLine() -ne "P6") {
            throw "Unexpected PPM format: $PpmPath"
        }
        $sizeLine = $reader.ReadLine()
        while ($sizeLine -and $sizeLine.StartsWith("#")) {
            $sizeLine = $reader.ReadLine()
        }
        $parts = $sizeLine -split "\s+"
        if ($parts.Count -lt 2) {
            throw "Missing PPM dimensions: $PpmPath"
        }
        return @{
            Width = [int]$parts[0]
            Height = [int]$parts[1]
        }
    } finally {
        $reader.Dispose()
    }
}

function Set-GuiDisplayMode {
    $probePpm = (Join-Path $OutDir "resolution-probe.ppm")
    $probe = Capture-Screen "resolution-probe" $probePpm (Join-Path $OutDir "resolution-probe.png")
    $probeSize = Get-PpmDimensions $probe.Ppm
    if ($probeSize.Width -eq $DisplayWidth -and
        $probeSize.Height -eq $DisplayHeight) {
        return
    }
    # With no external apps, System is 26 px right of taskbar center.
    Move-MouseRelative -2000 -2000
    Move-MouseRelative ([int]($probeSize.Width / 2) + 26) ($probeSize.Height - 44)
    Click-Left
    Send-Key ([string]$DisplayModeKey)
    Start-Sleep -Milliseconds 1200
    # System remains active after the mode switch. Clicking its task tile
    # minimizes it and returns focus to the launcher at the new size.
    Move-MouseRelative -2000 -2000
    Move-MouseRelative ([int]($DisplayWidth / 2) + 26) ($DisplayHeight - 44)
    Click-Left
    Start-Sleep -Milliseconds 500
    Move-MouseRelative -2000 -2000
    Move-MouseRelative ([int]($DisplayWidth / 2)) ([int]($DisplayHeight / 2))
}

function Move-MouseRelative([int]$Dx, [int]$Dy) {
    while ($Dx -ne 0 -or $Dy -ne 0) {
        $sx = [Math]::Max(-32, [Math]::Min(32, $Dx))
        $sy = [Math]::Max(-32, [Math]::Min(32, $Dy))
        Send-Hmp "mouse_move $sx $sy"
        $Dx -= $sx
        $Dy -= $sy
    }
}

function Click-Left {
    Send-Hmp "mouse_button 1"
    Send-Hmp "mouse_button 0"
}

function Stop-QemuIfRunning {
    if (!$script:qemuProcess) {
        return
    }
    try {
        $script:qemuProcess.Refresh()
        if (!$script:qemuProcess.HasExited) {
            $script:qemuProcess.Kill()
            $script:qemuProcess.WaitForExit(5000) | Out-Null
        }
    } catch {
        # Cleanup should not hide the real GUI smoke result.
    }
}

function Convert-And-Assert-Ppm([string]$PpmPath, [string]$PngPath, [string]$Name, [switch]$Quiet) {
    $python = @'
import struct, sys, zlib

ppm, png, name = sys.argv[1], sys.argv[2], sys.argv[3]

with open(ppm, "rb") as f:
    def token():
        out = bytearray()
        while True:
            c = f.read(1)
            if not c:
                raise SystemExit(f"{name}: truncated ppm header")
            if c == b"#":
                f.readline()
                continue
            if c not in b" \t\r\n":
                out.extend(c)
                break
        while True:
            c = f.read(1)
            if not c or c in b" \t\r\n":
                break
            out.extend(c)
        return bytes(out)

    if token() != b"P6":
        raise SystemExit(f"{name}: not a binary PPM")
    w = int(token())
    h = int(token())
    maxv = int(token())
    if maxv != 255:
        raise SystemExit(f"{name}: unsupported max value {maxv}")
    data = f.read(w * h * 3)

if w < 300 or h < 180 or len(data) != w * h * 3:
    raise SystemExit(f"{name}: unexpected frame size {w}x{h}")

step = max(1, (w * h) // 4096)
unique = set()
nonblack = 0
for i in range(0, w * h, step):
    rgb = data[i * 3:i * 3 + 3]
    unique.add(rgb)
    if rgb != b"\x00\x00\x00":
        nonblack += 1

if len(unique) < 8 or nonblack < 64:
    raise SystemExit(f"{name}: frame looks blank or too uniform (unique={len(unique)}, nonblack={nonblack})")

raw = b"".join(b"\x00" + data[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)

with open(png, "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n")
    f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
    f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
    f.write(chunk(b"IEND", b""))

print(f"{name}: {w}x{h}, unique={len(unique)}, nonblack={nonblack}")
'@
    $output = $python | & $PythonPath - $PpmPath $PngPath $Name 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Image validation failed for $Name`n$output"
    }
    if (!$Quiet) {
        $output | ForEach-Object { Write-Host $_ }
    }
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
Copy-Item -LiteralPath $Image -Destination $TestImage -Force
Remove-Item -LiteralPath $SerialLog -ErrorAction SilentlyContinue
Remove-Item -Path (Join-Path $OutDir "*.ppm") -ErrorAction SilentlyContinue
Remove-Item -Path (Join-Path $OutDir "*.png") -ErrorAction SilentlyContinue

$monitorPort = Get-FreeTcpPort
$qemuArgs = @(
    "-accel", $QemuAccel,
    "-cpu", $QemuCpu,
    "-m", "256",
    "-drive", "format=raw,file=$TestImage",
    "-serial", "file:$SerialLog",
    "-display", "none",
    "-monitor", "tcp:127.0.0.1:$monitorPort,server,nowait",
    "-no-reboot",
    "-audiodev", "$AudioDriver,id=audio0",
    "-device", "AC97,audiodev=audio0",
    "-netdev", "user,id=n0",
    "-device", "ne2k_isa,netdev=n0,iobase=0x300,irq=10"
)
if ($Graphics -eq "virtio") {
    $qemuArgs += @(
        "-vga", "none",
        "-device", "virtio-vga,xres=$DisplayWidth,yres=$DisplayHeight"
    )
} else {
    $qemuArgs += @("-vga", "std")
}

$script:qemuProcess = Start-Process -FilePath $QemuPath -ArgumentList $qemuArgs -WorkingDirectory (Get-Location) -PassThru -WindowStyle Hidden
$monitor = $null
$script:writer = $null
$screens = @()

try {
    Wait-ForLog "buzzos:/> " $TimeoutSeconds

    $monitor = [Net.Sockets.TcpClient]::new("127.0.0.1", $monitorPort)
    $script:writer = [IO.StreamWriter]::new($monitor.GetStream(), [Text.Encoding]::ASCII)
    $script:writer.NewLine = "`n"
    $script:writer.AutoFlush = $true

    Type-Command "gui"
    Start-Sleep -Milliseconds 900
    $appsPpm = (Join-Path $OutDir "app-center.ppm")
    $screens += Capture-Screen "app-center" $appsPpm (Join-Path $OutDir "app-center.png")

    Type-Text "text"
    Start-Sleep -Milliseconds 350
    $searchPpm = (Join-Path $OutDir "launcher-search.ppm")
    $screens += Capture-Screen "launcher-search" $searchPpm (Join-Path $OutDir "launcher-search.png")
    Type-Text "zzz"
    Start-Sleep -Milliseconds 250
    $noResultsPpm = (Join-Path $OutDir "launcher-no-results.ppm")
    $screens += Capture-Screen "launcher-no-results" $noResultsPpm (Join-Path $OutDir "launcher-no-results.png")
    Press-Many "backspace" 3
    Send-Key "ret"
    Start-Sleep -Milliseconds 900
    $texteditPpm = (Join-Path $OutDir "textedit.ppm")
    $screens += Capture-Screen "textedit" $texteditPpm (Join-Path $OutDir "textedit.png")
    $desktopSize = Get-PpmDimensions $texteditPpm
    $taskX = [int]($desktopSize.Width / 2) + 53
    $dockY = $desktopSize.Height - 44
    # With one external app, the compact task tile is 53 px right of center.
    # Its first click minimizes TextEdit and its second restores the same task.
    Move-MouseRelative -2000 -2000
    Move-MouseRelative $taskX $dockY
    Click-Left
    Start-Sleep -Milliseconds 500
    $minimizedPpm = (Join-Path $OutDir "taskbar-minimized.ppm")
    $screens += Capture-Screen "taskbar-minimized" $minimizedPpm (Join-Path $OutDir "taskbar-minimized.png")
    Click-Left
    Start-Sleep -Milliseconds 500
    $restoredPpm = (Join-Path $OutDir "taskbar-restored.ppm")
    $screens += Capture-Screen "taskbar-restored" $restoredPpm (Join-Path $OutDir "taskbar-restored.png")
    # The pinned System tile is one 54 px step to the left. Its tooltip should
    # appear after the desktop's short initial hover delay.
    Move-MouseRelative -54 0
    Start-Sleep -Milliseconds 600
    $tooltipPpm = (Join-Path $OutDir "taskbar-tooltip.ppm")
    $screens += Capture-Screen "taskbar-tooltip" $tooltipPpm (Join-Path $OutDir "taskbar-tooltip.png")
    Send-Key "alt-tab"
    Start-Sleep -Milliseconds 350
    $switcherPpm = (Join-Path $OutDir "alt-tab-launcher.ppm")
    $screens += Capture-Screen "alt-tab-launcher" $switcherPpm (Join-Path $OutDir "alt-tab-launcher.png")
    Send-Key "alt-tab"
    Start-Sleep -Milliseconds 250
    # Double-clicking the TextEdit title bar maximizes it.
    Move-MouseRelative -2000 -2000
    Move-MouseRelative 280 91
    Click-Left
    Click-Left
    Start-Sleep -Milliseconds 1250
    $texteditMaxPpm = (Join-Path $OutDir "textedit-maximized.ppm")
    $screens += Capture-Screen "textedit-maximized" $texteditMaxPpm (Join-Path $OutDir "textedit-maximized.png")
    # Dragging a maximized title bar restores the saved window size under the
    # pointer instead of overwriting the restore geometry.
    Move-MouseRelative -2000 -2000
    Move-MouseRelative 300 55
    Send-Hmp "mouse_button 1"
    Move-MouseRelative 140 110
    Send-Hmp "mouse_button 0"
    Start-Sleep -Milliseconds 1250
    $dragRestorePpm = (Join-Path $OutDir "textedit-drag-restored.ppm")
    $screens += Capture-Screen "textedit-drag-restored" $dragRestorePpm (Join-Path $OutDir "textedit-drag-restored.png")
    # Dragging a regular title bar into the left screen edge snaps it to the
    # left half of the desktop work area.
    Move-MouseRelative -2000 -2000
    Move-MouseRelative 440 168
    Send-Hmp "mouse_button 1"
    Move-MouseRelative -2000 0
    Start-Sleep -Milliseconds 350
    $snapPreviewPpm = (Join-Path $OutDir "textedit-snap-preview.ppm")
    $screens += Capture-Screen "textedit-snap-preview" $snapPreviewPpm (Join-Path $OutDir "textedit-snap-preview.png")
    Send-Hmp "mouse_button 0"
    Start-Sleep -Milliseconds 900
    $snapLeftPpm = (Join-Path $OutDir "textedit-snapped-left.ppm")
    $screens += Capture-Screen "textedit-snapped-left" $snapLeftPpm (Join-Path $OutDir "textedit-snapped-left.png")
    # Super+Right moves the focused window to the opposite half without an
    # animation; Super+Down restores its pre-snap geometry.
    Send-Key "meta_l-right"
    Start-Sleep -Milliseconds 700
    $snapRightPpm = (Join-Path $OutDir "textedit-snapped-right.ppm")
    $screens += Capture-Screen "textedit-snapped-right" $snapRightPpm (Join-Path $OutDir "textedit-snapped-right.png")
    Send-Key "meta_l-down"
    Start-Sleep -Milliseconds 550
    # Dragging to the top edge uses the same direct manipulation path to
    # maximize, distinct from the title-bar double-click path above.
    Move-MouseRelative -2000 -2000
    Move-MouseRelative 440 168
    Send-Hmp "mouse_button 1"
    Move-MouseRelative 0 -2000
    Send-Hmp "mouse_button 0"
    Start-Sleep -Milliseconds 900
    $dragMaxPpm = (Join-Path $OutDir "textedit-drag-maximized.ppm")
    $screens += Capture-Screen "textedit-drag-maximized" $dragMaxPpm (Join-Path $OutDir "textedit-drag-maximized.png")
    # Alt+F4 closes only the focused window; it must not terminate the desktop.
    Send-Key "alt-f4"
    Start-Sleep -Milliseconds 700
    $altF4Ppm = (Join-Path $OutDir "alt-f4-closed.ppm")
    $screens += Capture-Screen "alt-f4-closed" $altF4Ppm (Join-Path $OutDir "alt-f4-closed.png")
    Send-Key "ctrl-alt-esc"
    Wait-ForLog "\[gui\] exited" 10

    # Opening a regular /bin ELF through Files must hand it to Terminal
    # without terminating the File Manager GUI protocol session.
    Type-Command "gui"
    Press-Many "down" 4
    Send-Key "ret"
    Start-Sleep -Milliseconds 600
    Send-Key "left"
    Start-Sleep -Milliseconds 250
    Send-Key "ret"
    Press-Many "down" 4
    Send-Key "ret"
    Start-Sleep -Milliseconds 900
    $filesExecPpm = (Join-Path $OutDir "filemanager-terminal-exec.ppm")
    $screens += Capture-Screen "filemanager-terminal-exec" $filesExecPpm (Join-Path $OutDir "filemanager-terminal-exec.png")
    if ((Read-SerialLog) -match "\[gui\] app protocol ended") {
        Fail-WithLog "File Manager protocol ended while opening a terminal ELF."
    }
    Send-Key "ctrl-alt-esc"
    Wait-ForLog "\[gui\] exited" 10

    Type-Command "gui"
    Press-Many "down" 2
    Send-Key "ret"
    Start-Sleep -Milliseconds 900
    $paintPpm = (Join-Path $OutDir "paint.ppm")
    $screens += Capture-Screen "paint" $paintPpm (Join-Path $OutDir "paint.png")
    Send-Key "ctrl-alt-esc"
    Wait-ForLog "\[gui\] exited" 10

    Type-Command "gui"
    Press-Many "down" 3
    Send-Key "ret"
    Start-Sleep -Milliseconds 900
    $calculatorPpm = (Join-Path $OutDir "calculator.ppm")
    $screens += Capture-Screen "calculator" $calculatorPpm (Join-Path $OutDir "calculator.png")
    Send-Key "ctrl-alt-esc"
    Wait-ForLog "\[gui\] exited" 10

    Type-Command "gui"
    Press-Many "down" 5
    Send-Key "ret"
    Start-Sleep -Milliseconds 900
    $browserPpm = (Join-Path $OutDir "browser.ppm")
    $screens += Capture-Screen "browser" $browserPpm (Join-Path $OutDir "browser.png")
    Send-Key "ctrl-alt-esc"
    Wait-ForLog "\[gui\] exited" 10

    Type-Command "gui"
    Press-Many "down" 4
    Send-Key "ret"
    Start-Sleep -Milliseconds 900
    $filesPpm = (Join-Path $OutDir "filemanager.ppm")
    $screens += Capture-Screen "filemanager" $filesPpm (Join-Path $OutDir "filemanager.png")
    # Files starts at /fs with the apps directory selected. Enter it, select
    # calculator.readme, and verify a parameterized TextEdit window opens.
    Send-Key "ret"
    Send-Key "down"
    Send-Key "down"
    Send-Key "ret"
    Start-Sleep -Milliseconds 900
    $filesTextPpm = (Join-Path $OutDir "filemanager-textedit.ppm")
    $screens += Capture-Screen "filemanager-textedit" $filesTextPpm (Join-Path $OutDir "filemanager-textedit.png")
    # Cycle back to Files and launch six more document windows. Together with
    # Files this exercises eight external windows, beyond the old six-window
    # total (three built-ins plus three app slots).
    for ($i = 0; $i -lt 6; $i++) {
        Send-Key "tab"
        Send-Key "tab"
        Send-Key "tab"
        Send-Key "tab"
        Send-Key "ret"
    }
    Start-Sleep -Milliseconds 900
    $manyPpm = (Join-Path $OutDir "many-windows.ppm")
    $screens += Capture-Screen "many-windows" $manyPpm (Join-Path $OutDir "many-windows.png")
    Send-Key "ctrl-alt-esc"
    Wait-ForLog "\[gui\] exited" 10

    Type-Command "gui"
    Press-Many "down" 6
    Send-Key "ret"
    if ($AudioDriver -eq "none") {
        Start-Sleep -Seconds 6
    } else {
        Wait-ForLog "PCM playback started \(AC97 bus master\)" 15
    }
    Start-Sleep -Seconds 2
    $doomPpm = (Join-Path $OutDir "doom.ppm")
    $screens += Capture-Screen "doom" $doomPpm (Join-Path $OutDir "doom.png")
    Send-Key "ret"
    Start-Sleep -Seconds 2
    $doomInputPpm = (Join-Path $OutDir "doom-input.ppm")
    $screens += Capture-Screen "doom-input" $doomInputPpm (Join-Path $OutDir "doom-input.png")
    Send-Key "ctrl-alt-esc"
    Wait-ForLog "\[gui\] exited" 10

    Type-Command "gui"
    Type-Text "terminal"
    Send-Key "ret"
    Start-Sleep -Milliseconds 900
    Type-Text "about"
    Send-Key "ret"
    Start-Sleep -Milliseconds 900
    $terminalPpm = (Join-Path $OutDir "terminal-about.ppm")
    $screens += Capture-Screen "terminal-about" $terminalPpm (Join-Path $OutDir "terminal-about.png")
    Send-Key "ctrl-alt-esc"
    Wait-ForLog "\[gui\] exited" 10

    $log = Read-SerialLog
    if ($log -match "=== EXCEPTION ===") {
        Fail-WithLog "QEMU reported a CPU exception."
    }
    if ($log -match "\[gui\] app protocol ended") {
        Fail-WithLog "A GUI application protocol ended unexpectedly."
    }

    foreach ($screen in $screens) {
        if (!(Test-Path -LiteralPath $screen.Ppm)) {
            Fail-WithLog "Missing screenshot: $($screen.Ppm)"
        }
        Convert-And-Assert-Ppm $screen.Ppm $screen.Png $screen.Name
    }

    Send-Hmp "quit"
    if (!$script:qemuProcess.WaitForExit(5000)) {
        Stop-QemuIfRunning
    }

    Write-Host "GUI smoke passed. Screenshots:"
    foreach ($screen in $screens) {
        Write-Host "  $($screen.Png)"
    }
} finally {
    if ($script:writer) {
        $script:writer.Dispose()
    }
    if ($monitor) {
        $monitor.Dispose()
    }
    Stop-QemuIfRunning
}
