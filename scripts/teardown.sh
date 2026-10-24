#!/bin/bash
# DS5Dummy — Teardown script
# Removes every DS5Dummy USB gadget, unmounts each per-controller FunctionFS
# mount, and unloads dummy_hcd.
#
# Usage: sudo ./teardown.sh
#
# The Manager names each gadget ds5dummy-<12 hex MAC digits> and mounts its
# FunctionFS at /dev/ffs-ds5.<the same digits>, one pair per served controller.
# The sweep below therefore matches the whole family rather than one fixed name,
# so a crashed run that left three stations behind is cleaned in one pass.

set -euo pipefail

log() { echo "[teardown] $*"; }

if [ "$(id -u)" -ne 0 ]; then
    echo "[teardown] ERROR: Must be run as root (sudo)." >&2
    exit 1
fi

# --- Remove every gadget (if DS5Dummy didn't clean up) ---
removed=0
for gadget in /sys/kernel/config/usb_gadget/ds5dummy \
              /sys/kernel/config/usb_gadget/ds5dummy-*; do
    [ -d "$gadget" ] || continue
    removed=$((removed + 1))
    log "Removing USB gadget $(basename "$gadget")..."

    # Unbind UDC: configfs spells "unbind" as writing an empty line.
    if [ -f "$gadget/UDC" ]; then
        echo "" > "$gadget/UDC" 2>/dev/null || true
    fi

    # Drop the function symlink inside each configuration before rmdir: configfs
    # refuses to remove a configuration (and then the gadget) while a function
    # is still linked into it.
    for link in "$gadget"/configs/*/*; do
        [ -L "$link" ] && rm -f "$link"
    done

    rm -rf "$gadget/configs" 2>/dev/null || true
    rm -rf "$gadget/functions" 2>/dev/null || true
    rm -rf "$gadget/strings" 2>/dev/null || true
    rmdir "$gadget" 2>/dev/null || true
done
if [ "$removed" -eq 0 ]; then
    log "No gadget found (already cleaned up)."
fi

# --- Unmount every per-controller FunctionFS mount ---
for mount_point in /dev/ffs-ds5 /dev/ffs-ds5.*; do
    [ -e "$mount_point" ] || continue
    if mountpoint -q "$mount_point" 2>/dev/null; then
        log "Unmounting FunctionFS at $mount_point..."
        umount "$mount_point" 2>/dev/null || true
    fi
    # Mount points are directories the daemon created; drop them once unmounted.
    rmdir "$mount_point" 2>/dev/null || true
done

# --- Unload dummy_hcd ---
if ls /sys/class/udc/dummy_udc* &>/dev/null; then
    log "Unloading dummy_hcd..."
    rmmod dummy_hcd 2>/dev/null || true
    log "dummy_hcd unloaded."
else
    log "dummy_hcd not loaded."
fi

log "Teardown complete."
