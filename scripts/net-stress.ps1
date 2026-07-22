param(
    [string]$Image = "build/buzzos.img",
    [Alias("Qemu")]
    [string]$QemuPath = "",
    [string]$SerialLog = "build/serial-net-stress.log",
    [string]$TestImage = "build/buzzos-net-stress.img",
    [int]$Workers = 6,
    [int]$Rounds = 20,
    [int]$BodyBytes = 6144,
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Resolve-BuzzosQemu.ps1")
$qemuInfo = Resolve-BuzzosQemu -Preferred $QemuPath
$QemuPath = $qemuInfo.Path
$QemuAccel = $qemuInfo.Accel
$QemuCpu = $qemuInfo.Cpu

function Get-FreeTcpPort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}

function Read-SerialLog {
    if (!(Test-Path -LiteralPath $SerialLog)) { return "" }
    try { return Get-Content -LiteralPath $SerialLog -Raw -ErrorAction Stop }
    catch { return "" }
}

function Fail-WithLog([string]$Message) {
    $log = Read-SerialLog
    $tailStart = [Math]::Max(0, $log.Length - [Math]::Min($log.Length, 5000))
    throw ($Message + [Environment]::NewLine + $log.Substring($tailStart))
}

function Wait-ForLog([string]$Pattern, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        if ($script:qemuProcess -and $script:qemuProcess.HasExited) {
            Fail-WithLog "QEMU exited early with code $($script:qemuProcess.ExitCode)."
        }
        Start-Sleep -Milliseconds 250
        $log = Read-SerialLog
    } until ($log -match $Pattern -or (Get-Date) -gt $deadline)
    if ($log -notmatch $Pattern) { Fail-WithLog "Timed out waiting for: $Pattern" }
}

function Send-Hmp([string]$Line) {
    $script:writer.WriteLine($Line)
    Start-Sleep -Milliseconds 35
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

function Type-Command([string]$Text) {
    foreach ($ch in $Text.ToCharArray()) {
        Send-Hmp ("sendkey " + (Key-Name $ch))
    }
    Send-Hmp "sendkey ret"
}

function Connect-QemuMonitor([int]$Port, [int]$Seconds = 10) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        $client = [Net.Sockets.TcpClient]::new()
        try {
            $client.Connect("127.0.0.1", $Port)
            return $client
        } catch [Net.Sockets.SocketException] {
            $client.Dispose()
            Start-Sleep -Milliseconds 100
        }
    } until ((Get-Date) -gt $deadline)
    Fail-WithLog "Timed out connecting to QEMU monitor."
}

function Stop-QemuIfRunning {
    if (!$script:qemuProcess) { return }
    try {
        $script:qemuProcess.Refresh()
        if (!$script:qemuProcess.HasExited) {
            $script:qemuProcess.Kill()
            $script:qemuProcess.WaitForExit(5000) | Out-Null
        }
    } catch {}
}

function Start-InterleavedServer([int]$Port, [int]$WorkerCount,
                                 [int]$RoundCount, [int]$PayloadBytes,
                                 [string]$ReadyPath) {
    Remove-Item -LiteralPath $ReadyPath -ErrorAction SilentlyContinue
    return Start-Job -ArgumentList $Port, $WorkerCount, $RoundCount, $PayloadBytes, $ReadyPath -ScriptBlock {
        param($Port, $WorkerCount, $RoundCount, $PayloadBytes, $ReadyPath)
        $ErrorActionPreference = "Stop"
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
        $listener.Start($WorkerCount * 2)
        [IO.File]::WriteAllText($ReadyPath, "ready")
        try {
            for ($round = 0; $round -lt $RoundCount; $round++) {
                $clients = @()
                $streams = @()
                try {
                    for ($i = 0; $i -lt $WorkerCount; $i++) {
                        $client = $listener.AcceptTcpClient()
                        $client.NoDelay = $true
                        $clients += $client
                        $streams += $client.GetStream()
                    }

                    $scratch = New-Object byte[] 512
                    foreach ($stream in $streams) {
                        $stream.ReadTimeout = 3000
                        $request = ""
                        while ($request -notmatch "\r\n\r\n") {
                            $count = $stream.Read($scratch, 0, $scratch.Length)
                            if ($count -le 0) { throw "guest closed before request" }
                            $request += [Text.Encoding]::ASCII.GetString($scratch, 0, $count)
                        }
                    }

                    $crlf = ([string][char]13) + ([string][char]10)
                    $body = ("x" * $PayloadBytes) + "NETSTRESS_OK" + $crlf
                    $payload = [Text.Encoding]::ASCII.GetBytes($body)
                    $header = "HTTP/1.0 200 OK" + $crlf +
                              "Content-Length: $($payload.Length)" + $crlf +
                              "Connection: close" + $crlf + $crlf
                    $response = [Text.Encoding]::ASCII.GetBytes($header + $body)
                    for ($offset = 0; $offset -lt $response.Length; $offset += 257) {
                        $count = [Math]::Min(257, $response.Length - $offset)
                        foreach ($stream in $streams) {
                            $stream.Write($response, $offset, $count)
                            $stream.Flush()
                        }
                    }
                } finally {
                    foreach ($client in $clients) { $client.Dispose() }
                }
            }
            "served $RoundCount rounds x $WorkerCount clients"
        } finally {
            $listener.Stop()
        }
    }
}

if ($Workers -lt 2 -or $Workers -gt 6) { throw "Workers must be 2..6." }
if ($Rounds -lt 1 -or $Rounds -gt 100) { throw "Rounds must be 1..100." }

Copy-Item -LiteralPath $Image -Destination $TestImage -Force
Remove-Item -LiteralPath $SerialLog -ErrorAction SilentlyContinue
$monitorPort = Get-FreeTcpPort
$serverPort = Get-FreeTcpPort
$readyPath = [IO.Path]::GetFullPath((Join-Path "build" "net-stress.ready"))
$serverJob = Start-InterleavedServer $serverPort $Workers $Rounds $BodyBytes $readyPath

$readyDeadline = (Get-Date).AddSeconds(10)
while (!(Test-Path -LiteralPath $readyPath)) {
    if ($serverJob.State -ne "Running") { throw "Network stress server failed to start." }
    if ((Get-Date) -gt $readyDeadline) { throw "Timed out starting network stress server." }
    Start-Sleep -Milliseconds 100
}

$qemuArgs = @(
    "-accel", $QemuAccel,
    "-cpu", $QemuCpu,
    "-m", "256",
    "-drive", "format=raw,file=$TestImage",
    "-serial", "file:$SerialLog",
    "-display", "none",
    "-monitor", "tcp:127.0.0.1:$monitorPort,server,nowait",
    "-no-reboot", "-vga", "std",
    "-audiodev", "none,id=audio0",
    "-device", "AC97,audiodev=audio0",
    "-netdev", "user,id=n0",
    "-device", "ne2k_isa,netdev=n0,iobase=0x300,irq=10"
)

$script:qemuProcess = Start-Process -FilePath $QemuPath -ArgumentList $qemuArgs -WorkingDirectory (Get-Location) -PassThru -WindowStyle Hidden
$monitor = $null
$script:writer = $null
try {
    Wait-ForLog "s:/> " 45
    $monitor = Connect-QemuMonitor $monitorPort
    $script:writer = [IO.StreamWriter]::new($monitor.GetStream(), [Text.Encoding]::ASCII)
    $script:writer.NewLine = [string][char]10
    $script:writer.AutoFlush = $true
    Type-Command "exec /bin/netstress $serverPort $Workers $Rounds"

    $completion = "netstress: (?:ok " + $Rounds + "x" + $Workers + "|failed)"
    Wait-ForLog $completion $TimeoutSeconds
    $log = Read-SerialLog
    if ($log -match "netstress: failed") {
        Fail-WithLog "Guest network stress worker failed."
    }
    $done = Wait-Job -Job $serverJob -Timeout 15
    if (!$done -or $serverJob.State -ne "Completed") {
        $details = Receive-Job -Job $serverJob -ErrorAction SilentlyContinue | Out-String
        Fail-WithLog "Interleaved TCP server failed: $details"
    }
    $serverResult = Receive-Job -Job $serverJob | Out-String
    $log = Read-SerialLog
    if ($log -match "netstress: failed|connect timeout|tx timeout|Halted\.") {
        Fail-WithLog "Network stress log contains a failure."
    }
    Write-Host ("Network stress passed: " + $serverResult.Trim())
} finally {
    if ($script:writer) { $script:writer.Dispose() }
    if ($monitor) { $monitor.Dispose() }
    Stop-QemuIfRunning
    if ($serverJob) {
        Stop-Job -Job $serverJob -ErrorAction SilentlyContinue
        Remove-Job -Job $serverJob -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $readyPath -ErrorAction SilentlyContinue
}
