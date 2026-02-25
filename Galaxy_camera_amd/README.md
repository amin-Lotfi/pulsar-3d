<div align="center">

# Pulsar 3D
### Dual-Camera Realtime Pipeline (Daheng / Galaxy SDK)

<p>
  <img src="https://img.shields.io/badge/Status-Active-22c55e?style=for-the-badge" alt="status" />
  <img src="https://img.shields.io/badge/Language-C%2B%2B17-2563eb?style=for-the-badge" alt="c++17" />
  <img src="https://img.shields.io/badge/Focus-Realtime%20Imaging-f59e0b?style=for-the-badge" alt="realtime" />
  <img src="https://img.shields.io/badge/Domain-Medical%20Vision-ef4444?style=for-the-badge" alt="medical" />
</p>

<p>
  <a href="#english"><img src="https://img.shields.io/badge/Read-English-1f6feb?style=for-the-badge" alt="English" /></a>
  <a href="#persian-toggle"><img src="https://img.shields.io/badge/Read-فارسی-0ea5e9?style=for-the-badge" alt="Persian" /></a>
</p>

</div>

---

<a id="english"></a>
## English

## What Is Pulsar?
`pulsar` is a high-performance dual-camera capture pipeline for Daheng cameras using Galaxy SDK.
It is designed for **low-latency, deterministic behavior**, and **stable synchronization** in sensitive realtime use-cases.

## Core Capabilities
- Dual-camera concurrent acquisition.
- Side-by-side live output.
- Multiple sync modes:
  - `free`
  - `external`
  - `master` (recommended for USB/U3V dual-cam setups)
  - `action` / `scheduled` (GigE only)
- Runtime frame pairing using `frame_id` and `timestamp`.
- Unsynced pair dropping with configurable threshold (`--max-delta-us`).
- Color channel correction path (R/B swap fix when needed).
- Low-memory safeguards for systems around 2GB RAM.

## Project Structure (Pulsar)
```text
pulsar/
  main.cpp                 # app orchestration
  settings.h/.cpp          # CLI options, validation, memory profile
  camera_pipeline.h/.cpp   # camera setup, streaming, capture, trigger loops
  display_pipeline.h/.cpp  # pairing logic, display/no-display realtime loop
```

## Build
```bash
./run.sh
```

## Run (Help)
```bash
./run.sh --help
```

## Common Run Profiles
### 1) USB/U3V dual camera with line-based sync
```bash
./run.sh --sync master --trigger-source Line2 --fps 60 --buffers 2 --max-delta-us 2000 --verbose
```

### 2) External shared trigger source
```bash
./run.sh --sync external --trigger-source Line2 --buffers 2 --max-delta-us 1500 --verbose
```

### 3) Low-memory / headless realtime processing
```bash
./run.sh --no-display --sync master --trigger-source Line2 --fps 60 --buffers 2 --verbose
```

## Realtime Design Notes
- Capture and display are decoupled.
- Lock-free slot snapshot pattern with atomic sequence markers.
- New-pair-only rendering in display loop.
- Drift-aware trigger pacing in action loop (`sleep_until`).
- OpenCV thread count forced to 1 for more predictable timing.

## Memory Strategy
- Default stream buffers are intentionally short (`buffers=2`).
- Input buffers are clamped to a safe range.
- Auto low-memory profile reads `/proc/meminfo`.
- On small-memory systems, queue depth is limited to reduce latency spikes.
- Buffers are explicitly released on shutdown.

## Safety Notice
This repository targets realtime imaging workflows, but by itself it is **not a certified medical device**.
For clinical usage, you should validate end-to-end latency, long-run stability, and error/fallback handling on target hardware.

---

<a id="persian-toggle"></a>
<details>
<summary><strong>🇮🇷 فارسی (برای باز کردن کلیک کنید)</strong></summary>

## فارسی

### Pulsar چیست؟
`pulsar` یک پایپ‌لاین سریع و پایدار برای تصویربرداری همزمان با دو دوربین Daheng (Galaxy SDK) است که برای سناریوهای realtime طراحی شده است.

### قابلیت‌های اصلی
- دریافت همزمان از دو دوربین.
- نمایش زنده کنار هم.
- پشتیبانی از حالت‌های سینک:
  - `free`
  - `external`
  - `master` (مناسب USB/U3V)
  - `action` / `scheduled` (فقط GigE)
- جفت‌سازی فریم‌ها با `frame_id` و `timestamp`.
- حذف خودکار جفت‌فریم‌های ناسینک.
- اصلاح مشکل جابجایی رنگ قرمز/آبی در مسیرهای لازم.
- مدیریت حافظه برای سیستم‌های کم‌رم (حدود ۲ گیگ).

### ساختار پروژه (بخش pulsar)
```text
pulsar/
  main.cpp
  settings.h/.cpp
  camera_pipeline.h/.cpp
  display_pipeline.h/.cpp
```

### ساخت و اجرا
```bash
./run.sh
./run.sh --help
```

### پروفایل‌های پیشنهادی
#### USB/U3V با سینک line-based
```bash
./run.sh --sync master --trigger-source Line2 --fps 60 --buffers 2 --max-delta-us 2000 --verbose
```

#### Trigger خارجی مشترک
```bash
./run.sh --sync external --trigger-source Line2 --buffers 2 --max-delta-us 1500 --verbose
```

#### حالت کم‌رم / بدون نمایش
```bash
./run.sh --no-display --sync master --trigger-source Line2 --fps 60 --buffers 2 --verbose
```

### نکات مهم realtime
- capture و display از هم جدا هستند.
- حلقه نمایش فقط جفت‌فریم جدید را پردازش می‌کند.
- فریم ناسینک حذف می‌شود تا لگ جمع نشود.
- بافرها کوتاه نگه داشته می‌شوند تا تاخیر کم بماند.

### هشدار
این پروژه برای realtime مناسب است اما به‌تنهایی Medical Device Certificated نیست. برای استفاده بالینی، تست latency و پایداری روی سخت‌افزار واقعی ضروری است.

</details>
