#include "camera_pipeline.h"
#include "display_pipeline.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_run(true);

void SignalHandler(int) {
    g_run.store(false, std::memory_order_relaxed);
}

const char* DeviceTypeToString(GX_DEVICE_CLASS type) {
    switch (type) {
        case GX_DEVICE_CLASS_USB2:
            return "USB2";
        case GX_DEVICE_CLASS_U3V:
            return "U3V";
        case GX_DEVICE_CLASS_GEV:
            return "GEV";
        case GX_DEVICE_CLASS_CXP:
            return "CXP";
        case GX_DEVICE_CLASS_SMART:
            return "SMART";
        default:
            return "UNKNOWN";
    }
}

const char* DeviceModel(const GX_DEVICE_INFO& info) {
    switch (info.emDevType) {
        case GX_DEVICE_CLASS_GEV:
            return reinterpret_cast<const char*>(info.DevInfo.stGEVDevInfo.chModelName);
        case GX_DEVICE_CLASS_U3V:
            return reinterpret_cast<const char*>(info.DevInfo.stU3VDevInfo.chModelName);
        case GX_DEVICE_CLASS_USB2:
            return reinterpret_cast<const char*>(info.DevInfo.stUSBDevInfo.chModelName);
        case GX_DEVICE_CLASS_CXP:
            return reinterpret_cast<const char*>(info.DevInfo.stCXPDevInfo.chModelName);
        default:
            return "";
    }
}

const char* NonEmptyOrDash(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return "-";
    }
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, &opt)) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string opt_error;
    if (!ValidateOptions(&opt, &opt_error)) {
        std::fprintf(stderr, "%s\n", opt_error.c_str());
        PrintUsage(argv[0]);
        return 1;
    }

    if (opt.sync_mode == "free" && opt.strict_pair && opt.verbose) {
        std::fprintf(stderr,
                     "Warning: --sync free uses software pairing only. For medical-grade sync, use --sync "
                     "master or --sync external.\n");
    }

    uint64_t total_mem_mb = 0;
    const bool low_mem_profile = AutoTuneForSystemMemory(&opt, &total_mem_mb);
    if (low_mem_profile && opt.verbose) {
        std::fprintf(stdout,
                     "Low-memory profile active (MemTotal=%llu MB, buffers=%d)\n",
                     static_cast<unsigned long long>(total_mem_mb),
                     opt.buffers);
    }

    if (opt.ultra_low_latency) {
        if (opt.buffers > 1) {
            opt.buffers = 1;
        }

        if (setpriority(PRIO_PROCESS, 0, -10) != 0 && opt.verbose) {
            std::fprintf(stderr, "setpriority failed: %s\n", std::strerror(errno));
        }

        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0 && opt.verbose) {
            std::fprintf(stderr, "mlockall failed: %s\n", std::strerror(errno));
        }
    }

    if (opt.display && !DisplayBackendAvailable()) {
        std::fprintf(stderr, "OpenCV not found at compile time. Rebuild with OpenCV or use --no-display.\n");
        return 1;
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    GX_STATUS st = GXInitLib();
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXInitLib failed", st);
        return 1;
    }

    constexpr uint32_t kDiscoveryTimeoutMs = 1500;
    constexpr int kDiscoveryRetries = 3;
    constexpr int kRetrySleepMs = 500;
    uint32_t dev_num = 0;
    for (int attempt = 1; attempt <= kDiscoveryRetries; ++attempt) {
        st = GXUpdateAllDeviceList(&dev_num, kDiscoveryTimeoutMs);
        if (st != GX_STATUS_SUCCESS) {
            PrintGxError("GXUpdateAllDeviceList failed", st);
            GXCloseLib();
            return 1;
        }
        if (dev_num >= 2) {
            break;
        }
        if (attempt < kDiscoveryRetries && opt.verbose) {
            std::fprintf(stderr,
                         "Camera discovery attempt %d/%d: found %u camera(s), retrying...\n",
                         attempt,
                         kDiscoveryRetries,
                         dev_num);
        }
        if (attempt < kDiscoveryRetries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetrySleepMs));
        }
    }
    if (dev_num < 2) {
        std::fprintf(stderr, "Need at least 2 cameras, found %u\n", dev_num);
        if (dev_num > 0) {
            std::fprintf(stderr, "Detected devices:\n");
            for (uint32_t i = 1; i <= dev_num; ++i) {
                GX_DEVICE_INFO info{};
                if (GXGetDeviceInfo(i, &info) != GX_STATUS_SUCCESS) {
                    std::fprintf(stderr, "  [%u] <failed to read info>\n", i);
                    continue;
                }
                const std::string sn = GetDeviceSN(info);
                std::fprintf(stderr,
                             "  [%u] type=%s model=%s sn=%s\n",
                             i,
                             DeviceTypeToString(info.emDevType),
                             NonEmptyOrDash(DeviceModel(info)),
                             NonEmptyOrDash(sn.c_str()));
            }
        }
        GXCloseLib();
        return 1;
    }

    int idx0 = 1;
    int idx1 = 2;
    if (!opt.sn0.empty() || !opt.sn1.empty()) {
        idx0 = -1;
        idx1 = -1;
        for (uint32_t i = 1; i <= dev_num; ++i) {
            GX_DEVICE_INFO info{};
            if (GXGetDeviceInfo(i, &info) != GX_STATUS_SUCCESS) {
                continue;
            }
            const std::string sn = GetDeviceSN(info);
            if (!opt.sn0.empty() && sn == opt.sn0) {
                idx0 = static_cast<int>(i);
            }
            if (!opt.sn1.empty() && sn == opt.sn1) {
                idx1 = static_cast<int>(i);
            }
        }
        if (idx0 < 0 || idx1 < 0 || idx0 == idx1) {
            std::fprintf(stderr, "Could not map serial numbers to two distinct camera indices\n");
            GXCloseLib();
            return 1;
        }
    }

    GX_DEVICE_INFO info0{};
    GX_DEVICE_INFO info1{};
    if (GXGetDeviceInfo(static_cast<uint32_t>(idx0), &info0) != GX_STATUS_SUCCESS ||
        GXGetDeviceInfo(static_cast<uint32_t>(idx1), &info1) != GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "GXGetDeviceInfo failed\n");
        GXCloseLib();
        return 1;
    }

    CamContext cam0;
    CamContext cam1;
    cam0.index = 0;
    cam1.index = 1;
    cam0.dev_type = info0.emDevType;
    cam1.dev_type = info1.emDevType;
    cam0.sn = GetDeviceSN(info0);
    cam1.sn = GetDeviceSN(info1);

    auto cleanup_and_fail = [&](int code) {
        StopAndClose(&cam0, &cam1);
        GXCloseLib();
        return code;
    };

    if (!opt.sn0.empty()) {
        if (!OpenDeviceBySN(opt.sn0, &cam0.dev)) {
            return cleanup_and_fail(1);
        }
    } else if (!OpenDeviceByIndex(idx0, &cam0.dev)) {
        return cleanup_and_fail(1);
    }

    if (!opt.sn1.empty()) {
        if (!OpenDeviceBySN(opt.sn1, &cam1.dev)) {
            return cleanup_and_fail(1);
        }
    } else if (!OpenDeviceByIndex(idx1, &cam1.dev)) {
        return cleanup_and_fail(1);
    }

    if ((opt.sync_mode == "action" || opt.sync_mode == "scheduled") &&
        (cam0.dev_type != GX_DEVICE_CLASS_GEV || cam1.dev_type != GX_DEVICE_CLASS_GEV)) {
        std::fprintf(stderr, "Action/scheduled sync requires two GigE cameras\n");
        return cleanup_and_fail(1);
    }

    int preferred_width = 0;
    int preferred_height = 0;
    if (opt.display_mode == "dual") {
        preferred_width = std::max(1, std::min(opt.mon0_width, opt.mon1_width / 2));
        preferred_height = std::max(1, std::min(opt.mon0_height, opt.mon1_height));
    } else {
        preferred_width = std::max(1, opt.mon0_width / 2);
        preferred_height = std::max(1, opt.mon0_height);
    }

    if (!ForceSameResolution(&cam0, &cam1, preferred_width, preferred_height, opt.verbose)) {
        return cleanup_and_fail(1);
    }

    if (!SetupCamera(&cam0, opt) || !SetupCamera(&cam1, opt)) {
        return cleanup_and_fail(1);
    }

    if (!ForceSameImageSettings(&cam0, &cam1, opt, opt.verbose)) {
        return cleanup_and_fail(1);
    }

    if (opt.sync_mode == "scheduled") {
        std::vector<CamContext*> cams{&cam0, &cam1};
        if (!EnablePtpAndWait(cams, 8)) {
            return cleanup_and_fail(1);
        }
    }

    if (GXStreamOn(cam0.dev) != GX_STATUS_SUCCESS || GXStreamOn(cam1.dev) != GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "GXStreamOn failed\n");
        return cleanup_and_fail(1);
    }

    cam0.th = std::thread(CaptureLoop, &cam0, opt.verbose, opt.ultra_low_latency, &g_run);
    cam1.th = std::thread(CaptureLoop, &cam1, opt.verbose, opt.ultra_low_latency, &g_run);

    std::thread trigger_thread;
    if (opt.sync_mode == "action" || opt.sync_mode == "scheduled") {
        std::vector<CamContext*> cams{&cam0, &cam1};
        const bool scheduled = (opt.sync_mode == "scheduled");
        trigger_thread = std::thread(ActionTriggerLoop, opt, cams, scheduled, &g_run);
    }

    RunDisplayLoop(opt, &cam0, &cam1, &g_run);

    g_run.store(false, std::memory_order_relaxed);

    if (trigger_thread.joinable()) {
        trigger_thread.join();
    }
    if (cam0.th.joinable()) {
        cam0.th.join();
    }
    if (cam1.th.joinable()) {
        cam1.th.join();
    }

    StopAndClose(&cam0, &cam1);
    GXCloseLib();
    return 0;
}
