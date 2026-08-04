# Procfs Status Files

BuzzOS mounts a small read-only pseudo filesystem at `/proc`. It exposes live
kernel status through normal file operations, so shell tools can inspect the
system without adding a new syscall for every diagnostic.

## Files

```text
/proc/about
/proc/tasks
/proc/threads
/proc/health
/proc/interfaces
/proc/limits
/proc/fs
/proc/meminfo
/proc/net
/proc/sync
/proc/fds
/proc/mounts
```

`/proc/about` is the compact project identity surface:

```text
name BuzzOS
kind native-x86_64-posix-os
status experimental
arch x86_64
mode long64
entrypoints shell,gui,procfs,fs,apps,report
docs README.md,README.en.md,docs/project-status.md
log CHANGELOG.md
```

The text shell exposes the same file through the `about` command. The desktop
terminal runs the same shell command, and `make report` renders it as the
Project Identity table.

`/proc/health` is the compact cross-interface health summary. It is the first
status file to try when you want one screen of runtime state:

```text
status
interfaces
mem_free_pages
mem_used_pages
mem_managed_pages
fs_status
fs_used_inodes
fs_used_blocks
fs_free_blocks
net_ip
proc_entries
```

The text shell exposes the same file through the `health` command. The desktop
terminal runs the same shell command, so the status surface is available
through `/proc`, the CLI, and the user GUI without adding another syscall.

`/proc/interfaces` is a compact capability matrix for BuzzOS entrypoints:

```text
NAME STATUS ENTRYPOINTS
procfs stable /proc
about stable /proc/about,about,gui:about,make:report
health stable /proc/health,health,gui:health,make:report
interfaces stable /proc/interfaces,interfaces,gui:interfaces,make:report
limits stable /proc/limits,limits,gui:limits,make:report
apps stable /fs/apps,apps,gui:apps,tools:gen_app_registry
shell stable /bin/sh,pipes,redirection
gui experimental /bin/gui,/fs/apps
fs stable /fs,/proc/fs,fsinfo,fsstat,tools:check_minifs
net experimental socket,wget,netstat,/proc/net
sync stable pipe,futex,/proc/sync
report stable make:report,build/project-report.md
```

The text shell exposes it as `interfaces`; the desktop terminal runs the same
command. This keeps the supported surfaces discoverable without a heavier
service registry.

`/proc/limits` exposes fixed runtime capacity boundaries:

```text
max_tasks
max_fd_per_owner
max_pipes
pipe_buf_bytes
max_mounts
ramfs_nodes
ramfs_file_buf_bytes
fs_name_len
page_size
managed_limit_bytes
minifs_lba_start
minifs_sectors
minifs_status
minifs_inodes
minifs_blocks
minifs_max_file_size
```

The text shell exposes it as `limits`; the desktop terminal runs the same command.
These are read-only capacity facts, not tunables.

`/proc/fs` exposes the live `/fs` minifs status in one read-only text file:

```text
mount /fs
driver minifs
status ok
mounted yes
dirty no
corrupt no
recovery_required no
journal disabled
last_error none
lba_start 67584
sectors 4096
magic 1397113421
inodes_used
inodes_total
dirs
files
blocks_used
blocks_free
blocks_total
data_lba
max_file_size
host_check make fs-check
host_repair make fs-repair
```

The text shell and desktop terminal expose it as `fsinfo`. `fsstat` remains the
syscall-backed compact counter view; `fsinfo` is the procfs view that also
points users to host-side check and repair commands.

`/proc/tasks` mirrors the process table format used by `ps`.

`/proc/threads` lists individual scheduler tasks, including threads that are
currently `READY`, `RUNNING`, `SLEEP`, or `BLOCKED`.

`/proc/meminfo` reports the physical page allocator's current view:

```text
page_size
managed_limit
managed_pages
used_pages
free_pages
```

`/proc/net` reports the current lightweight network state:

```text
driver
mac
ip
gateway
dns
dhcp
arp
tcp
tcp_rx_buffered
tcp_rx_dropped
invalid_frames
checksum_failures
fragment_drops
tx_frames
rx_frames
arp_frames
ip_frames
icmp_packets
udp_packets
tcp_packets
dhcp_packets
dns_packets
```

The validation counters record frames rejected before protocol code can inspect
packet-declared offsets or lengths. The `tcp` line reports whether any TCP PCB is connected and the current open
PCB count, for example `tcp closed open 0`. `tcp_rx_buffered` and
`tcp_rx_dropped` expose the lightweight TCP demux receive queue state.

`/proc/sync` reports synchronization state:

```text
futex_waiters
SLOT TID ADDR WOKEN
```

The `syncstat` shell command prints the same file.

`/proc/fds` reports the current process fd-owner table:

```text
OWNER FD OF REFS FLAGS KIND NAME DETAIL
```

It includes stdio, currently open procfs files, and pipe endpoints with pipe
index, buffered byte count, reader count, and writer count. The `fdstat` shell
command prints the same file.

`/proc/mounts` lists the built-in mounts:

```text
/ ramfs
/dev devfs
/proc procfs
/fs minifs
```

## Usage

From the BuzzOS shell:

```text
ls /proc
cat /proc/about
cat /proc/tasks
cat /proc/threads
cat /proc/health
cat /proc/interfaces
cat /proc/limits
cat /proc/fs
cat /proc/meminfo
cat /proc/fds
cat /proc/net
cat /proc/sync
cat /proc/mounts
about
health
interfaces
limits
fsinfo
fdstat
```

The files are generated when read. They cannot be created, removed, renamed, or
written, and they do not consume `/fs` space.
