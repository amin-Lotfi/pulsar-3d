# pulsar-3ddd

اسکریپت `run.sh` معماری را خودکار تشخیص می‌دهد و SDK مناسب را انتخاب می‌کند:

- `x86_64` → `Galaxy_camera_amd/lib/x86_64`
- `aarch64` (Jetson Nano) → `Galaxy_camera_arm64/lib/armv8`

## اجرا

```bash
./run.sh
```

نکته: روی `aarch64` (Jetson) اسکریپت به‌صورت پیش‌فرض با حالت `single` اجرا می‌شود.  
اگر محیط گرافیکی فعال نباشد (`DISPLAY`/`WAYLAND_DISPLAY` خالی باشد) یا OpenCV از طریق `pkg-config` پیدا نشود، به‌صورت خودکار `--no-display` اضافه می‌شود.

## اجرای Jetson Nano (ARM64) بدون دیلی

قبل از اولین اجرا روی Jetson، SDK را یک‌بار نصب کنید تا فایل‌های سیستمی (`/etc/Galaxy/...`) و دسترسی دستگاه‌ها تنظیم شود:

```bash
cd Galaxy_camera_arm64
sudo ./Galaxy_camera.run
sudo reboot
```

بعد از ریبوت:

```bash
sudo nvpmodel -m 0
sudo jetson_clocks
sudo ./Galaxy_camera_arm64/SetUSBStack.sh
./run.sh --ultra-low-latency
```

> اگر دوربین GigE دارید، برای throughput بالا اسکریپت زیر هم مفید است:

```bash
sudo ./Galaxy_camera_arm64/SetSocketBufferSize.sh
```

## مسیر SDK سفارشی

اگر SDK را جای دیگری نصب کرده‌اید:

```bash
GALAXY_SDK_ROOT=/path/to/Galaxy_camera_arm64 ./run.sh
```
