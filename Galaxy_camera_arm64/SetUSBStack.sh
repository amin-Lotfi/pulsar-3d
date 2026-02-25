#!/bin/sh
# Auto-Configure USBFS Memory (Run with sudo)

TARGET=1000
SYS_PATH="/sys/module/usbcore/parameters/usbfs_memory_mb"
SERVICE_NAME="usbfs_auto.service"

# Function: Safe value reader
get_current()
{
    local val
    val=$(cat "$SYS_PATH" 2>/dev/null)
    if expr "$val" : '^[0-9]\+$' >/dev/null; then
        echo "$val"
    else
        echo "$TARGET"
    fi
}

# Get current value
current=$(get_current)

# Apply temporary modification if needed
if [ "$current" -le $TARGET ]; then
    if ! echo $TARGET | sudo tee "$SYS_PATH" >/dev/null; then
        echo "ERROR: usbfs_memory_mb set failed (check permissions)"
        exit 1
    fi
    echo "SUCCESS: usbfs_memory_mb set to $TARGET"
else
	echo "INFO: usbfs_memory_mb is $current"
fi

sudo systemctl stop $SERVICE_NAME 2>/dev/null
sudo systemctl disable $SERVICE_NAME 2>/dev/null
sudo rm -f /etc/systemd/system/$SERVICE_NAME 2>/dev/null

# Create service
sudo bash -c "cat > /etc/systemd/system/$SERVICE_NAME" <<EOF
[Unit]
Description=USBFS Memory Auto-Configuration
After=sysinit.target

[Service]
Type=oneshot
ExecStartPre=/bin/sleep 3
ExecStart=/bin/bash -c 'current_val=\$(cat /sys/module/usbcore/parameters/usbfs_memory_mb 2>/dev/null); if [[ \"\$current_val\" =~ ^[0-9]+$ && \"\$current_val\" -le 1000 ]]; then echo 1000 > /sys/module/usbcore/parameters/usbfs_memory_mb; fi'
TimeoutSec=5
SuccessExitStatus=0
StandardOutput=syslog
StandardError=syslog
SyslogIdentifier=$SERVICE_NAME

[Install]
WantedBy=multi-user.target
EOF

# Enable service
sudo systemctl daemon-reload
sudo systemctl enable --now $SERVICE_NAME 2>/dev/null
