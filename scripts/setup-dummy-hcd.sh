#!/bin/bash
set -euo pipefail

# DS5Dummy — Setup dummy_hcd and FunctionFS mount

FFS_MOUNT="/dev/ffs-ds5"

# Load dummy_hcd if not already loaded
if ! lsmod | grep -q dummy_hcd; then
    echo "[setup] Loading dummy_hcd..."
    sudo modprobe dummy_hcd is_high_speed=0
    sleep 0.5
fi
echo "[setup] dummy_hcd loaded."

# Check for UDC
if ! ls /sys/class/udc/dummy_udc.* &>/dev/null; then
    echo "[setup] ERROR: dummy_udc not found. dummy_hcd may not be loaded correctly."
    exit 1
fi
echo "[setup] UDC found: $(ls /sys/class/udc/dummy_udc.*)"

# Mount FunctionFS
if [ ! -d "$FFS_MOUNT" ]; then
    echo "[setup] Creating FunctionFS mount at $FFS_MOUNT..."
    sudo mkdir -p "$FFS_MOUNT"
fi

if ! mountpoint -q "$FFS_MOUNT" 2>/dev/null; then
    echo "[setup] Mounting FunctionFS..."
    sudo mount -t functionfs ds5dummy "$FFS_MOUNT" -o "uid=$(id -u),gid=$(id -g)"
fi

echo "[setup] FunctionFS mounted at $FFS_MOUNT"
ls -la "$FFS_MOUNT/"
echo "[setup] Ready. Run: sudo ./build/mini_gadget   (no-BT gadget probe; build with -DDS5DUMMY_BUILD_TOOLS=ON)"
