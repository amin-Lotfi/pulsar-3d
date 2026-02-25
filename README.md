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

قبل از اجرای `pulsar/main`، اسکریپت حالا با `camera_probe` اتصال دوربین‌ها را چک می‌کند و تعداد/نوع/سریال دوربین‌های کشف‌شده را چاپ می‌کند.

اگر خواستید موقتا precheck را رد کنید:

```bash
PULSAR_SKIP_CAMERA_CHECK=1 ./run.sh
```

پارامترهای precheck (اختیاری):

- `PULSAR_MIN_CAMERAS` (پیش‌فرض: `2`)
- `PULSAR_CAM_DISCOVERY_TIMEOUT_MS` (پیش‌فرض: `1500`)
- `PULSAR_CAM_DISCOVERY_RETRIES` (پیش‌فرض: `3`)
- `PULSAR_CAM_DISCOVERY_RETRY_SLEEP_MS` (پیش‌فرض: `500`)

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

## رفع خطای `Need at least 2 cameras, found 0`

طبق README رسمی SDK:

1. نصب سیستمی SDK باید یک‌بار با دسترسی root انجام شود:

```bash
sudo ./Galaxy_camera_arm64/Galaxy_camera.run
sudo reboot
```

2. بعد از نصب، دوربین USB را یک بار جدا/وصل کنید.
3. اگر هنوز دوربین USB کشف نمی‌شود، `SetUSBStack.sh` را با `sudo` اجرا کنید.
4. برای دوربین GigE، فایروال روی کارت شبکه دوربین را خاموش کنید و `rp_filter` را بررسی کنید.
5. طبق Linux SDK FAQ، اگر فایل `99-galaxy-u3v.rules` در `/etc/udev/rules.d/` نباشد، کشف دوربین USB ممکن است فقط با `sudo` کار کند.

نمونه اعمال udev rule:

```bash
sudo cp ./Galaxy_camera_arm64/config/99-galaxy-u3v.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

نکته Jetson (از Linux SDK FAQ): روی Jetson Nano/TX2 با Linux kernel `4.9` (نسخه‌های قدیمی R32.2.3)، اگر خطای thread creation مکرر دارید، ممکن است لازم باشد:

```bash
sudo rm -f /etc/security/limits.d/galaxy-limits.conf
sudo reboot
```

## مسیر SDK سفارشی

اگر SDK را جای دیگری نصب کرده‌اید:

```bash
GALAXY_SDK_ROOT=/path/to/Galaxy_camera_arm64 ./run.sh
```
