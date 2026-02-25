// -------------------------------------------------------------
// Dual camera sync + side-by-side display sample
// -------------------------------------------------------------

#include "GxIAPI.h"
#include "DxImageProc.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef USE_OPENCV
#define USE_OPENCV 1
#endif

#if USE_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace {

std::atomic<bool> g_run{true};

void SignalHandler(int) {
    g_run.store(false, std::memory_order_relaxed);
}

struct Options {
    std::string sn0;
    std::string sn1;
    std::string sync_mode = "free"; // free | external | action | scheduled
    std::string trigger_source = "Line1";
    std::string broadcast_ip = "255.255.255.255";
    std::string special_ip;
    int buffers = 2;
    int fps = 30;
    int max_delta_us = 2000;
    int action_lead_ms = 5;
    int packet_size = 0; // GEV only, 0 = do not set
    bool display = true;
    bool verbose = false;
};

void PrintUsage(const char* argv0) {
    std::printf("Usage: %s [options]\n", argv0);
    std::printf("\nOptions:\n");
    std::printf("  --sn0 <SN>              Serial number of camera 0\n");
    std::printf("  --sn1 <SN>              Serial number of camera 1\n");
    std::printf("  --sync <mode>           free | external | action | scheduled (default: free)\n");
    std::printf("  --trigger-source <val>  TriggerSource for external mode (default: Line1)\n");
    std::printf("  --fps <n>               Trigger FPS for action/scheduled (default: 30)\n");
    std::printf("  --max-delta-us <n>       Max timestamp delta for sync check (default: 2000, 0=disable)\n");
    std::printf("  --buffers <n>           Acquisition buffer count (default: 2)\n");
    std::printf("  --packet-size <n>        GEV packet size (e.g., 8192). 0 = no change\n");
    std::printf("  --broadcast <ip>         Action command broadcast IP (default: 255.255.255.255)\n");
    std::printf("  --special-ip <ip>        Action command source IP (default: empty)\n");
    std::printf("  --no-display             Run without GUI\n");
    std::printf("  --verbose                Extra logs\n");
    std::printf("  --help                   Show this help\n");
}

bool ParseArgs(int argc, char** argv, Options* opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("Missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--sn0") {
            const char* v = need_value("--sn0");
            if (!v) return false;
            opt->sn0 = v;
        } else if (arg == "--sn1") {
            const char* v = need_value("--sn1");
            if (!v) return false;
            opt->sn1 = v;
        } else if (arg == "--sync") {
            const char* v = need_value("--sync");
            if (!v) return false;
            opt->sync_mode = v;
        } else if (arg == "--trigger-source") {
            const char* v = need_value("--trigger-source");
            if (!v) return false;
            opt->trigger_source = v;
        } else if (arg == "--fps") {
            const char* v = need_value("--fps");
            if (!v) return false;
            opt->fps = std::atoi(v);
        } else if (arg == "--max-delta-us") {
            const char* v = need_value("--max-delta-us");
            if (!v) return false;
            opt->max_delta_us = std::atoi(v);
        } else if (arg == "--buffers") {
            const char* v = need_value("--buffers");
            if (!v) return false;
            opt->buffers = std::atoi(v);
        } else if (arg == "--packet-size") {
            const char* v = need_value("--packet-size");
            if (!v) return false;
            opt->packet_size = std::atoi(v);
        } else if (arg == "--broadcast") {
            const char* v = need_value("--broadcast");
            if (!v) return false;
            opt->broadcast_ip = v;
        } else if (arg == "--special-ip") {
            const char* v = need_value("--special-ip");
            if (!v) return false;
            opt->special_ip = v;
        } else if (arg == "--no-display") {
            opt->display = false;
        } else if (arg == "--verbose") {
            opt->verbose = true;
        } else if (arg == "--help") {
            return false;
        } else {
            std::printf("Unknown arg: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

void PrintGxError(const char* msg, GX_STATUS status) {
    std::fprintf(stderr, "%s (GX_STATUS=%d)\n", msg, status);
    size_t size = 0;
    GX_STATUS st = GXGetLastError(&status, nullptr, &size);
    if (st != GX_STATUS_SUCCESS || size == 0) {
        return;
    }
    std::vector<char> buf(size);
    st = GXGetLastError(&status, buf.data(), &size);
    if (st == GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "%s\n", buf.data());
    }
}

bool NodeAvailable(GX_DEV_HANDLE hDevice, const char* name) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    if (GXGetNodeAccessMode(hDevice, name, &mode) != GX_STATUS_SUCCESS) {
        return false;
    }
    return (mode == GX_NODE_ACCESS_MODE_RO || mode == GX_NODE_ACCESS_MODE_WO || mode == GX_NODE_ACCESS_MODE_RW);
}

bool SetEnumByString(GX_DEV_HANDLE hDevice, const char* name, const char* value) {
    GX_STATUS st = GXSetEnumValueByString(hDevice, name, value);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXSetEnumValueByString failed", st);
        return false;
    }
    return true;
}

bool SetIntValue(GX_DEV_HANDLE hDevice, const char* name, int64_t value) {
    GX_STATUS st = GXSetIntValue(hDevice, name, value);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXSetIntValue failed", st);
        return false;
    }
    return true;
}

bool SetBoolValue(GX_DEV_HANDLE hDevice, const char* name, bool value) {
    GX_STATUS st = GXSetBoolValue(hDevice, name, value);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXSetBoolValue failed", st);
        return false;
    }
    return true;
}

bool GetIntValue(GX_DEV_HANDLE hDevice, const char* name, int64_t* out) {
    GX_INT_VALUE val{};
    GX_STATUS st = GXGetIntValue(hDevice, name, &val);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXGetIntValue failed", st);
        return false;
    }
    *out = val.nCurValue;
    return true;
}

bool GetEnumValue(GX_DEV_HANDLE hDevice, const char* name, GX_ENUM_VALUE* out) {
    GX_STATUS st = GXGetEnumValue(hDevice, name, out);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXGetEnumValue failed", st);
        return false;
    }
    return true;
}

bool EnumHasSymbolic(const GX_ENUM_VALUE& ev, const char* sym) {
    for (uint32_t i = 0; i < ev.nSupportedNum; ++i) {
        if (std::strcmp(ev.nArrySupportedValue[i].strCurSymbolic, sym) == 0) {
            return true;
        }
    }
    return false;
}

bool TrySetEnumPreferred(GX_DEV_HANDLE hDevice, const char* name, const std::vector<std::string>& prefs, std::string* chosen) {
    GX_ENUM_VALUE ev{};
    if (!GetEnumValue(hDevice, name, &ev)) {
        return false;
    }
    for (const auto& p : prefs) {
        if (EnumHasSymbolic(ev, p.c_str())) {
            if (SetEnumByString(hDevice, name, p.c_str())) {
                if (chosen) *chosen = p;
                return true;
            }
        }
    }
    return false;
}

std::string GetDeviceSN(const GX_DEVICE_INFO& info) {
    switch (info.emDevType) {
        case GX_DEVICE_CLASS_GEV:
            return reinterpret_cast<const char*>(info.DevInfo.stGEVDevInfo.chSerialNumber);
        case GX_DEVICE_CLASS_U3V:
            return reinterpret_cast<const char*>(info.DevInfo.stU3VDevInfo.chSerialNumber);
        case GX_DEVICE_CLASS_USB2:
            return reinterpret_cast<const char*>(info.DevInfo.stUSBDevInfo.chSerialNumber);
        case GX_DEVICE_CLASS_CXP:
            return reinterpret_cast<const char*>(info.DevInfo.stCXPDevInfo.chSerialNumber);
        default:
            return std::string();
    }
}

struct FrameSlot {
    std::atomic<uint32_t> seq{0};
    std::vector<uint8_t> bgr;
    int width = 0;
    int height = 0;
    uint64_t timestamp = 0;
    uint64_t frame_id = 0;
};

struct CamContext {
    GX_DEV_HANDLE dev = nullptr;
    GX_DS_HANDLE ds = nullptr;
    GX_DEVICE_CLASS dev_type = GX_DEVICE_CLASS_UNKNOWN;
    uint32_t payload = 0;
    DX_IMAGE_FORMAT_CONVERT_HANDLE cvt = nullptr;
    int out_size = 0;
    FrameSlot slots[2];
    std::atomic<int> latest_idx{-1};
    std::atomic<bool> running{false};
    std::thread th;
    uint64_t tick_freq = 0;
    std::string sn;
};

bool OpenDeviceBySN(const std::string& sn, GX_DEV_HANDLE* out) {
    GX_OPEN_PARAM param{};
    param.openMode = GX_OPEN_SN;
    param.accessMode = GX_ACCESS_EXCLUSIVE;
    param.pszContent = const_cast<char*>(sn.c_str());
    GX_STATUS st = GXOpenDevice(&param, out);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXOpenDevice failed", st);
        return false;
    }
    return true;
}

bool OpenDeviceByIndex(int index, GX_DEV_HANDLE* out) {
    char buf[32] = {0};
    std::snprintf(buf, sizeof(buf), "%d", index);
    GX_OPEN_PARAM param{};
    param.openMode = GX_OPEN_INDEX;
    param.accessMode = GX_ACCESS_EXCLUSIVE;
    param.pszContent = buf;
    GX_STATUS st = GXOpenDevice(&param, out);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXOpenDevice failed", st);
        return false;
    }
    return true;
}

bool SetupCamera(CamContext* cam, const Options& opt) {
    if (!SetEnumByString(cam->dev, "AcquisitionMode", "Continuous")) {
        return false;
    }

    if (opt.sync_mode == "free") {
        SetEnumByString(cam->dev, "TriggerMode", "Off");
    } else {
        SetEnumByString(cam->dev, "TriggerMode", "On");
        if (opt.sync_mode == "action" || opt.sync_mode == "scheduled") {
            SetEnumByString(cam->dev, "TriggerSource", "Action0");
        } else {
            SetEnumByString(cam->dev, "TriggerSource", opt.trigger_source.c_str());
        }
    }

    if (opt.buffers > 0) {
        GX_STATUS st = GXSetAcqusitionBufferNumber(cam->dev, static_cast<uint64_t>(opt.buffers));
        if (st != GX_STATUS_SUCCESS) {
            PrintGxError("GXSetAcqusitionBufferNumber failed", st);
            return false;
        }
    }

    // USB3 stream settings
    if (NodeAvailable(cam->dev, "StreamTransferSize")) {
        SetIntValue(cam->dev, "StreamTransferSize", 64 * 1024);
    }
    if (NodeAvailable(cam->dev, "StreamTransferNumberUrb")) {
        SetIntValue(cam->dev, "StreamTransferNumberUrb", 64);
    }

    // GEV packet size (optional)
    if (opt.packet_size > 0 && NodeAvailable(cam->dev, "GevSCPSPacketSize")) {
        SetIntValue(cam->dev, "GevSCPSPacketSize", opt.packet_size);
    }

    // Action command parameters if needed
    if (opt.sync_mode == "action" || opt.sync_mode == "scheduled") {
        if (NodeAvailable(cam->dev, "ActionDeviceKey")) {
            SetIntValue(cam->dev, "ActionDeviceKey", 1);
        }
        if (NodeAvailable(cam->dev, "ActionGroupKey")) {
            SetIntValue(cam->dev, "ActionGroupKey", 1);
        }
        if (NodeAvailable(cam->dev, "ActionGroupMask")) {
            SetIntValue(cam->dev, "ActionGroupMask", 0xFFFFFFFF);
        }
    }

    // Prefer 8-bit pixel formats for speed
    bool is_color = NodeAvailable(cam->dev, "PixelColorFilter");
    std::vector<std::string> prefs;
    if (is_color) {
        prefs = {"BayerRG8", "BayerBG8", "BayerGR8", "BayerGB8", "RGB8", "BGR8"};
    } else {
        prefs = {"Mono8"};
    }
    std::string chosen;
    TrySetEnumPreferred(cam->dev, "PixelFormat", prefs, &chosen);

    // Stream handle + payload
    uint32_t nDSNum = 0;
    if (GXGetDataStreamNumFromDev(cam->dev, &nDSNum) != GX_STATUS_SUCCESS || nDSNum < 1) {
        std::fprintf(stderr, "Failed to get data stream number\n");
        return false;
    }
    if (GXGetDataStreamHandleFromDev(cam->dev, 1, &cam->ds) != GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "Failed to get data stream handle\n");
        return false;
    }
    if (GXGetPayLoadSize(cam->ds, &cam->payload) != GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "Failed to get payload size\n");
        return false;
    }

    // Timestamp frequency
    if (NodeAvailable(cam->dev, "TimestampTickFrequency")) {
        int64_t tf = 0;
        if (GetIntValue(cam->dev, "TimestampTickFrequency", &tf)) {
            cam->tick_freq = static_cast<uint64_t>(tf);
        }
    }

    // Setup converter
    if (DxImageFormatConvertCreate(&cam->cvt) != DX_OK) {
        std::fprintf(stderr, "DxImageFormatConvertCreate failed\n");
        return false;
    }
    DxImageFormatConvertSetOutputPixelFormat(cam->cvt, GX_PIXEL_FORMAT_BGR8);
    DxImageFormatConvertSetInterpolationType(cam->cvt, RAW2RGB_NEIGHBOUR);

    int64_t width = 0;
    int64_t height = 0;
    if (!GetIntValue(cam->dev, "Width", &width) || !GetIntValue(cam->dev, "Height", &height)) {
        std::fprintf(stderr, "Failed to get width/height\n");
        return false;
    }

    int out_size = 0;
    if (DxImageFormatConvertGetBufferSizeForConversion(cam->cvt, GX_PIXEL_FORMAT_BGR8,
            static_cast<VxUint32>(width), static_cast<VxUint32>(height), &out_size) != DX_OK) {
        out_size = static_cast<int>(width * height * 3);
    }
    cam->out_size = out_size;

    for (auto& slot : cam->slots) {
        slot.bgr.resize(static_cast<size_t>(out_size));
        slot.width = static_cast<int>(width);
        slot.height = static_cast<int>(height);
    }

    return true;
}

void CaptureLoop(CamContext* cam, bool verbose) {
    cam->running.store(true, std::memory_order_release);

    while (g_run.load(std::memory_order_relaxed)) {
        PGX_FRAME_BUFFER frame = nullptr;
        GX_STATUS st = GXDQBuf(cam->dev, &frame, 1000);
        if (st == GX_STATUS_TIMEOUT) {
            continue;
        }
        if (st != GX_STATUS_SUCCESS) {
            PrintGxError("GXDQBuf failed", st);
            break;
        }
        if (frame && frame->nStatus == GX_FRAME_STATUS_SUCCESS) {
            int write_idx = (cam->latest_idx.load(std::memory_order_relaxed) + 1) & 1;
            FrameSlot& slot = cam->slots[write_idx];

            uint32_t seq = slot.seq.load(std::memory_order_relaxed);
            slot.seq.store(seq + 1, std::memory_order_release); // begin write

            // Convert to BGR
            VxInt32 dx = DxImageFormatConvert(cam->cvt,
                                              frame->pImgBuf,
                                              frame->nImgSize,
                                              slot.bgr.data(),
                                              cam->out_size,
                                              static_cast<GX_PIXEL_FORMAT_ENTRY>(frame->nPixelFormat),
                                              static_cast<VxUint32>(frame->nWidth),
                                              static_cast<VxUint32>(frame->nHeight),
                                              false);
            if (dx == DX_OK) {
                slot.width = frame->nWidth;
                slot.height = frame->nHeight;
                slot.timestamp = frame->nTimestamp;
                slot.frame_id = frame->nFrameID;
                slot.seq.store(seq + 2, std::memory_order_release); // end write
                cam->latest_idx.store(write_idx, std::memory_order_release);
            } else {
                slot.seq.store(seq + 2, std::memory_order_release); // end write
                if (verbose) {
                    std::fprintf(stderr, "DxImageFormatConvert failed: %d\n", dx);
                }
            }
        }

        if (frame) {
            GXQBuf(cam->dev, frame);
        }
    }

    cam->running.store(false, std::memory_order_release);
}

struct FrameMeta {
    int width = 0;
    int height = 0;
    uint64_t timestamp = 0;
    uint64_t frame_id = 0;
};

bool CopySlotToBuffer(const FrameSlot& slot, uint8_t* dest, int dest_stride, FrameMeta* meta) {
    for (;;) {
        uint32_t s1 = slot.seq.load(std::memory_order_acquire);
        if (s1 & 1U) {
            continue;
        }
        int w = slot.width;
        int h = slot.height;
        if (w <= 0 || h <= 0) {
            return false;
        }
        const uint8_t* src = slot.bgr.data();
        const int row_bytes = w * 3;
        for (int y = 0; y < h; ++y) {
            std::memcpy(dest + y * dest_stride, src + y * row_bytes, static_cast<size_t>(row_bytes));
        }
        uint32_t s2 = slot.seq.load(std::memory_order_acquire);
        if (s1 == s2) {
            if (meta) {
                meta->width = w;
                meta->height = h;
                meta->timestamp = slot.timestamp;
                meta->frame_id = slot.frame_id;
            }
            return true;
        }
    }
}

bool EnablePtpAndWait(const std::vector<CamContext*>& cams, int timeout_sec) {
    for (auto* cam : cams) {
        if (!NodeAvailable(cam->dev, "PtpEnable")) {
            std::fprintf(stderr, "PTP not supported on one of the cameras\n");
            return false;
        }
        if (!SetBoolValue(cam->dev, "PtpEnable", true)) {
            return false;
        }
    }

    int elapsed = 0;
    while (elapsed < timeout_sec) {
        GX_ENUM_VALUE ev{};
        if (!GetEnumValue(cams[0]->dev, "PtpStatus", &ev)) {
            return false;
        }
        const char* status = ev.stCurValue.strCurSymbolic;
        if (std::strcmp(status, "Master") == 0 || std::strcmp(status, "Slave") == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++elapsed;
    }
    std::fprintf(stderr, "PTP role assignment timeout\n");
    return false;
}

uint64_t GetTimestampLatch(GX_DEV_HANDLE dev) {
    GXSetCommandValue(dev, "TimestampLatch");
    int64_t ts = 0;
    if (!GetIntValue(dev, "TimestampLatchValue", &ts)) {
        return 0;
    }
    return static_cast<uint64_t>(ts);
}

void ActionTriggerLoop(const Options& opt, const std::vector<CamContext*>& cams, bool scheduled) {
    if (opt.fps <= 0) {
        std::fprintf(stderr, "Invalid fps\n");
        return;
    }
    const uint32_t device_key = 1;
    const uint32_t group_key = 1;
    const uint32_t group_mask = 0xFFFFFFFF;

    const auto period = std::chrono::duration<double>(1.0 / static_cast<double>(opt.fps));
    const uint64_t lead_ns = static_cast<uint64_t>(opt.action_lead_ms) * 1000000ULL;

    while (g_run.load(std::memory_order_relaxed)) {
        if (scheduled) {
            uint64_t ts = GetTimestampLatch(cams[0]->dev);
            if (ts == 0) {
                std::this_thread::sleep_for(period);
                continue;
            }
            uint64_t action_time = ts + lead_ns;
            GXGigEIssueScheduledActionCommand(device_key, group_key, group_mask,
                                              action_time,
                                              opt.broadcast_ip.c_str(),
                                              opt.special_ip.empty() ? "" : opt.special_ip.c_str(),
                                              0, nullptr, nullptr);
        } else {
            GXGigEIssueActionCommand(device_key, group_key, group_mask,
                                     opt.broadcast_ip.c_str(),
                                     opt.special_ip.empty() ? "" : opt.special_ip.c_str(),
                                     0, nullptr, nullptr);
        }
        std::this_thread::sleep_for(period);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, &opt)) {
        PrintUsage(argv[0]);
        return 1;
    }

#if !USE_OPENCV
    opt.display = false;
#endif

    std::signal(SIGINT, SignalHandler);

    GX_STATUS st = GXInitLib();
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXInitLib failed", st);
        return 1;
    }

    uint32_t dev_num = 0;
    st = GXUpdateAllDeviceList(&dev_num, 1000);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXUpdateAllDeviceList failed", st);
        GXCloseLib();
        return 1;
    }
    if (dev_num < 2) {
        std::fprintf(stderr, "Need at least 2 cameras, found %u\n", dev_num);
        GXCloseLib();
        return 1;
    }

    std::vector<GX_DEVICE_INFO> infos;
    infos.reserve(dev_num);
    for (uint32_t i = 1; i <= dev_num; ++i) {
        GX_DEVICE_INFO info{};
        if (GXGetDeviceInfo(i, &info) == GX_STATUS_SUCCESS) {
            infos.push_back(info);
        }
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
            std::string sn = GetDeviceSN(info);
            if (!opt.sn0.empty() && sn == opt.sn0) {
                idx0 = static_cast<int>(i);
            }
            if (!opt.sn1.empty() && sn == opt.sn1) {
                idx1 = static_cast<int>(i);
            }
        }
        if (idx0 < 0 || idx1 < 0) {
            std::fprintf(stderr, "Failed to match SNs to device indices\n");
            GXCloseLib();
            return 1;
        }
    }

    GX_DEVICE_INFO info0{};
    GX_DEVICE_INFO info1{};
    if (GXGetDeviceInfo(static_cast<uint32_t>(idx0), &info0) != GX_STATUS_SUCCESS ||
        GXGetDeviceInfo(static_cast<uint32_t>(idx1), &info1) != GX_STATUS_SUCCESS) {
        std::fprintf(stderr, \"Failed to get device info\\n\");
        GXCloseLib();
        return 1;
    }

    CamContext cam0;
    CamContext cam1;
    cam0.dev_type = info0.emDevType;
    cam1.dev_type = info1.emDevType;
    cam0.sn = GetDeviceSN(info0);
    cam1.sn = GetDeviceSN(info1);

    if (!opt.sn0.empty()) {
        if (!OpenDeviceBySN(opt.sn0, &cam0.dev)) {
            GXCloseLib();
            return 1;
        }
    } else {
        if (!OpenDeviceByIndex(idx0, &cam0.dev)) {
            GXCloseLib();
            return 1;
        }
    }

    if (!opt.sn1.empty()) {
        if (!OpenDeviceBySN(opt.sn1, &cam1.dev)) {
            GXCloseDevice(cam0.dev);
            GXCloseLib();
            return 1;
        }
    } else {
        if (!OpenDeviceByIndex(idx1, &cam1.dev)) {
            GXCloseDevice(cam0.dev);
            GXCloseLib();
            return 1;
        }
    }

    if ((opt.sync_mode == \"action\" || opt.sync_mode == \"scheduled\") &&\n+        (cam0.dev_type != GX_DEVICE_CLASS_GEV || cam1.dev_type != GX_DEVICE_CLASS_GEV)) {\n+        std::fprintf(stderr, \"Action/scheduled sync requires GigE (GEV) cameras\\n\");\n+        GXCloseDevice(cam0.dev);\n+        GXCloseDevice(cam1.dev);\n+        GXCloseLib();\n+        return 1;\n+    }\n+\n+    // Setup cameras
    if (!SetupCamera(&cam0, opt) || !SetupCamera(&cam1, opt)) {
        GXCloseDevice(cam0.dev);
        GXCloseDevice(cam1.dev);
        GXCloseLib();
        return 1;
    }

    // Enable PTP for scheduled action
    if (opt.sync_mode == "scheduled") {
        std::vector<CamContext*> cams{&cam0, &cam1};
        if (!EnablePtpAndWait(cams, 8)) {
            std::fprintf(stderr, "PTP setup failed\n");
            GXCloseDevice(cam0.dev);
            GXCloseDevice(cam1.dev);
            GXCloseLib();
            return 1;
        }
    }

    if (GXStreamOn(cam0.dev) != GX_STATUS_SUCCESS || GXStreamOn(cam1.dev) != GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "GXStreamOn failed\n");
        GXCloseDevice(cam0.dev);
        GXCloseDevice(cam1.dev);
        GXCloseLib();
        return 1;
    }

    cam0.th = std::thread(CaptureLoop, &cam0, opt.verbose);
    cam1.th = std::thread(CaptureLoop, &cam1, opt.verbose);

    std::thread trigger_thread;
    if (opt.sync_mode == "action" || opt.sync_mode == "scheduled") {
        std::vector<CamContext*> cams{&cam0, &cam1};
        bool scheduled = (opt.sync_mode == "scheduled");
        trigger_thread = std::thread(ActionTriggerLoop, opt, cams, scheduled);
    }

#if USE_OPENCV
    if (opt.display) {
        cv::setNumThreads(1);
        cv::namedWindow("DualCam", cv::WINDOW_NORMAL);
    }
#endif

    if (cam0.slots[0].width != cam1.slots[0].width || cam0.slots[0].height != cam1.slots[0].height) {\n+        std::fprintf(stderr, \"Resolution mismatch: %dx%d vs %dx%d\\n\",\n+                     cam0.slots[0].width, cam0.slots[0].height,\n+                     cam1.slots[0].width, cam1.slots[0].height);\n+        g_run.store(false, std::memory_order_relaxed);\n+    }\n+\n+    const uint64_t tick_freq = cam0.tick_freq != 0 ? cam0.tick_freq : cam1.tick_freq;
    uint64_t max_delta_ticks = 0;
    if (tick_freq != 0 && opt.max_delta_us > 0) {
        max_delta_ticks = static_cast<uint64_t>(opt.max_delta_us) * tick_freq / 1000000ULL;
    }

    const int combined_stride = cam0.slots[0].width * 2 * 3;\n+    std::vector<uint8_t> combined;\n+    combined.resize(static_cast<size_t>(cam0.slots[0].width * cam0.slots[0].height * 2 * 3));

    while (g_run.load(std::memory_order_relaxed)) {
        int idx0_local = cam0.latest_idx.load(std::memory_order_acquire);
        int idx1_local = cam1.latest_idx.load(std::memory_order_acquire);
        if (idx0_local < 0 || idx1_local < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        FrameMeta m0{};
        FrameMeta m1{};

        uint8_t* left = combined.data();
        uint8_t* right = combined.data() + cam0.slots[0].width * 3;

        if (!CopySlotToBuffer(cam0.slots[idx0_local], left, combined_stride, &m0) ||
            !CopySlotToBuffer(cam1.slots[idx1_local], right, combined_stride, &m1)) {
            continue;
        }

        if (m0.width != m1.width || m0.height != m1.height) {
            std::fprintf(stderr, "Resolution mismatch: %dx%d vs %dx%d\n", m0.width, m0.height, m1.width, m1.height);
            break;
        }

        if (max_delta_ticks > 0 && (opt.sync_mode != "free")) {
            uint64_t delta = (m0.timestamp > m1.timestamp) ? (m0.timestamp - m1.timestamp) : (m1.timestamp - m0.timestamp);
            if (delta > max_delta_ticks) {
                continue;
            }
        }

#if USE_OPENCV
        if (opt.display) {
            cv::Mat view(m0.height, m0.width * 2, CV_8UC3, combined.data());
            cv::imshow("DualCam", view);
            int key = cv::waitKey(1);
            if (key == 27 || key == 'q' || key == 'Q') {
                g_run.store(false, std::memory_order_relaxed);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }

    g_run.store(false, std::memory_order_relaxed);

    if (trigger_thread.joinable()) {
        trigger_thread.join();
    }

    if (cam0.th.joinable()) cam0.th.join();
    if (cam1.th.joinable()) cam1.th.join();

    GXStreamOff(cam0.dev);
    GXStreamOff(cam1.dev);

    if (cam0.cvt) DxImageFormatConvertDestroy(cam0.cvt);
    if (cam1.cvt) DxImageFormatConvertDestroy(cam1.cvt);

    GXCloseDevice(cam0.dev);
    GXCloseDevice(cam1.dev);
    GXCloseLib();

    return 0;
}
