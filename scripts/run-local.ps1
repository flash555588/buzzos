param(
    [string]$Image = "build/buzzos.img",
    [Alias("Qemu")]
    [string]$QemuPath = "",
    [string]$SerialLog = "build/serial-live.log",
    [string]$Command = "",
    [switch]$NoBuild,
    [switch]$ResetFs,
    [switch]$KillExisting
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "Resolve-BuzzosQemu.ps1")
$qemuInfo = Resolve-BuzzosQemu -Preferred $QemuPath
$QemuPath = $qemuInfo.Path
$QemuAccel = $qemuInfo.Accel
$QemuCpu = $qemuInfo.Cpu

if ($KillExisting) {
    Get-Process | Where-Object { $_.ProcessName -like "qemu*" } |
        Stop-Process -ErrorAction SilentlyContinue
}

if (!$NoBuild) {
    if ($ResetFs) {
        & make image-reset-fs
    } else {
        & make
    }
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if (!(Test-Path -LiteralPath $Image)) {
    throw "$Image does not exist. Build first or omit -NoBuild."
}

$imagePath = (Resolve-Path $Image).Path
$serialDir = Split-Path $SerialLog -Parent
if ($serialDir) {
    New-Item -ItemType Directory -Force $serialDir | Out-Null
}
Remove-Item -LiteralPath $SerialLog -ErrorAction SilentlyContinue
$serialPath = if ([IO.Path]::IsPathRooted($SerialLog)) {
    $SerialLog
} else {
    Join-Path (Get-Location) $SerialLog
}

function Get-FreeTcpPort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}

function Read-SerialLog {
    if (!(Test-Path -LiteralPath $serialPath)) {
        return ""
    }
    try {
        return Get-Content -LiteralPath $serialPath -Raw -ErrorAction Stop
    } catch {
        return ""
    }
}

function Wait-ForLog([string]$Pattern, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        if ($script:qemuProcess -and $script:qemuProcess.HasExited) {
            throw "QEMU exited early with code $($script:qemuProcess.ExitCode)."
        }
        Start-Sleep -Milliseconds 300
        $log = Read-SerialLog
    } until ($log -match $Pattern -or (Get-Date) -gt $deadline)

    if ($log -notmatch $Pattern) {
        throw "Timed out waiting for serial output: $Pattern"
    }
}

function Key-Name([char]$Ch) {
    switch ($Ch) {
        " " { return "spc" }
        "/" { return "slash" }
        "-" { return "minus" }
        "." { return "dot" }
        default { return [string]$Ch }
    }
}

$monitorPort = Get-FreeTcpPort
# Match make run: SDL+GL is far faster than default GTK under WHPX.
$QemuDisplay = if (-not [string]::IsNullOrWhiteSpace($env:QEMU_DISPLAY)) {
    $env:QEMU_DISPLAY.Trim()
} else {
    "sdl,gl=on"
}
$QemuVideo = if (-not [string]::IsNullOrWhiteSpace($env:QEMU_VIDEO)) {
    $env:QEMU_VIDEO.Trim()
} else {
    "-vga none -device virtio-vga,xres=1600,yres=900"
}
$QemuInput = if (-not [string]::IsNullOrWhiteSpace($env:QEMU_INPUT)) {
    $env:QEMU_INPUT.Trim()
} else {
    "-device virtio-tablet-pci"
}
$argLine = '-drive "format=raw,file=' + $imagePath + '"' +
    ' -accel ' + $QemuAccel +
    ' -cpu ' + $QemuCpu +
    ' -m 256' +
    ' -serial "file:' + $serialPath + '"' +
    ' -monitor "tcp:127.0.0.1:' + $monitorPort + ',server,nowait"' +
    ' -no-reboot' +
    ' ' + $QemuVideo +
    ' ' + $QemuInput +
    ' -display ' + $QemuDisplay +
    ' -audiodev dsound,id=audio0' +
    ' -device AC97,audiodev=audio0' +
    ' -netdev user,id=n0' +
    ' -device ne2k_isa,netdev=n0,iobase=0x300,irq=10'

$script:qemuProcess = Start-Process -FilePath $QemuPath -ArgumentList $argLine `
    -WorkingDirectory (Get-Location) -PassThru

if (![string]::IsNullOrWhiteSpace($Command)) {
    Wait-ForLog "buzzos:/> " 45
    $client = [Net.Sockets.TcpClient]::new("127.0.0.1", $monitorPort)
    try {
        $writer = [IO.StreamWriter]::new($client.GetStream(), [Text.Encoding]::ASCII)
        $writer.NewLine = "`n"
        $writer.AutoFlush = $true
        foreach ($ch in $Command.ToCharArray()) {
            $writer.WriteLine("sendkey " + (Key-Name $ch))
            Start-Sleep -Milliseconds 55
        }
        $writer.WriteLine("sendkey ret")
    } finally {
        if ($writer) {
            $writer.Dispose()
        }
        $client.Dispose()
    }
}

Write-Host "QEMU started: pid=$($script:qemuProcess.Id)"
Write-Host "QEMU binary: $QemuPath"
Write-Host "QEMU accel: $QemuAccel  cpu: $QemuCpu  display: $QemuDisplay"
Write-Host "QEMU video: $QemuVideo"
Write-Host "QEMU input: $QemuInput"
Write-Host "Serial log: $serialPath"
Write-Host "Monitor: 127.0.0.1:$monitorPort"
