param(
    [string]$Image = "build/buzzos.img",
    [Alias("Qemu")]
    [string]$QemuPath = "",
    [string]$SerialLog = "build/serial-smoke.log",
    [string]$TestImage = "build/buzzos-test.img",
    [int]$TimeoutSeconds = 45
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($QemuPath)) {
    $QemuPath = $env:QEMU
}
if ([string]::IsNullOrWhiteSpace($QemuPath)) {
    $QemuPath = "qemu-system-i386"
}
$QemuPath = $QemuPath.Trim('"')

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
        Start-Sleep -Milliseconds 500
        $log = Read-SerialLog
    } until ($log -match $Pattern -or (Get-Date) -gt $deadline)

    if ($log -notmatch $Pattern) {
        Fail-WithLog "Timed out waiting for serial output: $Pattern"
    }
}

function Send-Hmp([string]$Line) {
    $script:writer.WriteLine($Line)
    Start-Sleep -Milliseconds 45
}

function Key-Name([char]$Ch) {
    switch ($Ch) {
        " " { return "spc" }
        "/" { return "slash" }
        "-" { return "minus" }
        "." { return "dot" }
        "|" { return "shift-backslash" }
        "<" { return "shift-comma" }
        ">" { return "shift-dot" }
        default { return [string]$Ch }
    }
}

function Type-Command([string]$Text) {
    foreach ($ch in $Text.ToCharArray()) {
        Send-Hmp ("sendkey " + (Key-Name $ch))
    }
    Send-Hmp "sendkey ret"
    Start-Sleep -Milliseconds 900
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
        # Cleanup should not hide the smoke-test result.
    }
}

function Connect-QemuMonitor([int]$Port, [int]$Seconds = 10) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        if ($script:qemuProcess -and $script:qemuProcess.HasExited) {
            Fail-WithLog "QEMU exited before its monitor became ready (code $($script:qemuProcess.ExitCode))."
        }
        $client = [Net.Sockets.TcpClient]::new()
        try {
            $client.Connect("127.0.0.1", $Port)
            return $client
        } catch [Net.Sockets.SocketException] {
            $client.Dispose()
            Start-Sleep -Milliseconds 100
        }
    } until ((Get-Date) -gt $deadline)
    Fail-WithLog "Timed out connecting to QEMU monitor on 127.0.0.1:$Port."
}

function Start-TcpSmokeServer([int]$Port, [string]$Body, [string]$ReadyPath) {
    Remove-Item -LiteralPath $ReadyPath -ErrorAction SilentlyContinue
    $job = Start-Job -ArgumentList $Port, $Body, $ReadyPath -ScriptBlock {
        param([int]$Port, [string]$Body, [string]$ReadyPath)
        $ErrorActionPreference = "Stop"
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
        $listener.Start(4)
        [IO.File]::WriteAllText($ReadyPath, "ready")
        try {
            $deadline = (Get-Date).AddSeconds(90)
            while (!$listener.Pending()) {
                if ((Get-Date) -gt $deadline) {
                    throw "timed out waiting for guest TCP connection"
                }
                Start-Sleep -Milliseconds 50
            }
            $client = $listener.AcceptTcpClient()
            try {
                $stream = $client.GetStream()
                $stream.ReadTimeout = 500
                $scratch = New-Object byte[] 512
                $request = ""
                $readDeadline = (Get-Date).AddSeconds(8)
                while ($request -notmatch "`r`n`r`n" -and (Get-Date) -lt $readDeadline) {
                    try {
                        $count = $stream.Read($scratch, 0, $scratch.Length)
                        if ($count -le 0) {
                            break
                        }
                        $request += [Text.Encoding]::ASCII.GetString($scratch, 0, $count)
                    } catch [System.IO.IOException] {
                        # Keep waiting briefly; the guest may still be sending the request.
                    }
                }
                if ($request.Length -eq 0) {
                    throw "no HTTP request received from guest"
                }
                $payload = [Text.Encoding]::ASCII.GetBytes($Body + "`n")
                $header = "HTTP/1.0 200 OK`r`nContent-Length: $($payload.Length)`r`nConnection: close`r`n`r`n"
                $headerBytes = [Text.Encoding]::ASCII.GetBytes($header)
                $stream.Write($headerBytes, 0, $headerBytes.Length)
                $stream.Write($payload, 0, $payload.Length)
                $stream.Flush()
                "served"
            } finally {
                $client.Dispose()
            }
        } finally {
            $listener.Stop()
        }
    }

    $deadline = (Get-Date).AddSeconds(10)
    while (!(Test-Path -LiteralPath $ReadyPath)) {
        if ($job.State -ne "Running") {
            $details = Receive-Job -Job $job -ErrorAction SilentlyContinue | Out-String
            throw "TCP smoke server failed to start: $details"
        }
        if ((Get-Date) -gt $deadline) {
            throw "Timed out waiting for TCP smoke server to start."
        }
        Start-Sleep -Milliseconds 100
    }
    return $job
}

function Wait-TcpSmokeServer([System.Management.Automation.Job]$Job) {
    $done = Wait-Job -Job $Job -Timeout 15
    if (!$done) {
        Fail-WithLog "Timed out waiting for TCP smoke server to finish."
    }
    if ($Job.State -ne "Completed") {
        $details = Receive-Job -Job $Job -ErrorAction SilentlyContinue | Out-String
        Fail-WithLog "TCP smoke server failed: $details"
    }
    [void](Receive-Job -Job $Job -ErrorAction SilentlyContinue)
}

Copy-Item -LiteralPath $Image -Destination $TestImage -Force
Remove-Item -LiteralPath $SerialLog -ErrorAction SilentlyContinue

$monitorPort = Get-FreeTcpPort
$tcpSmokePort = Get-FreeTcpPort
$tcpSmokeBody = ("x" * 3072) + "BUZZOS_TCP_SMOKE_OK"
$tcpSmokeReady = [IO.Path]::GetFullPath((Join-Path "build" "tcp-smoke.ready"))
$tcpSmokeJob = Start-TcpSmokeServer $tcpSmokePort $tcpSmokeBody $tcpSmokeReady
$tcpPairPortA = Get-FreeTcpPort
$tcpPairPortB = Get-FreeTcpPort
$tcpPairReadyA = [IO.Path]::GetFullPath((Join-Path "build" "tcp-pair-a.ready"))
$tcpPairReadyB = [IO.Path]::GetFullPath((Join-Path "build" "tcp-pair-b.ready"))
$tcpPairJobA = Start-TcpSmokeServer $tcpPairPortA "BUZZOS_TCP_TWO_A" $tcpPairReadyA
$tcpPairJobB = Start-TcpSmokeServer $tcpPairPortB "BUZZOS_TCP_TWO_B" $tcpPairReadyB

$qemuArgs = @(
    "-cpu", "max",
    "-m", "256",
    "-drive", "format=raw,file=$TestImage",
    "-serial", "file:$SerialLog",
    "-display", "none",
    "-monitor", "tcp:127.0.0.1:$monitorPort,server,nowait",
    "-no-reboot",
    "-vga", "std",
    "-audiodev", "none,id=audio0",
    "-device", "AC97,audiodev=audio0",
    "-netdev", "user,id=n0",
    "-device", "ne2k_isa,netdev=n0,iobase=0x300,irq=10"
)

$script:qemuProcess = Start-Process -FilePath $QemuPath -ArgumentList $qemuArgs -WorkingDirectory (Get-Location) -PassThru -WindowStyle Hidden
$monitor = $null
$script:writer = $null

try {
    Wait-ForLog "buzzos:/> " $TimeoutSeconds

    $monitor = Connect-QemuMonitor $monitorPort
    $script:writer = [IO.StreamWriter]::new($monitor.GetStream(), [Text.Encoding]::ASCII)
    $script:writer.NewLine = "`n"
    $script:writer.AutoFlush = $true

    $commands = @(
        "exec /bin/audiotest",
        "ls /",
        "ls /bin",
        "ls /proc",
        "ls /fs/apps",
        "cat /proc/about",
        "cat /proc/health",
        "cat /proc/interfaces",
        "cat /proc/limits",
        "cat /proc/fs",
        "cat /proc/mounts",
        "cat /proc/meminfo",
        "cat /proc/fds",
        "cat /proc/net",
        "cat /proc/sync",
        "cat /proc/tasks",
        "cat /proc/threads",
        "about",
        "health",
        "interfaces",
        "limits",
        "fsinfo",
        "fsstat",
        "fdstat",
        "help apps",
        "help gui",
        "help proc",
        "help edit",
        "help net",
        "help pipes",
        ("wget 10.0.2.2 " + $tcpSmokePort),
        ("tcptwotest 10.0.2.2 " + $tcpPairPortA + " " + $tcpPairPortB),
        "netstat",
        "syncstat",
        "apps",
        "apps info textedit",
        "apps info calculator",
        "basm /fs/basm-full.asm /fs/basm-full",
        "exec /fs/basm-full",
        "rm /fs/basm-full",
        "bcc /fs/hello.c /fs/hello-bcc",
        "exec /fs/hello-bcc",
        "rm /fs/hello-bcc",
        "write /proc/meminfo nope",
        "touch /proc/nope",
        "write /fs/smoke ok",
        "cat /fs/smoke",
        "rm /fs/smoke",
        "elfbadtest",
        "badptrtest",
        "exec /bin/faulttest",
        "threadreusetest",
        "exec /bin/socketleak",
        "exec /bin/socketleak",
        "exec /bin/heaptest",
        "exec /bin/nsporttest",
        "exec /bin/nshtmltest",
        "pipetest",
        "pipeedgetest",
        "pipeblocktest",
        "echo pipelinesmokeok | cat",
        "echo multipipesmokeok | cat | cat",
        "echo redir-one > /fs/redir",
        "echo redir-two >> /fs/redir",
        "cat < /fs/redir",
        "rm /fs/redir",
        "futextest",
        "futextimeouttest",
        "futexcanceltest",
        "threads",
        "futexblocktest",
        "ps"
    )
    foreach ($command in $commands) {
        Type-Command $command
        if ($command -like "wget 10.0.2.2 *") {
            Wait-TcpSmokeServer $tcpSmokeJob
            Start-Sleep -Seconds 1
        }
        if ($command -like "tcptwotest 10.0.2.2 *") {
            Wait-TcpSmokeServer $tcpPairJobA
            Wait-TcpSmokeServer $tcpPairJobB
            Start-Sleep -Seconds 1
        }
    }

    Start-Sleep -Seconds 2
    $log = Read-SerialLog
    if ($log -match "Halted\.") {
        Fail-WithLog "QEMU halted after a CPU exception."
    }
    if ([regex]::Matches($log, "socketleak: opened 8").Count -lt 2) {
        Fail-WithLog "Socket ownership cleanup did not survive two leaking child processes."
    }

    $expected = @(
        "=== BuzzOS User Shell ===",
        "bin",
        "fs",
        "proc",
        "gui",
        "tasks",
        "threads",
        "meminfo",
        "net",
        "mounts",
        "sync",
        "fds",
        "about",
        "health",
        "interfaces",
        "limits",
        "name\s+BuzzOS",
        "kind\s+lightweight-i386-posix-like-os",
        "entrypoints\s+shell,gui,procfs,fs,apps,report",
        "docs\s+README\.md,README\.en\.md,docs/project-status\.md",
        "log\s+CHANGELOG\.md",
        "status\s+ok",
        "interfaces\s+proc\s+shell\s+gui\s+report",
        "mem_free_pages\s+[0-9]+",
        "mem_used_pages\s+[0-9]+",
        "mem_managed_pages\s+[0-9]+",
        "fs_status\s+ok",
        "fs_used_inodes\s+[0-9]+",
        "fs_used_blocks\s+[0-9]+",
        "fs_free_blocks\s+[0-9]+",
        "net_ip\s+10\.0\.2\.15",
        "proc_entries\s+12",
        "max_tasks\s+32",
        "max_fd_per_owner\s+32",
        "max_pipes\s+16",
        "pipe_buf_bytes\s+8192",
        "max_mounts\s+8",
        "fs_name_len\s+24",
        "managed_limit_bytes\s+67108864",
        "minifs_lba_start\s+67584",
        "minifs_sectors\s+32768",
        "minifs_status\s+ok",
        "minifs_inodes\s+128",
        "minifs_blocks\s+3959",
        "minifs_max_file_size\s+16678400",
        "NAME\s+STATUS\s+ENTRYPOINTS",
        "procfs\s+stable\s+/proc",
        "about\s+stable\s+/proc/about,about,gui:about,make:report",
        "health\s+stable\s+/proc/health,health,gui:health,make:report",
        "interfaces\s+stable\s+/proc/interfaces,interfaces,gui:interfaces,make:report",
        "limits\s+stable\s+/proc/limits,limits,gui:limits,make:report",
        "apps\s+stable\s+/fs/apps,apps,gui:apps,tools:gen_app_registry",
        "fs\s+stable\s+/fs,/proc/fs,fsinfo,fsstat,tools:check_minifs",
        "net\s+experimental\s+socket,wget,netstat,/proc/net",
        "report\s+stable\s+make:report,build/project-report.md",
        "/proc procfs",
        "mount\s+/fs",
        "driver\s+minifs",
        "lba_start\s+67584",
        "sectors\s+4096",
        "magic\s+1397113421",
        "inodes_used\s+[0-9]+",
        "inodes_total\s+128",
        "dirs\s+[0-9]+",
        "files\s+[0-9]+",
        "blocks_used\s+[0-9]+",
        "blocks_free\s+[0-9]+",
        "blocks_total\s+3959",
        "data_lba\s+67721",
        "max_file_size\s+16678400",
        "host_check\s+make fs-check",
        "host_repair\s+make fs-repair",
        "page_size\s+4096",
        "managed_limit\s+67108864",
        "OWNER\s+FD\s+OF\s+REFS\s+FLAGS\s+KIND\s+NAME\s+DETAIL",
        "[0-9]+\s+0\s+[0-9]+\s+1\s+rw\s+dev\s+console\s+pos=0",
        "[0-9]+\s+1\s+[0-9]+\s+1\s+rw\s+dev\s+console\s+pos=0",
        "[0-9]+\s+2\s+[0-9]+\s+1\s+rw\s+dev\s+console\s+pos=0",
        "driver\s+ne2000",
        "ip\s+10\.0\.2\.15",
        "gateway\s+10\.0\.2\.2",
        "dns\s+10\.0\.2\.3",
        "dhcp\s+assigned",
        "tcp\s+closed\s+open\s+0",
        "tcp_rx_buffered\s+[0-9]+",
        "tcp_rx_dropped\s+[0-9]+",
        "tx_frames\s+[0-9]+",
        "rx_frames\s+[0-9]+",
        "arp_frames\s+tx\s+[0-9]+\s+rx\s+[0-9]+",
        "ip_frames\s+tx\s+[0-9]+\s+rx\s+[0-9]+",
        "icmp_packets\s+tx\s+[0-9]+\s+rx\s+[0-9]+",
        "udp_packets\s+tx\s+[0-9]+\s+rx\s+[0-9]+",
        "tcp_packets\s+tx\s+[0-9]+\s+rx\s+[0-9]+",
        "dhcp_packets\s+tx\s+[0-9]+\s+rx\s+[0-9]+",
        "dns_packets\s+tx\s+[0-9]+\s+rx\s+[0-9]+",
        "futex_waiters\s+0/32",
        "SLOT\s+TID\s+ADDR\s+WOKEN",
        "TID\s+PID\s+STATE\s+OUT\s+NAME",
        "textedit",
        "paint",
        "calculator",
        "/fs minifs",
        "inodes\s+[0-9]+/128",
        "blocks\s+[0-9]+/3959",
        "data_lba\s+67721",
        "GUI apps are launched through the desktop",
        "gui opens the desktop",
        "/proc is read-only runtime state",
        "cat /proc/about",
        "cat /proc/fs",
        "cat /proc/limits",
        "cat /proc/health",
        "cat /proc/interfaces",
        "cat /proc/fds",
        "network: dhcp renews IP, netstat shows /proc/net",
        "shell edit: arrows",
        "shell pipelines connect external programs",
        "redirection: < input, > output, >> append",
        "pipelinesmokeok",
        "\[pipe\] exited 0 \| 0",
        "multipipesmokeok",
        "\[pipe\] exited 0 \| 0 \| 0",
        "redir-one",
        "redir-two",
        "HTTP/1\.0 200 OK",
        "BUZZOS_TCP_SMOKE_OK",
        "tcptwotest: recv b",
        "BUZZOS_TCP_TWO_B",
        "tcptwotest: recv a",
        "BUZZOS_TCP_TWO_A",
        "tcptwotest: done",
        "APP\s+VERSION\s+SUMMARY",
        "TextEdit\s+1\s+Plain text editor",
        "state\s+/fs/textedit.txt",
        "readme\s+/fs/apps/textedit.readme",
        "Calculator\s+1\s+Expression calculator",
        "state\s+/fs/calculator.seed",
        "readme\s+/fs/apps/calculator.readme",
        "basm: wrote /fs/basm-full \([0-9]+ bytes\)",
        "\[basm\] exited 0",
        "basm-full-ok",
        "\[exec\] exited 7",
        "bcc: wrote /fs/hello-bcc",
        "bcc-hello-ok",
        "\[exec\] exited 42",
        "write: open failed",
        "touch: failed",
        "elfbad: vaddr -1",
        "elfbad: filesz -1",
        "elfbad: memsz -1",
        "elfbad: entry -1",
        "elfbad: ok",
        "badptr: rejected -1",
        "Terminating faulting user task",
        "\[exec\] exited -14",
        "threadreuse: joined 40",
        "socketleak: opened 8",
        "heaptest: ok 320K realloc reuse",
        "audiotest: ok [0-9]+ bytes",
        "nsporttest: ok surface scheduler",
        "nshtmltest: ok HTML DOM CSS form img",
        "basm",
        "nano",
        "sh",
        "ok",
        "pipe: hello through pipe",
        "pipeedge: eof 0",
        "pipeedge: no-reader -1",
        "pipeblock: reader 4 write 4 wake",
        "pipeblock: writer 600 drain 512\+88",
        "futex: woke",
        "futextimeout: timeout -2",
        "futextimeout: wake 0 word 1",
        "futexcancel: killed 34 wait -2",
        "futexblock: waiting threads",
        "BLOCKED\s+tty\s+user_thread",
        "futex_waiters\s+1/32",
        "[0-9]+\s+[0-9]+\s+0x[0-9A-F]+\s+0",
        "futexblock: woke\s+1",
        "PID\s+STATE"
    )
    foreach ($pattern in $expected) {
        if ($log -notmatch $pattern) {
            Fail-WithLog "Missing expected serial output: $pattern"
        }
    }

    Send-Hmp "quit"
    if (!$script:qemuProcess.WaitForExit(5000)) {
        Stop-QemuIfRunning
    }
    Write-Host "Smoke test passed: $SerialLog"
} finally {
    if ($script:writer) {
        $script:writer.Dispose()
    }
    if ($monitor) {
        $monitor.Dispose()
    }
    if ($tcpSmokeJob) {
        if ($tcpSmokeJob.State -eq "Running") {
            Stop-Job -Job $tcpSmokeJob -ErrorAction SilentlyContinue
        }
        Remove-Job -Job $tcpSmokeJob -Force -ErrorAction SilentlyContinue
    }
    if ($tcpPairJobA) {
        if ($tcpPairJobA.State -eq "Running") {
            Stop-Job -Job $tcpPairJobA -ErrorAction SilentlyContinue
        }
        Remove-Job -Job $tcpPairJobA -Force -ErrorAction SilentlyContinue
    }
    if ($tcpPairJobB) {
        if ($tcpPairJobB.State -eq "Running") {
            Stop-Job -Job $tcpPairJobB -ErrorAction SilentlyContinue
        }
        Remove-Job -Job $tcpPairJobB -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $tcpSmokeReady -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tcpPairReadyA -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tcpPairReadyB -ErrorAction SilentlyContinue
    Stop-QemuIfRunning
}
