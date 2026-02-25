# GxDualCamOpen

Minimal dual-camera USB3 test for Daheng Galaxy SDK.

## What it does

- Initializes `GxIAPI` (`GXInitLib`).
- Enumerates devices (`GXUpdateAllDeviceList`).
- Opens 2 cameras (prefers U3V devices).
- Sets:
  - `AcquisitionMode=Continuous`
  - `TriggerMode=Off`
  - `StreamTransferSize=65536` (if available)
  - `StreamTransferNumberUrb=64` (if available)
  - `StreamBufferHandlingMode=NewestOnly` (if available)
- Uses `GXDQAllBufs + GXQAllBufs` to drain queue and keep low-latency streaming.
- Starts both streams, grabs for N seconds, prints FPS and summary.

## Build and run

From `Galaxy_camera`:

```bash
./run_dual_cam_test.sh 10
```

`10` is the capture duration in seconds.

Note: the script checks architecture and should be run on `aarch64` target host.

## If cameras are not detected

1. Check USB vendor visibility:

```bash
lsusb | grep -i 2ba2
```

2. Ensure udev rule exists:

```bash
cat /etc/udev/rules.d/99-galaxy-u3v.rules
```

3. Reload rules and replug cameras:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
```

4. Confirm USB stack memory (`>=1000` is recommended for multi-camera):

```bash
cat /sys/module/usbcore/parameters/usbfs_memory_mb
```

If needed:

```bash
cd Galaxy_camera
./SetUSBStack.sh
```
