#!/bin/bash
# DS5Dummy — Full setup script
# Sets up dummy_hcd, FunctionFS, and prepares for DS5Dummy.
#
# Usage: sudo ./setup.sh
#
# Prerequisites:
#   - dummy_hcd kernel module (CONFIG_USB_DUMMY_HCD=m)
#   - configfs mounted at /sys/kernel/config
#   - DualSense paired and connected via Bluetooth
#
# IMPORTANT: The kernel's hid-playstation driver MUST NOT be bound to
# the BT DualSense. This script will attempt to unbind it.

set -euo pipefail

log() { echo "[setup] $*"; }
err() { echo "[setup] ERROR: $*" >&2; }

# --- Check root ---
if [ "$(id -u)" -ne 0 ]; then
    err "This script must be run as root (sudo)."
    exit 1
fi

# --- Check kernel module ---
log "Checking for dummy_hcd module..."
if ! modinfo dummy_hcd &>/dev/null; then
    err "dummy_hcd module not found."
    err "Install it with: sudo modprobe dummy_hcd"
    err "If not available, enable CONFIG_USB_DUMMY_HCD in your kernel config."
    exit 1
fi

# --- Load dummy_hcd ---
if ! ls /sys/class/udc/dummy_udc* &>/dev/null; then
    log "Loading dummy_hcd..."
    modprobe dummy_hcd is_high_speed=0
    sleep 0.5
else
    log "dummy_hcd already loaded."
fi

# --- Load snd-usb-audio (for UAC1 gadget audio) ---
if ! lsmod | grep -q snd_usb_audio; then
    log "Loading snd-usb-audio module..."
    modprobe snd-usb-audio 2>/dev/null || log "WARNING: snd-usb-audio not available (audio will be disabled)"
else
    log "snd-usb-audio already loaded."
fi

# --- Load libcomposite (for configfs gadget functions) ---
if ! lsmod | grep -q libcomposite; then
    log "Loading libcomposite..."
    modprobe libcomposite 2>/dev/null || log "WARNING: libcomposite not available"
else
    log "libcomposite already loaded."
fi

# --- FunctionFS is mounted by the daemon, one gadget per controller ---
# The Manager mounts <ffs_path>.<12 hex MAC digits> with its own gadget instance
# as the mount source, so there is no fixed mount point to prepare here.

# --- Handle hid-playstation conflict ---
log "Checking for hid-playstation driver conflict..."
if lsmod | grep -q hid_playstation; then
    log "hid-playstation is loaded. Attempting to unbind..."

    # Find all hidraw devices and check if any are DualSense
    for dev in /sys/class/hidraw/hidraw*; do
        [ -e "$dev" ] || continue
        devname=$(basename "$dev")

        # Read VID/PID from uevent
        if [ -f "$dev/device/uevent" ]; then
            hid_id=$(grep '^HID_ID=' "$dev/device/uevent" 2>/dev/null || true)
            if echo "$hid_id" | grep -qi '054c:0ce6'; then
                log "Found DualSense at $devname"

                # Check if hid-playstation is bound
                driver_link="$dev/device/driver"
                if [ -L "$driver_link" ]; then
                    driver_name=$(basename "$(readlink -f "$driver_link")")
                    if [ "$driver_name" = "hid-playstation" ]; then
                        log "Unbinding hid-playstation from $devname..."
                        # Get the HID device path
                        hid_dev=$(readlink -f "$dev/device")
                        echo "$(basename "$hid_dev")" > "$driver_link/unbind" 2>/dev/null || true
                        log "Unbound. Note: hidraw device may no longer be available."
                        log "You may need to reconnect the DualSense via Bluetooth."
                    fi
                fi
            fi
        fi
    done

    log ""
    log "IMPORTANT: To prevent hid-playstation from auto-binding on reconnect,"
    log "add this to /etc/modprobe.d/ds5dummy.conf:"
    log "  blacklist hid-playstation"
    log ""
    log "Then reload: sudo rmmod hid-playstation && sudo modprobe dummy_hcd"
fi

# --- Ensure configfs is mounted ---
if ! mountpoint -q /sys/kernel/config 2>/dev/null; then
    log "Mounting configfs..."
    mount -t configfs none /sys/kernel/config
fi

log ""
log "Setup complete!"
log ""
log "Next steps:"
log "  1. Make sure your DualSense is connected via Bluetooth"
log "  2. Run: sudo ./ds5dummyd"
log ""
log "To teardown: sudo ./teardown.sh"
