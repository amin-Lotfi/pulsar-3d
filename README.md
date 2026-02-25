# pulsar-3d

اسکریپت `run.sh` حالا معماری را به‌صورت خودکار تشخیص می‌دهد و SDK مناسب را انتخاب می‌کند:

- `x86_64` → `Galaxy_camera_amd/lib/x86_64`
- `aarch64` (Jetson Nano) → `Galaxy_camera_arm64/lib/armv8`

## اجرا

```bash
./run.sh
```

## اجرای سریع روی Jetson Nano (ARM64)

برای کمترین تأخیر:

```bash
sudo nvpmodel -m 0
sudo jetson_clocks
./run.sh --ultra-low-latency
```

اگر SDK را جای دیگری نصب کرده‌اید، مسیر را override کنید:

```bash
GALAXY_SDK_ROOT=/path/to/Galaxy_camera_arm64 ./run.sh
```
