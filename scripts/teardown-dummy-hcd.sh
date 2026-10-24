#!/bin/bash
set -euo pipefail

# DS5Dummy — Teardown dummy_hcd and FunctionFS mount

FFS_MOUNT="/dev/ffs-ds5"

# Unmount FunctionFS
if mountpoint -q "$FFS_MOUNT" 2>/dev/null; then
    echo "[teardown] Unmounting FunctionFS..."
    sudo umount "$FFS_MOUNT"
fi

# Remove dummy_hcd
if lsmod | grep -q dummy_hcd; then
    echo "[teardown] Removing dummy_hcd..."
    sudo rmmod dummy_hcd
fi

echo "[teardown] Done."
