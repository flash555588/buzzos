#!/bin/bash
# Run QEMU with monitor socket, send ping command via sendkey, capture output
set -e
cd "$(dirname "$0")/.."

QEMU="${QEMU:-/c/msys64/mingw64/bin/qemu-system-x86_64.exe}"
SOCK="build/qemu-monitor.sock"
rm -f "$SOCK" build/serial.log

# Start QEMU with monitor socket
"$QEMU" -accel whpx -cpu qemu64 -m 256 \
    -drive format=raw,file=build/buzzos.img \
    -serial file:build/serial.log \
    -no-reboot \
    -netdev user,id=n0 \
    -device ne2k_isa,netdev=n0,iobase=0x300,irq=10 \
    -monitor unix:$SOCK,server,nowait \
    -display none &
QEMU_PID=$!

# Wait for boot
sleep 4

# Send "ping 10.0.2.2" via monitor sendkey
SENDKEY() {
    local key="$1"
    socat - UNIX-CONNECT:$SOCK <<< "sendkey $key" 2>/dev/null || \
    python -c "import socket;s=socket.socket(socket.AF_UNIX);s.connect('$SOCK');s.send(b'sendkey $key\n');print(s.recv(1024).decode());s.close()" 2>/dev/null
}

# Try Python-based QMP/HMP
python -c "
import socket, time
s = socket.socket(socket.AF_UNIX)
s.connect('$SOCK')
# Read initial banner
time.sleep(0.2)
try: s.recv(4096)
except: pass
# Send keys for 'ping 10.0.2.2\n'
for c in 'ping 10.0.2.2\r':
    s.send(f'sendkey {c}\n'.encode())
    time.sleep(0.05)
s.close()
"

sleep 5
kill $QEMU_PID 2>/dev/null
sleep 1
kill -9 $QEMU_PID 2>/dev/null

echo "=== Serial output ==="
cat build/serial.log
